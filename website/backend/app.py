import base64, hashlib, hmac, json, os, secrets, threading, time
import requests
from cryptography.fernet import Fernet
from flask import Flask, Response, jsonify, request
from flask_cors import CORS
from werkzeug.middleware.proxy_fix import ProxyFix

ADMIN_PASSWORD = os.environ["ADMIN_PASSWORD"]
SECRET_KEY = os.environ["SECRET_KEY"]
DATABASE_URL = os.getenv("DATABASE_URL")        # Neon (keeps data after restarts)
DEFAULT_CONTENT_URL = os.getenv(
    "DEFAULT_CONTENT_URL", "https://buxinelectronics-art.github.io/Ruju"
).rstrip("/")
GITHUB_TOKEN = os.getenv("GITHUB_TOKEN", "")
GITHUB_REPO = os.getenv("GITHUB_REPO", "buxinelectronics-art/Ruju")
GITHUB_BRANCH = os.getenv("GITHUB_BRANCH", "main")
GITHUB_ALLOWED = ("docs/adhan/", "docs/huruf/", "docs/hadith/", "docs/qa/")
fernet = Fernet(base64.urlsafe_b64encode(hashlib.sha256(SECRET_KEY.encode()).digest()))

app = Flask(__name__)
app.config["MAX_CONTENT_LENGTH"] = 6 * 1024 * 1024   # Adhan MP3 uploads
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

METHODS = ("MWL", "ISNA", "Egypt", "UmmAlQura", "Karachi")
ASR_OPTIONS = ("Shafi", "Hanafi")
HIGH_LAT = ("None", "MidNight", "OneSeventh", "AngleBased")
ADJ_KEYS = ("adj_fajr", "adj_sunrise", "adj_dhuhr", "adj_asr", "adj_maghrib", "adj_isha")
PRAYER_KEYS = ("prayer_fajr", "prayer_dhuhr", "prayer_asr", "prayer_maghrib", "prayer_isha")

DEVICE_DEFAULTS = {
    "content_url": DEFAULT_CONTENT_URL,
    "timezone": "GMT0",
    "latitude": 21.4225,
    "longitude": 39.8262,
    "method": "MWL",
    "asr": "Shafi",
    "high_lat": "None",
    "adj_fajr": 0,
    "adj_sunrise": 0,
    "adj_dhuhr": 0,
    "adj_asr": 0,
    "adj_maghrib": 0,
    "adj_isha": 0,
    "prayer_fajr": True,
    "prayer_dhuhr": True,
    "prayer_asr": True,
    "prayer_maghrib": True,
    "prayer_isha": True,
    "volume": -1,
    "reciter": -1,
    "config_version": 1,
}

# ---------- storage: one JSON document in Neon, or a local file ----------
def database(sql, args=()):
    import psycopg
    with psycopg.connect(DATABASE_URL) as c:
        c.execute("CREATE TABLE IF NOT EXISTS store (id INT PRIMARY KEY, data TEXT NOT NULL)")
        cur = c.execute(sql, args)
        return cur.fetchone() if cur.description else None

def normalize_device(raw):
    if not isinstance(raw, dict):
        raw = {}
    out = {**DEVICE_DEFAULTS}
    out["name"] = str(raw.get("name", ""))[:60]
    out["hash"] = str(raw.get("hash", ""))
    for key, default in DEVICE_DEFAULTS.items():
        if key not in raw:
            continue
        out[key] = raw[key]
    # keep types sane even if old data is messy
    out["content_url"] = str(out.get("content_url") or DEFAULT_CONTENT_URL).rstrip("/")[:200]
    out["timezone"] = str(out.get("timezone") or "GMT0")[:40]
    try:
        out["latitude"] = float(out["latitude"])
        out["longitude"] = float(out["longitude"])
    except (TypeError, ValueError):
        out["latitude"], out["longitude"] = DEVICE_DEFAULTS["latitude"], DEVICE_DEFAULTS["longitude"]
    if out.get("method") not in METHODS:
        out["method"] = "MWL"
    if out.get("asr") not in ASR_OPTIONS:
        out["asr"] = "Shafi"
    if out.get("high_lat") not in HIGH_LAT:
        out["high_lat"] = "None"
    for key in ADJ_KEYS:
        try:
            out[key] = max(-90, min(90, int(out[key])))
        except (TypeError, ValueError):
            out[key] = 0
    for key in PRAYER_KEYS:
        out[key] = bool(out.get(key, True))
    try:
        out["volume"] = int(out.get("volume", -1))
    except (TypeError, ValueError):
        out["volume"] = -1
    try:
        out["reciter"] = int(out.get("reciter", -1))
    except (TypeError, ValueError):
        out["reciter"] = -1
    try:
        out["config_version"] = max(1, int(out.get("config_version", 1)))
    except (TypeError, ValueError):
        out["config_version"] = 1
    return out

def load():
    if DATABASE_URL:
        row = database("SELECT data FROM store WHERE id=1")
        text = row[0] if row else None
    else:
        text = open("data.json").read() if os.path.exists("data.json") else None
    saved = json.loads(text) if text else {}
    devices = {pid: normalize_device(dev) for pid, dev in (saved.get("devices") or {}).items()}
    return {"key": saved.get("key", ""), "settings": {**SETTINGS, **saved.get("settings", {})},
            "devices": devices}

def save():
    text = json.dumps(data)
    if DATABASE_URL:
        database("INSERT INTO store VALUES (1,%s) ON CONFLICT (id) DO UPDATE SET data=EXCLUDED.data", (text,))
    else:
        open("data.json", "w").write(text)

def validate_device_settings(d):
    out = {}
    url = str(d.get("content_url", "")).strip().rstrip("/")
    if not url.startswith("https://") or len(url) > 200:
        return None, "content_url must be https://... (max 200 chars)"
    out["content_url"] = url

    tz = str(d.get("timezone", "")).strip()
    if not tz or len(tz) > 40 or any(c in tz for c in " \t\r\n\"'"):
        return None, "timezone must be a POSIX string (no spaces), max 40 chars"
    out["timezone"] = tz

    try:
        lat = float(d.get("latitude"))
        lon = float(d.get("longitude"))
    except (TypeError, ValueError):
        return None, "latitude and longitude must be numbers"
    if not (-90.0 <= lat <= 90.0 and -180.0 <= lon <= 180.0):
        return None, "latitude/longitude out of range"
    out["latitude"], out["longitude"] = lat, lon

    method = str(d.get("method", "")).strip()
    if method not in METHODS:
        return None, "method must be one of: " + ", ".join(METHODS)
    out["method"] = method

    asr = str(d.get("asr", "")).strip()
    if asr not in ASR_OPTIONS:
        return None, "asr must be Shafi or Hanafi"
    out["asr"] = asr

    high = str(d.get("high_lat", "")).strip()
    if high not in HIGH_LAT:
        return None, "high_lat must be one of: " + ", ".join(HIGH_LAT)
    out["high_lat"] = high

    for key in ADJ_KEYS:
        try:
            val = int(d.get(key, 0))
        except (TypeError, ValueError):
            return None, key + " must be an integer minute adjustment"
        if val < -90 or val > 90:
            return None, key + " must be between -90 and 90"
        out[key] = val

    for key in PRAYER_KEYS:
        out[key] = bool(d.get(key, True))

    if "volume" in d and d.get("volume") is not None and str(d.get("volume")).strip() != "":
        try:
            vol = int(d.get("volume"))
        except (TypeError, ValueError):
            return None, "volume must be -1 or 5..100"
        if vol != -1 and not (5 <= vol <= 100):
            return None, "volume must be -1 or 5..100"
        out["volume"] = vol

    if "reciter" in d and d.get("reciter") is not None and str(d.get("reciter")).strip() != "":
        try:
            rec = int(d.get("reciter"))
        except (TypeError, ValueError):
            return None, "reciter must be -1 or 0..3"
        if rec != -1 and not (0 <= rec <= 3):
            return None, "reciter must be -1 or 0..3"
        out["reciter"] = rec

    return out, None

def device_public(pid, d, online):
    return {
        "id": pid,
        "name": d["name"],
        "online": online,
        "content_url": d["content_url"],
        "timezone": d["timezone"],
        "latitude": d["latitude"],
        "longitude": d["longitude"],
        "method": d["method"],
        "asr": d["asr"],
        "high_lat": d["high_lat"],
        "adj_fajr": d["adj_fajr"],
        "adj_sunrise": d["adj_sunrise"],
        "adj_dhuhr": d["adj_dhuhr"],
        "adj_asr": d["adj_asr"],
        "adj_maghrib": d["adj_maghrib"],
        "adj_isha": d["adj_isha"],
        "prayer_fajr": d["prayer_fajr"],
        "prayer_dhuhr": d["prayer_dhuhr"],
        "prayer_asr": d["prayer_asr"],
        "prayer_maghrib": d["prayer_maghrib"],
        "prayer_isha": d["prayer_isha"],
        "volume": d["volume"],
        "reciter": d["reciter"],
        "config_version": d["config_version"],
    }

def hello_payload(d):
    return {
        "config_version": d["config_version"],
        "volume": d["volume"],
        "reciter": d["reciter"],
        "content_url": d["content_url"],
        "timezone": d["timezone"],
        "latitude": d["latitude"],
        "longitude": d["longitude"],
        "method": d["method"],
        "asr": d["asr"],
        "high_lat": d["high_lat"],
        "adj_fajr": d["adj_fajr"],
        "adj_sunrise": d["adj_sunrise"],
        "adj_dhuhr": d["adj_dhuhr"],
        "adj_asr": d["adj_asr"],
        "adj_maghrib": d["adj_maghrib"],
        "adj_isha": d["adj_isha"],
        "prayer_fajr": 1 if d["prayer_fajr"] else 0,
        "prayer_dhuhr": 1 if d["prayer_dhuhr"] else 0,
        "prayer_asr": 1 if d["prayer_asr"] else 0,
        "prayer_maghrib": 1 if d["prayer_maghrib"] else 0,
        "prayer_isha": 1 if d["prayer_isha"] else 0,
    }

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
    devices = [device_public(i, d, time.time() - last_seen.get(i, 0) < 660)
               for i, d in data["devices"].items()]
    return jsonify(has_key=bool(data["key"]), settings=data["settings"], devices=devices,
                   methods=list(METHODS), asr_options=list(ASR_OPTIONS), high_lat=list(HIGH_LAT))

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
        data["devices"][pid] = normalize_device({
            **DEVICE_DEFAULTS,
            "name": name,
            "hash": hashlib.sha256(token.encode()).hexdigest(),
        })
        save()
    return jsonify(product_id=pid, token=token)                      # token shown once

@app.post("/api/admin/devices/<pid>/settings")
def device_settings(pid):
    if pid not in data["devices"]:
        return error("Unknown device", 404)
    cleaned, err = validate_device_settings(body())
    if err:
        return error(err, 400)
    with lock:
        dev = data["devices"][pid]
        for key, value in cleaned.items():
            dev[key] = value
        dev["config_version"] = int(dev.get("config_version", 1)) + 1
        data["devices"][pid] = normalize_device(dev)
        save()
        out = device_public(pid, data["devices"][pid], time.time() - last_seen.get(pid, 0) < 660)
    return jsonify(ok=True, device=out)

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

# ---------- GitHub Pages content (docs/ only) ----------
def github_configured():
    return bool(GITHUB_TOKEN and GITHUB_REPO and GITHUB_BRANCH)

def github_headers():
    if not GITHUB_TOKEN:
        raise RuntimeError("GITHUB_TOKEN not set on server")
    return {
        "Authorization": "Bearer " + GITHUB_TOKEN,
        "Accept": "application/vnd.github+json",
        "X-GitHub-Api-Version": "2022-11-28",
    }

def github_assert_path(path):
    path = path.replace("\\", "/").lstrip("/")
    if ".." in path.split("/") or not path.startswith(GITHUB_ALLOWED):
        raise ValueError("path not allowed")
    return path

def github_get_file(path):
    path = github_assert_path(path)
    url = f"https://api.github.com/repos/{GITHUB_REPO}/contents/{path}"
    r = requests.get(url, headers=github_headers(), params={"ref": GITHUB_BRANCH}, timeout=40)
    if r.status_code == 404:
        return None
    if not r.ok:
        raise RuntimeError(f"GitHub read failed ({r.status_code})")
    return r.json()

def github_put_file(path, data_bytes, message):
    path = github_assert_path(path)
    if not isinstance(data_bytes, (bytes, bytearray)) or not data_bytes:
        raise ValueError("empty file")
    existing = github_get_file(path)
    payload = {
        "message": message,
        "content": base64.b64encode(data_bytes).decode("ascii"),
        "branch": GITHUB_BRANCH,
    }
    if existing and existing.get("sha"):
        payload["sha"] = existing["sha"]
    url = f"https://api.github.com/repos/{GITHUB_REPO}/contents/{path}"
    r = requests.put(url, headers=github_headers(), json=payload, timeout=60)
    if not r.ok:
        raise RuntimeError(f"GitHub write failed ({r.status_code}): {r.text[:200]}")
    return r.json()

def looks_like_mp3(data):
    if len(data) < 4:
        return False
    if data[:3] == b"ID3":
        return True
    # MPEG frame sync
    return data[0] == 0xFF and (data[1] & 0xE0) == 0xE0

@app.get("/api/admin/content/adhan")
def adhan_status():
    if not github_configured():
        return error("Set GITHUB_TOKEN, GITHUB_REPO and GITHUB_BRANCH on Render", 503)
    out = {}
    for name in ("adhan.mp3", "fajr.mp3"):
        path = "docs/adhan/" + name
        try:
            info = github_get_file(path)
        except Exception as e:
            return error(str(e), 502)
        out[name] = None if not info else {
            "path": path,
            "size": info.get("size", 0),
            "sha": info.get("sha", "")[:12],
            "url": info.get("download_url") or info.get("html_url"),
        }
    return jsonify(files=out, repo=GITHUB_REPO, branch=GITHUB_BRANCH)

@app.post("/api/admin/content/adhan")
def adhan_upload():
    if not github_configured():
        return error("Set GITHUB_TOKEN, GITHUB_REPO and GITHUB_BRANCH on Render", 503)
    kind = str(request.form.get("kind", "")).strip().lower()
    if kind not in ("adhan", "fajr"):
        return error("kind must be adhan or fajr", 400)
    f = request.files.get("file")
    if not f:
        return error("Choose an MP3 file", 400)
    raw = f.read()
    if len(raw) < 1000:
        return error("File is too small", 400)
    if len(raw) > 5 * 1024 * 1024:
        return error("File is too large (max 5 MB)", 400)
    if not looks_like_mp3(raw):
        return error("File does not look like an MP3", 400)
    name = kind + ".mp3"
    path = "docs/adhan/" + name
    try:
        github_put_file(path, raw, f"Update {path}")
    except ValueError as e:
        return error(str(e), 400)
    except Exception as e:
        return error(str(e), 502)
    return jsonify(ok=True, path=path, size=len(raw))

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
    with lock:
        data["devices"][pid] = normalize_device(data["devices"][pid])
        payload = hello_payload(data["devices"][pid])
    return jsonify(payload)

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
