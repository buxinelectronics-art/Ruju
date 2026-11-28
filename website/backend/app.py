import base64, hashlib, hmac, json, os, re, secrets, threading, time
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
GITHUB_ALLOWED = (
    "website/docs/adhan/", "website/docs/huruf/", "website/docs/hadith/",
    "website/docs/qa/", "website/docs/songs/",
)
fernet = Fernet(base64.urlsafe_b64encode(hashlib.sha256(SECRET_KEY.encode()).digest()))

app = Flask(__name__)
app.config["MAX_CONTENT_LENGTH"] = 8 * 1024 * 1024   # default; songs routes raise to 10 MB
app.wsgi_app = ProxyFix(app.wsgi_app, x_for=1)
app.json.ensure_ascii = False
CORS(app, resources={r"/api/admin.*": {"origins": os.getenv("FRONTEND_ORIGIN", "*").rstrip("/")}})

SETTINGS = {
    "chat_model": "gpt-5.4-mini",
    "stt_model": "gpt-4o-mini-transcribe",
    "tts_model": "gpt-4o-mini-tts",
    "voice": "alloy",
    "audio_provider": "github",  # github | cloudinary | s3 — new uploads only
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
    # Songs uploads may be up to 10 MB; keep other routes at 8 MB.
    if request.path.startswith("/api/admin/songs"):
        app.config["MAX_CONTENT_LENGTH"] = 10 * 1024 * 1024 + 256 * 1024
    else:
        app.config["MAX_CONTENT_LENGTH"] = 8 * 1024 * 1024
    if request.path.startswith("/api/admin") and request.method != "OPTIONS":
        who = "login:" + str(request.remote_addr)
        if len([t for t in hits.get(who, []) if time.time() - t < 300]) >= 10:
            return error("Too many wrong passwords. Wait 5 minutes.", 429)
        if not hmac.compare_digest(request.headers.get("X-Admin-Password", "").encode(), ADMIN_PASSWORD.encode()):
            too_many(who, 10, 300)
            return error("Wrong password", 401)

@app.errorhandler(413)
def too_large(_e):
    return error("File is too large for this route", 413)

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
import github_ops as gh
import storage
from storage.base import StorageError

publish_jobs = {}
publish_lock = threading.Lock()

def need_github():
    if not gh.configured():
        return error("Set GITHUB_TOKEN, GITHUB_REPO and GITHUB_BRANCH on Render", 503)
    return None

def _audio_provider(form_or_body=None):
    raw = ""
    if form_or_body is not None:
        if hasattr(form_or_body, "get"):
            raw = form_or_body.get("provider") or form_or_body.get("audio_provider") or ""
        elif isinstance(form_or_body, dict):
            raw = form_or_body.get("provider") or form_or_body.get("audio_provider") or ""
    try:
        return storage.resolve_provider(str(raw), data.get("settings"))
    except StorageError as ex:
        raise ValueError(str(ex)) from ex

def _ingest_audio(*, file_storage=None, link: str = "", provider: str,
                  docs_rel: str, object_key: str,
                  max_bytes: int = 5 * 1024 * 1024, max_kbps: int | None = None):
    """Upload a file to a provider, or validate a pasted HTTPS MP3 link."""
    link = (link or "").strip()
    if link:
        return storage.register_link(link, max_kbps=max_kbps)
    if not file_storage:
        raise StorageError("Choose an MP3 file or paste an HTTPS audio link")
    raw = file_storage.read()
    return storage.upload_bytes(
        raw, provider=provider, docs_rel=docs_rel, object_key=object_key,
        max_bytes=max_bytes, max_kbps=max_kbps,
    )

@app.get("/api/admin/storage")
def storage_status():
    from storage.base import PROVIDERS
    return jsonify(
        default=storage.default_provider(data.get("settings")),
        providers=storage.providers_status(),
        options=list(PROVIDERS),
    )

@app.post("/api/admin/storage")
def storage_set_default():
    from storage.base import PROVIDERS
    provider = str(body().get("audio_provider") or body().get("provider") or "").strip().lower()
    if provider not in PROVIDERS:
        return error("audio_provider must be github, cloudinary, or s3", 400)
    with lock:
        data["settings"]["audio_provider"] = provider
        save()
    return jsonify(ok=True, default=provider)

@app.post("/api/admin/storage/test")
def storage_test():
    provider = str(body().get("provider") or "").strip().lower()
    try:
        result = storage.test_provider(provider)
    except StorageError as ex:
        return error(str(ex), 400)
    except Exception as ex:
        return error(str(ex), 502)
    return jsonify(result)

@app.get("/api/admin/content/adhan")
def adhan_status():
    if (e := need_github()):
        return e
    try:
        media = storage.load_media()
    except Exception as ex:
        return error(str(ex), 502)
    out = {}
    for kind, name in (("adhan", "adhan.mp3"), ("fajr", "fajr.mp3")):
        path = "website/docs/adhan/" + name
        try:
            info = gh.get_file(path)
        except Exception as ex:
            return error(str(ex), 502)
        entry = (media.get("adhan") or {}).get(kind) or {}
        play = storage.public_play_url(entry, f"adhan/{name}", int(media.get("version") or 0))
        out[name] = {
            "path": path,
            "present": bool(info) or bool(entry.get("url")),
            "size": entry.get("size") or (info or {}).get("size", 0),
            "sha": ((info or {}).get("sha") or "")[:12],
            "url": play,
            "source": entry.get("provider") or ("github" if info else ""),
            "key": entry.get("key") or "",
            "github_file": bool(info),
        }
    return jsonify(files=out, media_version=media.get("version", 0),
                   default_provider=storage.default_provider(data.get("settings")))

@app.post("/api/admin/content/adhan")
def adhan_upload():
    if (e := need_github()):
        return e
    kind = str(request.form.get("kind", "")).strip().lower()
    if kind not in ("adhan", "fajr"):
        return error("kind must be adhan or fajr", 400)
    try:
        provider = _audio_provider(request.form)
        old = storage.load_media()
        old_entry = (old.get("adhan") or {}).get(kind)
        stored = _ingest_audio(
            file_storage=request.files.get("file"),
            link=str(request.form.get("link", "")),
            provider=provider,
            docs_rel=f"adhan/{kind}.mp3",
            object_key=storage.object_key_for("adhan", f"{kind}.mp3"),
            max_bytes=5 * 1024 * 1024,
            max_kbps=None,
        )
        media = dict(old)
        adhan = dict(media.get("adhan") or {})
        adhan[kind] = storage.entry_from_stored(stored)
        media["adhan"] = adhan
        storage.save_media(media, f"Update adhan/{kind} audio source")
    except (StorageError, ValueError) as ex:
        return error(str(ex), 400)
    except Exception as ex:
        return error(str(ex), 502)
    if old_entry and (old_entry.get("key") != stored.key or old_entry.get("provider") != stored.provider):
        try:
            storage.delete_stored(old_entry)
        except Exception:
            app.logger.exception("adhan old delete failed")
    return jsonify(ok=True, kind=kind, source=stored.meta(), media_version=media.get("version"))

@app.get("/api/admin/content/huruf")
def huruf_status():
    if (e := need_github()):
        return e
    try:
        media = storage.load_media()
    except Exception as ex:
        return error(str(ex), 502)
    huruf_map = media.get("huruf") or {}
    slots = []
    for i, name in enumerate(gh.HURUF_NAMES, start=1):
        stem = f"{i:02d}"
        path = f"website/docs/huruf/{stem}.mp3"
        try:
            info = gh.get_file(path)
        except Exception as ex:
            return error(str(ex), 502)
        entry = huruf_map.get(stem) or {}
        play = storage.public_play_url(entry, f"huruf/{stem}.mp3", int(media.get("version") or 0))
        slots.append({
            "n": i, "name": name, "path": path,
            "present": bool(info) or bool(entry.get("url")),
            "size": entry.get("size") or (info or {}).get("size", 0),
            "sha": ((info or {}).get("sha") or "")[:12],
            "url": play,
            "source": entry.get("provider") or ("github" if info else ""),
            "key": entry.get("key") or "",
        })
    return jsonify(slots=slots, media_version=media.get("version", 0),
                   default_provider=storage.default_provider(data.get("settings")))

@app.post("/api/admin/content/huruf")
def huruf_upload():
    if (e := need_github()):
        return e
    try:
        n = int(request.form.get("n", "0"))
    except ValueError:
        return error("n must be 1..28", 400)
    if n < 1 or n > 28:
        return error("n must be 1..28", 400)
    stem = f"{n:02d}"
    try:
        provider = _audio_provider(request.form)
        old = storage.load_media()
        old_entry = (old.get("huruf") or {}).get(stem)
        stored = _ingest_audio(
            file_storage=request.files.get("file"),
            link=str(request.form.get("link", "")),
            provider=provider,
            docs_rel=f"huruf/{stem}.mp3",
            object_key=storage.object_key_for("huruf", f"{stem}.mp3"),
            max_bytes=5 * 1024 * 1024,
            max_kbps=None,
        )
        media = dict(old)
        huruf = dict(media.get("huruf") or {})
        huruf[stem] = storage.entry_from_stored(stored)
        media["huruf"] = huruf
        storage.save_media(media, f"Update huruf/{stem} audio source")
    except (StorageError, ValueError) as ex:
        return error(str(ex), 400)
    except Exception as ex:
        return error(str(ex), 502)
    if old_entry and (old_entry.get("key") != stored.key or old_entry.get("provider") != stored.provider):
        try:
            storage.delete_stored(old_entry)
        except Exception:
            app.logger.exception("huruf old delete failed")
    return jsonify(ok=True, n=n, name=gh.HURUF_NAMES[n - 1], source=stored.meta(),
                   media_version=media.get("version"))

def _job_new(kind):
    jid = secrets.token_hex(8)
    with publish_lock:
        publish_jobs[jid] = {
            "kind": kind, "status": "running", "progress": "starting",
            "done": 0, "total": 0, "error": "", "ok": False,
        }
    return jid

def _job_set(jid, **kw):
    with publish_lock:
        if jid in publish_jobs:
            publish_jobs[jid].update(kw)

@app.get("/api/admin/jobs/<jid>")
def job_status(jid):
    with publish_lock:
        job = publish_jobs.get(jid)
    if not job:
        return error("Unknown job", 404)
    return jsonify(job)

@app.get("/api/admin/hadith")
def hadith_list():
    if (e := need_github()):
        return e
    try:
        idx = gh.load_hadith_index()
    except Exception as ex:
        return error(str(ex), 502)
    return jsonify(index=idx)

@app.post("/api/admin/hadith/preview")
def hadith_preview():
    f = request.files.get("file")
    if not f:
        return error("Choose an .xlsx file", 400)
    raw = f.read()
    if len(raw) > 2 * 1024 * 1024:
        return error("Excel file too large", 400)
    rows, errors = gh.parse_hadith_xlsx(raw)
    return jsonify(ok=not errors, rows=rows, errors=errors, count=len(rows))

def _publish_hadith(jid, rows):
    try:
        _job_set(jid, progress="loading live index", total=len(rows))
        old = gh.load_hadith_index()
        old_map = {int(it.get("number")): it for it in old.get("items", []) if "number" in it}
        provider = storage.default_provider(data.get("settings"))
        files = {}
        version = int(old.get("version") or 0) + 1
        items_meta = []
        old_audio = []
        for i, row in enumerate(rows):
            num = int(row["number"])
            stem = f"{num:03d}"
            json_obj = {"number": num, "title": row["title"], "text": row["text"], "source": row["source"]}
            prev = old_map.get(num)
            reuse = prev and prev.get("hash") == row["hash"]
            mp3_path = f"website/docs/hadith/{stem}.mp3"
            audio_meta = {}
            if reuse and prev.get("url") and prev.get("provider") == provider:
                json_obj["url"] = prev["url"]
                audio_meta = {"provider": prev.get("provider"), "key": prev.get("key") or "", "url": prev["url"]}
                if provider == "github":
                    existing = gh.get_file_bytes(mp3_path)
                    if existing:
                        files[mp3_path] = existing
                _job_set(jid, progress=f"reuse {i + 1}/{len(rows)}", done=i + 1)
            else:
                if reuse and provider == "github":
                    existing = gh.get_file_bytes(mp3_path)
                    if existing:
                        files[mp3_path] = existing
                        _job_set(jid, progress=f"reuse {i + 1}/{len(rows)}", done=i + 1)
                        reuse = True
                    else:
                        reuse = False
                if not reuse or (reuse and provider != "github" and not json_obj.get("url")):
                    reuse = False
                if not reuse:
                    _job_set(jid, progress=f"TTS {i + 1}/{len(rows)}", done=i)
                    speak = row["title"] + ". " + row["text"]
                    if row.get("source"):
                        speak += ". Source: " + row["source"]
                    for attempt in range(3):
                        try:
                            mp3 = tts(speak[:4000])
                            break
                        except Exception:
                            if attempt == 2:
                                raise
                            time.sleep(1 + attempt)
                            _job_set(jid, progress=f"TTS retry {i + 1}/{len(rows)}")
                    stored = storage.upload_bytes(
                        mp3, provider=provider, docs_rel=f"hadith/{stem}.mp3",
                        object_key=storage.object_key_for("hadith", f"{stem}.mp3"),
                        max_bytes=5 * 1024 * 1024, max_kbps=None,
                    )
                    json_obj["url"] = stored.url
                    audio_meta = stored.meta()
                    if provider == "github":
                        # put_file already wrote; keep blob in tree via get
                        existing = gh.get_file_bytes(mp3_path)
                        if existing:
                            files[mp3_path] = existing
                    if prev:
                        old_audio.append(prev)
                    _job_set(jid, progress=f"TTS done {i + 1}/{len(rows)}", done=i + 1)
            json_bytes = json.dumps(json_obj, ensure_ascii=True, separators=(",", ":")).encode()
            files[f"website/docs/hadith/{stem}.json"] = json_bytes
            meta = {"number": num, "title": row["title"], "hash": row["hash"], "file": stem}
            meta.update({k: audio_meta[k] for k in ("provider", "key", "url") if audio_meta.get(k)})
            items_meta.append(meta)
        index = {"version": version, "count": len(items_meta), "items": items_meta}
        files["website/docs/hadith/index.json"] = json.dumps(index, ensure_ascii=True, indent=2).encode()
        _job_set(jid, progress="committing to GitHub")
        sha = gh.atomic_replace("website/docs/hadith/", files, f"Publish hadith v{version} ({len(rows)} items)")
        for old_meta in old_audio:
            try:
                storage.delete_stored(old_meta)
            except Exception:
                app.logger.exception("hadith old audio delete failed")
        _job_set(jid, status="done", ok=True, progress="done", done=len(rows), commit=sha, version=version)
    except Exception as ex:
        app.logger.exception("hadith publish failed")
        _job_set(jid, status="error", error=str(ex), progress="failed")

@app.post("/api/admin/hadith/publish")
def hadith_publish():
    if (e := need_github()):
        return e
    d = body()
    rows = d.get("rows")
    if not isinstance(rows, list) or not rows:
        return error("rows required", 400)
    # re-validate lightly
    if len(rows) > 500:
        return error("Max 500 rows", 400)
    jid = _job_new("hadith")
    threading.Thread(target=_publish_hadith, args=(jid, rows), daemon=True).start()
    return jsonify(job_id=jid)

@app.delete("/api/admin/hadith")
def hadith_delete():
    if (e := need_github()):
        return e
    empty = {"version": int(time.time()), "count": 0, "items": []}
    try:
        sha = gh.atomic_replace(
            "website/docs/hadith/",
            {"website/docs/hadith/index.json": json.dumps(empty, indent=2).encode()},
            "Clear hadith list",
        )
    except Exception as ex:
        return error(str(ex), 502)
    return jsonify(ok=True, commit=sha)

@app.get("/api/admin/qa")
def qa_list():
    if (e := need_github()):
        return e
    try:
        idx = gh.load_qa_index()
    except Exception as ex:
        return error(str(ex), 502)
    return jsonify(index=idx)

@app.post("/api/admin/qa/preview")
def qa_preview():
    f = request.files.get("file")
    if not f:
        return error("Choose an .xlsx file", 400)
    raw = f.read()
    if len(raw) > 2 * 1024 * 1024:
        return error("Excel file too large", 400)
    rows, errors = gh.parse_qa_xlsx(raw)
    return jsonify(ok=not errors, rows=rows, errors=errors, count=len(rows))

def _publish_qa(jid, rows):
    try:
        _job_set(jid, progress="loading live index", total=len(rows))
        old = gh.load_qa_index()
        old_hash = {}
        for t in old.get("topics", []):
            tid = t.get("id")
            for n in range(1, int(t.get("count") or 0) + 1):
                path = f"website/docs/qa/{tid}/{n:03d}.json"
                raw = gh.get_file_bytes(path)
                if not raw:
                    continue
                obj = json.loads(raw.decode())
                h = gh.row_hash(tid, obj.get("question", ""), obj.get("answer", ""), obj.get("source", ""))
                old_hash[(tid, n)] = h
        # group by topic preserving order
        topics_order = []
        by_topic = {}
        for row in rows:
            tid = row["topic_id"]
            if tid not in by_topic:
                by_topic[tid] = []
                topics_order.append((tid, row["topic"]))
            by_topic[tid].append(row)
        files = {}
        version = int(old.get("version") or 0) + 1
        topics_meta = []
        done = 0
        for tid, tname in topics_order:
            items = by_topic[tid]
            topics_meta.append({"id": tid, "name": tname, "count": len(items)})
            for n, row in enumerate(items, start=1):
                done += 1
                json_obj = {"question": row["question"], "answer": row["answer"], "source": row["source"]}
                json_bytes = json.dumps(json_obj, ensure_ascii=True, separators=(",", ":")).encode()
                mp3_path = f"website/docs/qa/{tid}/{n:03d}.mp3"
                reuse = old_hash.get((tid, n)) == row["hash"]
                if reuse:
                    existing = gh.get_file_bytes(mp3_path)
                    if not existing:
                        reuse = False
                if reuse:
                    mp3 = existing
                    _job_set(jid, progress=f"reuse {done}/{len(rows)}", done=done)
                else:
                    _job_set(jid, progress=f"TTS {done}/{len(rows)}", done=done - 1)
                    speak = row["question"].rstrip(".?") + "? ... " + row["answer"]
                    for attempt in range(3):
                        try:
                            mp3 = tts(speak[:4000])
                            break
                        except Exception:
                            if attempt == 2:
                                raise
                            time.sleep(1 + attempt)
                    _job_set(jid, progress=f"TTS done {done}/{len(rows)}", done=done)
                files[f"website/docs/qa/{tid}/{n:03d}.json"] = json_bytes
                files[mp3_path] = mp3
        index = {"version": version, "topics": topics_meta}
        files["website/docs/qa/index.json"] = json.dumps(index, ensure_ascii=True, indent=2).encode()
        _job_set(jid, progress="committing to GitHub")
        sha = gh.atomic_replace("website/docs/qa/", files, f"Publish Q&A v{version} ({len(rows)} items)")
        _job_set(jid, status="done", ok=True, progress="done", done=len(rows), commit=sha, version=version)
    except Exception as ex:
        app.logger.exception("qa publish failed")
        _job_set(jid, status="error", error=str(ex), progress="failed")

@app.post("/api/admin/qa/publish")
def qa_publish():
    if (e := need_github()):
        return e
    rows = body().get("rows")
    if not isinstance(rows, list) or not rows:
        return error("rows required", 400)
    if len(rows) > 500:
        return error("Max 500 rows", 400)
    jid = _job_new("qa")
    threading.Thread(target=_publish_qa, args=(jid, rows), daemon=True).start()
    return jsonify(job_id=jid)

@app.delete("/api/admin/qa")
def qa_delete():
    if (e := need_github()):
        return e
    empty = {"version": int(time.time()), "topics": []}
    try:
        sha = gh.atomic_replace(
            "website/docs/qa/",
            {"website/docs/qa/index.json": json.dumps(empty, indent=2).encode()},
            "Clear Q&A list",
        )
    except Exception as ex:
        return error(str(ex), 502)
    return jsonify(ok=True, commit=sha)

# ---------- Songs (Arabic Songs) ----------
song_batches = {}

def _songs_storage_payload(stats=None):
    stats = stats or gh.website_docs_stats()
    docs = int(stats["docs_bytes"])
    return {
        "docs_bytes": docs,
        "docs_files": stats["docs_files"],
        "songs_bytes": stats["songs_bytes"],
        "warn_bytes": gh.DOCS_WARN_BYTES,
        "block_bytes": gh.DOCS_BLOCK_BYTES,
        "near_limit": docs >= gh.DOCS_WARN_BYTES,
        "blocked": docs >= gh.DOCS_BLOCK_BYTES,
    }

@app.get("/api/admin/songs")
def songs_list():
    if (e := need_github()):
        return e
    try:
        idx, songs = gh.load_songs_list()
        stats = gh.website_docs_stats()
        # per-file sizes from tree
        sizes = {}
        ref = gh.api("GET", f"git/ref/heads/{gh.GITHUB_BRANCH}")
        if ref.ok:
            commit = gh.api("GET", f"git/commits/{ref.json()['object']['sha']}")
            if commit.ok:
                tree = gh.api("GET", f"git/trees/{commit.json()['tree']['sha']}", params={"recursive": "1"})
                if tree.ok:
                    for item in tree.json().get("tree", []):
                        path = item.get("path", "")
                        if path.startswith(gh.SONGS_PREFIX) and path.endswith(".mp3"):
                            sizes[path.rsplit("/", 1)[-1].removesuffix(".mp3")] = int(item.get("size") or 0)
        for s in songs:
            if not s.get("size"):
                s["size"] = sizes.get(s["id"], 0)
            if not s.get("url"):
                s["url"] = f"{DEFAULT_CONTENT_URL}/songs/{s['id']}.mp3?v={idx.get('version') or 0}"
            s["source"] = s.get("provider") or "github"
    except Exception as ex:
        return error(str(ex), 502)
    return jsonify(index=idx, songs=songs, storage=_songs_storage_payload(stats),
                   max_mb=10, max_kbps=gh.SONG_MP3_MAX_KBPS,
                   default_provider=storage.default_provider(data.get("settings")))

@app.post("/api/admin/songs/batch/start")
def songs_batch_start():
    if (e := need_github()):
        return e
    try:
        stats = gh.website_docs_stats()
    except Exception as ex:
        return error(str(ex), 502)
    try:
        provider = _audio_provider(body() if request.is_json else request.form)
    except ValueError as ex:
        return error(str(ex), 400)
    if provider == "github" and stats["docs_bytes"] >= gh.DOCS_BLOCK_BYTES:
        return error(
            f"Storage full: website/docs is over {gh.DOCS_BLOCK_BYTES // (1024 * 1024)} MB. "
            "Delete songs or other content before uploading.",
            400,
        )
    bid = secrets.token_hex(8)
    with publish_lock:
        song_batches[bid] = {
            "items": [], "status": "open", "created": time.time(), "provider": provider,
            "error": "", "warning": (
                f"Warning: website/docs is over {gh.DOCS_WARN_BYTES // (1024 * 1024)} MB"
                if stats["docs_bytes"] >= gh.DOCS_WARN_BYTES else ""
            ),
        }
    return jsonify(batch_id=bid, provider=provider, storage=_songs_storage_payload(stats),
                   warning=song_batches[bid]["warning"])

@app.post("/api/admin/songs/batch/<bid>/file")
def songs_batch_file(bid):
    """Validate one MP3 or paste-link; stage for one Git commit of the song list."""
    if (e := need_github()):
        return e
    with publish_lock:
        batch = song_batches.get(bid)
    if not batch or batch.get("status") != "open":
        return error("Unknown or closed upload batch", 404)
    title = str(request.form.get("title", "")).strip()
    artist = str(request.form.get("artist", "")).strip()
    if (err := gh.song_title_ok(title)):
        return error(err, 400)
    if (err := gh.song_artist_ok(artist)):
        return error(err, 400)
    provider = batch.get("provider") or storage.default_provider(data.get("settings"))
    link = str(request.form.get("link", "")).strip()
    try:
        if link:
            stored = storage.register_link(link, max_kbps=gh.SONG_MP3_MAX_KBPS)
            item = {
                "client_id": str(request.form.get("client_id", ""))[:40],
                "title": title, "artist": artist,
                "provider": stored.provider, "url": stored.url, "key": "",
                "size": stored.size, "sha": "",
            }
        elif provider == "github":
            f = request.files.get("file")
            if not f:
                raise StorageError("Choose an MP3 file or paste an HTTPS audio link")
            cleaned, errors = gh.validate_song_mp3(f.read())
            if errors:
                raise StorageError(errors[0])
            stats = gh.website_docs_stats()
            pending = sum(it.get("size", 0) for it in batch["items"]) + len(cleaned)
            if stats["docs_bytes"] + pending > gh.DOCS_BLOCK_BYTES:
                raise StorageError(
                    f"Upload blocked: this file would push website/docs over "
                    f"{gh.DOCS_BLOCK_BYTES // (1024 * 1024)} MB"
                )
            blob_sha = gh.create_blob(cleaned)
            item = {
                "client_id": str(request.form.get("client_id", ""))[:40],
                "title": title, "artist": artist,
                "provider": "github", "url": "", "key": "",
                "size": len(cleaned), "sha": blob_sha,
            }
        else:
            stored = _ingest_audio(
                file_storage=request.files.get("file"),
                link="",
                provider=provider,
                docs_rel="songs/_unused.mp3",
                object_key=storage.object_key_for("songs", f"pending-{secrets.token_hex(4)}.mp3"),
                max_bytes=gh.SONG_MP3_MAX_BYTES,
                max_kbps=gh.SONG_MP3_MAX_KBPS,
            )
            item = {
                "client_id": str(request.form.get("client_id", ""))[:40],
                "title": title, "artist": artist,
                "provider": stored.provider, "url": stored.url, "key": stored.key,
                "size": stored.size, "sha": "",
            }
    except (StorageError, ValueError) as ex:
        return error(str(ex), 400)
    except Exception as ex:
        return error(str(ex), 502)
    with publish_lock:
        batch = song_batches.get(bid)
        if not batch or batch.get("status") != "open":
            return error("Batch closed", 404)
        batch["items"].append(item)
        n = len(batch["items"])
    return jsonify(ok=True, index=n - 1, size=item["size"],
                   source={"provider": item["provider"], "url": item.get("url") or "", "key": item.get("key") or ""},
                   warning="")

@app.post("/api/admin/songs/batch/<bid>/commit")
def songs_batch_commit(bid):
    if (e := need_github()):
        return e
    with publish_lock:
        batch = song_batches.get(bid)
        if not batch or batch.get("status") != "open":
            return error("Unknown or closed upload batch", 404)
        items = list(batch["items"])
        batch["status"] = "committing"
    if not items:
        with publish_lock:
            if bid in song_batches:
                song_batches[bid]["status"] = "open"
        return error("No files in this batch", 400)
    try:
        idx, songs = gh.load_songs_list()
        old_count = len(songs)
        nid = gh.next_song_id(songs)
        added = []
        changes = {}
        store_items = [it for it in (idx.get("items") or []) if isinstance(it, dict)]
        for it in items:
            sid = f"s{nid:04d}"
            nid += 1
            song = {"id": sid, "title": it["title"], "artist": it.get("artist") or ""}
            if it.get("url"):
                song["url"] = it["url"]
            elif it.get("sha"):
                song["url"] = f"{DEFAULT_CONTENT_URL}/songs/{sid}.mp3"
            if it.get("provider"):
                song["provider"] = it["provider"]
            if it.get("key"):
                song["key"] = it["key"]
            if it.get("sha"):
                changes[f"{gh.SONGS_PREFIX}{sid}.mp3"] = it["sha"]
                song["key"] = f"{gh.SONGS_PREFIX}{sid}.mp3"
            songs.append(song)
            store_items.append({
                "id": sid, "provider": song.get("provider") or "github",
                "key": song.get("key") or "", "size": it.get("size") or 0,
            })
            added.append(sid)
        version = int(idx.get("version") or 0) + 1
        pages = gh.build_song_pages(songs)
        changes.update(pages)
        changes.update(gh.songs_page_deletes(old_count, len(songs)))
        index = {
            "version": version, "count": len(songs), "pageSize": gh.SONGS_PAGE_SIZE,
            "items": store_items,
        }
        changes[gh.SONGS_PREFIX + "index.json"] = json.dumps(index, ensure_ascii=True, separators=(",", ":")).encode()
        sha = gh.atomic_update(changes, f"Add songs v{version} (+{len(added)})")
        with publish_lock:
            if bid in song_batches:
                song_batches[bid]["status"] = "done"
                song_batches[bid]["commit"] = sha
        return jsonify(ok=True, commit=sha, version=version, count=len(songs), added=added)
    except Exception as ex:
        app.logger.exception("songs batch commit failed")
        with publish_lock:
            if bid in song_batches:
                song_batches[bid]["status"] = "open"
                song_batches[bid]["error"] = str(ex)
        return error(str(ex), 502)

@app.delete("/api/admin/songs/batch/<bid>")
def songs_batch_cancel(bid):
    with publish_lock:
        song_batches.pop(bid, None)
    return jsonify(ok=True)

@app.patch("/api/admin/songs/<sid>")
def songs_edit(sid):
    if (e := need_github()):
        return e
    if not re.fullmatch(r"s\d{4}", sid):
        return error("Bad song id", 400)
    d = body()
    title = str(d.get("title", "")).strip()
    artist = str(d.get("artist", "")).strip()
    if (err := gh.song_title_ok(title)):
        return error(err, 400)
    if (err := gh.song_artist_ok(artist)):
        return error(err, 400)
    try:
        idx, songs = gh.load_songs_list()
        found = False
        for s in songs:
            if s["id"] == sid:
                s["title"], s["artist"] = title, artist
                found = True
                break
        if not found:
            return error("Song not found", 404)
        version = int(idx.get("version") or 0) + 1
        changes = gh.build_song_pages(songs)
        changes[gh.SONGS_PREFIX + "index.json"] = json.dumps(
            {"version": version, "count": len(songs), "pageSize": gh.SONGS_PAGE_SIZE},
            ensure_ascii=True, separators=(",", ":"),
        ).encode()
        sha = gh.atomic_update(changes, f"Edit song {sid} v{version}")
    except Exception as ex:
        return error(str(ex), 502)
    return jsonify(ok=True, commit=sha, version=version)

@app.post("/api/admin/songs/reorder")
def songs_reorder():
    if (e := need_github()):
        return e
    order = body().get("ids")
    if not isinstance(order, list) or not order:
        return error("ids required", 400)
    try:
        idx, songs = gh.load_songs_list()
        by_id = {s["id"]: s for s in songs}
        if sorted(order) != sorted(by_id.keys()):
            return error("ids must match the full song list", 400)
        songs = [by_id[i] for i in order]
        version = int(idx.get("version") or 0) + 1
        changes = gh.build_song_pages(songs)
        changes.update(gh.songs_page_deletes(len(by_id), len(songs)))
        changes[gh.SONGS_PREFIX + "index.json"] = json.dumps(
            {"version": version, "count": len(songs), "pageSize": gh.SONGS_PAGE_SIZE},
            ensure_ascii=True, separators=(",", ":"),
        ).encode()
        sha = gh.atomic_update(changes, f"Reorder songs v{version}")
    except Exception as ex:
        return error(str(ex), 502)
    return jsonify(ok=True, commit=sha, version=version)

@app.delete("/api/admin/songs/<sid>")
def songs_delete(sid):
    if (e := need_github()):
        return e
    if not re.fullmatch(r"s\d{4}", sid):
        return error("Bad song id", 400)
    try:
        idx, songs = gh.load_songs_list()
        old_count = len(songs)
        removed = next((s for s in songs if s["id"] == sid), None)
        songs = [s for s in songs if s["id"] != sid]
        if len(songs) == old_count:
            return error("Song not found", 404)
        version = int(idx.get("version") or 0) + 1
        store_items = [it for it in (idx.get("items") or []) if it.get("id") != sid]
        changes = gh.build_song_pages(songs)
        changes.update(gh.songs_page_deletes(old_count, len(songs)))
        changes[f"{gh.SONGS_PREFIX}{sid}.mp3"] = None
        changes[gh.SONGS_PREFIX + "index.json"] = json.dumps(
            {"version": version, "count": len(songs), "pageSize": gh.SONGS_PAGE_SIZE, "items": store_items},
            ensure_ascii=True, separators=(",", ":"),
        ).encode()
        sha = gh.atomic_update(changes, f"Delete song {sid} v{version}")
        if removed:
            try:
                storage.delete_stored(removed)
            except Exception:
                app.logger.exception("song storage delete failed")
    except Exception as ex:
        return error(str(ex), 502)
    return jsonify(ok=True, commit=sha, version=version, count=len(songs))

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
