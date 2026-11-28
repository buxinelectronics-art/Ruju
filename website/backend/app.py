import base64, hashlib, hmac, json, os, secrets, threading, time
import requests
from cryptography.fernet import Fernet
from flask import Flask, Response, jsonify, request
from flask_cors import CORS
from werkzeug.middleware.proxy_fix import ProxyFix

ADMIN_PASSWORD = os.environ["ADMIN_PASSWORD"]
SECRET_KEY = os.environ["SECRET_KEY"]
DATABASE_URL = os.getenv("DATABASE_URL")        # Neon (keeps data after restarts)
fernet = Fernet(base64.urlsafe_b64encode(hashlib.sha256(SECRET_KEY.encode()).digest()))

app = Flask(__name__)
app.config["MAX_CONTENT_LENGTH"] = 400 * 1024
app.wsgi_app = ProxyFix(app.wsgi_app, x_for=1)
app.json.ensure_ascii = False
CORS(app, resources={r"/api/admin.*": {"origins": os.getenv("FRONTEND_ORIGIN", "*").rstrip("/")}})

SETTINGS = {
    "chat_model": "gpt-5.4-mini",
    "stt_model": "gpt-4o-mini-transcribe",
    "tts_model": "gpt-4o-mini-tts",
    "voice": "alloy",
    "instructions": "You are Deen AI, a religious question assistant. Focus on Islam; you may also answer "
    "questions about Christianity and religion. If a question is not about religion, reply exactly: "
    "I can only answer religious questions. A greeting may get a short greeting. Answer in simple English, "
    "at most 60 words, plain text. Never invent verses or hadith. For personal rulings, advise asking a scholar.",
}

# ---------- storage: one JSON document in Neon, or a local file ----------
def database(sql, args=()):
    import psycopg
    with psycopg.connect(DATABASE_URL) as c:
        c.execute("CREATE TABLE IF NOT EXISTS store (id INT PRIMARY KEY, data TEXT NOT NULL)")
        cur = c.execute(sql, args)
        return cur.fetchone() if cur.description else None

def load():
    if DATABASE_URL:
        row = database("SELECT data FROM store WHERE id=1")
        text = row[0] if row else None
    else:
        text = open("data.json").read() if os.path.exists("data.json") else None
    saved = json.loads(text) if text else {}
    return {"key": saved.get("key", ""), "settings": {**SETTINGS, **saved.get("settings", {})},
            "devices": saved.get("devices", {})}

def save():
    text = json.dumps(data)
    if DATABASE_URL:
        database("INSERT INTO store VALUES (1,%s) ON CONFLICT (id) DO UPDATE SET data=EXCLUDED.data", (text,))
    else:
        open("data.json", "w").write(text)

data = load()
lock = threading.Lock()
hits, last_seen, audio_store = {}, {}, {}

def too_many(key, limit, seconds):
    now = time.time()
    recent = [t for t in hits.get(key, []) if now - t < seconds]
    hits[key] = recent + [now]
    return len(recent) >= limit

def error(message, code):
    return jsonify(error=message), code

# ---------- OpenAI ----------
class AIError(Exception):
    pass

def openai(path, **kw):
    if not data["key"]:
        raise AIError("No API key saved")
    key = fernet.decrypt(data["key"].encode()).decode()
    try:
        r = requests.post("https://api.openai.com/v1" + path, timeout=40,
                          headers={"Authorization": "Bearer " + key}, **kw)
    except requests.RequestException:
        raise AIError("OpenAI unreachable")
    if not r.ok:
        app.logger.warning("OpenAI %s %s %s", path, r.status_code, r.text[:300])
        raise AIError({401: "Wrong API key", 429: "No credit or busy"}.get(r.status_code, f"OpenAI error {r.status_code}"))
    return r

def chat(text):
    s = data["settings"]
    body = openai("/responses", json={"model": s["chat_model"], "instructions": s["instructions"],
                                      "input": text, "max_output_tokens": 1200, "store": False}).json()
    answer = "".join(p.get("text", "") for item in body.get("output", []) if item.get("type") == "message"
                     for p in item.get("content", []) if p.get("type") == "output_text").strip()
    if not answer:
        raise AIError("No answer")
    return answer[:900]

def tts(text):
    s = data["settings"]
    return openai("/audio/speech", json={"model": s["tts_model"], "voice": s["voice"],
                                         "input": text, "response_format": "mp3"}).content

@app.errorhandler(AIError)
def ai_failed(e):
    return error(str(e), 502)

@app.get("/")
def home():
    return jsonify(ok=True)

# ---------- admin dashboard ----------
@app.before_request
def admin_login():
    if request.path.startswith("/api/admin") and request.method != "OPTIONS":
        who = "login:" + str(request.remote_addr)
        if len([t for t in hits.get(who, []) if time.time() - t < 300]) >= 10:
            return error("Too many wrong passwords. Wait 5 minutes.", 429)
        if not hmac.compare_digest(request.headers.get("X-Admin-Password", "").encode(), ADMIN_PASSWORD.encode()):
            too_many(who, 10, 300)
            return error("Wrong password", 401)

def body():
    d = request.get_json(silent=True)
    return d if isinstance(d, dict) else {}

@app.get("/api/admin")
def dashboard():
    devices = [{"id": i, "name": d["name"], "online": time.time() - last_seen.get(i, 0) < 660}
               for i, d in data["devices"].items()]
    return jsonify(has_key=bool(data["key"]), settings=data["settings"], devices=devices)

@app.post("/api/admin/config")
def configure():
    d = body()
    with lock:
        key = str(d.get("api_key", "")).strip()
        if key:
            data["key"] = fernet.encrypt(key.encode()).decode()     # never sent back
        for name in SETTINGS:
            value = str(d.get(name, "")).strip()
            if value:
                data["settings"][name] = value[:4000]
        save()
    return jsonify(ok=True)

@app.post("/api/admin/devices")
def add_device():
    name = str(body().get("name", "")).strip()[:60]
    if not name:
        return error("Enter a device name", 400)
    pid, token = "QP-" + secrets.token_hex(4).upper(), secrets.token_urlsafe(32)
    with lock:
        data["devices"][pid] = {"name": name, "hash": hashlib.sha256(token.encode()).hexdigest()}
        save()
    return jsonify(product_id=pid, token=token)                      # token shown once

@app.delete("/api/admin/devices/<pid>")
def remove_device(pid):
    with lock:
        data["devices"].pop(pid, None)
        save()
    return jsonify(ok=True)

@app.post("/api/admin/test")
def test_chat():
    text = str(body().get("text", "")).strip()[:300]
    return jsonify(answer=chat(text)) if text else error("Type something", 400)

@app.post("/api/admin/speak")
def test_speak():
    text = str(body().get("text", "")).strip()[:900]
    return Response(tts(text), mimetype="audio/mpeg") if text else error("Type something", 400)

# ---------- ESP32 ----------
def device_id():
    pid = request.headers.get("X-Product-ID", "")
    token = request.headers.get("Authorization", "").removeprefix("Bearer ")
    d = data["devices"].get(pid)
    ok = d and hmac.compare_digest(d["hash"], hashlib.sha256(token.encode()).hexdigest())
    return pid if ok else None

@app.post("/api/device/hello")
def hello():
    pid = device_id()
    if not pid:
        return error("Bad device login", 401)
    last_seen[pid] = time.time()
    return jsonify(config_version=1, volume=-1, reciter=-1)

@app.post("/api/device/ask")
def ask():
    pid = device_id()
    if not pid:
        return error("Bad device login", 401)
    if too_many("min:" + pid, 10, 60) or too_many("day:" + pid, 200, 86400):
        return error("Limit reached", 429)
    audio = request.get_data()
    if audio[:4] != b"fLaC":
        return error("Bad audio", 400)
    last_seen[pid] = time.time()
    question = openai("/audio/transcriptions", data={"model": data["settings"]["stt_model"], "language": "en"},
                      files={"file": ("q.flac", audio, "audio/flac")}).json().get("text", "").strip()[:300]
    if not question:
        return error("Didn't catch that", 422)
    answer = chat(question)
    audio_id = ""
    try:
        sound = tts(answer)
        audio_id = str(int(time.time() * 1000))
        audio_store[pid] = (audio_id, sound)                         # latest answer only
    except AIError:
        pass                                                         # still show the text
    return jsonify(question=question, answer=answer, audio_id=audio_id)

@app.get("/api/device/audio/<audio_id>")
def get_audio(audio_id):
    pid = device_id()
    saved = audio_store.get(pid)
    if not pid or not saved or saved[0] != audio_id:
        return error("Not found", 404)
    return Response(saved[1], mimetype="audio/mpeg")
