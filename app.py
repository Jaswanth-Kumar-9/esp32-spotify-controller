import os
from functools import wraps

from dotenv import load_dotenv
from flask import Flask, jsonify, redirect, request, url_for
import spotipy
from spotipy.oauth2 import SpotifyOAuth
from spotipy.exceptions import SpotifyException

load_dotenv()

CLIENT_ID = os.getenv("SPOTIFY_CLIENT_ID")
CLIENT_SECRET = os.getenv("SPOTIFY_CLIENT_SECRET")
REDIRECT_URI = os.getenv(
    "SPOTIFY_REDIRECT_URI",
    "http://127.0.0.1:8888/callback",
)
BRIDGE_API_KEY = os.getenv("BRIDGE_API_KEY", "")

if not CLIENT_ID or not CLIENT_SECRET:
    raise RuntimeError(
        "Missing SPOTIFY_CLIENT_ID or SPOTIFY_CLIENT_SECRET. "
        "Copy .env.example to .env and fill them in."
    )

SCOPE = "user-read-playback-state user-modify-playback-state"

sp_oauth = SpotifyOAuth(
    client_id=CLIENT_ID,
    client_secret=CLIENT_SECRET,
    redirect_uri=REDIRECT_URI,
    scope=SCOPE,
    cache_path=".spotify_token_cache",
    open_browser=False,
)

sp = spotipy.Spotify(auth_manager=sp_oauth)

app = Flask(__name__)


def api_key_required(func):
    @wraps(func)
    def wrapper(*args, **kwargs):
        if BRIDGE_API_KEY:
            supplied = request.headers.get("X-API-Key", "")
            if supplied != BRIDGE_API_KEY:
                return jsonify({"error": "unauthorized"}), 401
        return func(*args, **kwargs)

    return wrapper


def spotify_error(e):
    status = getattr(e, "http_status", 500)
    message = str(getattr(e, "msg", e))
    return jsonify({"error": "spotify_error", "message": message}), status


@app.get("/")
def index():
    token = sp_oauth.validate_token(sp_oauth.cache_handler.get_cached_token())
    authenticated = bool(token)

    return f"""
    <!doctype html>
    <html>
    <head>
      <meta charset="utf-8">
      <title>ESP32 Spotify Bridge</title>
      <style>
        body {{
          font-family: system-ui, sans-serif;
          max-width: 760px;
          margin: 40px auto;
          padding: 0 20px;
        }}
        code, pre {{
          background: #f4f4f4;
          padding: 2px 5px;
          border-radius: 4px;
        }}
        .ok {{ color: green; }}
        .no {{ color: #a00; }}
      </style>
    </head>
    <body>
      <h1>ESP32 Spotify Bridge</h1>
      <p>Status:
        <strong class="{'ok' if authenticated else 'no'}">
          {'Authenticated' if authenticated else 'Not authenticated'}
        </strong>
      </p>
      {'<p><a href="/login">Log in to Spotify</a></p>' if not authenticated else ''}
      <p>ESP32 API endpoints:</p>
      <pre>
GET  /api/player
POST /api/playpause
POST /api/next
POST /api/previous
GET  /api/devices
      </pre>
    </body>
    </html>
    """


@app.get("/login")
def login():
    token = sp_oauth.validate_token(sp_oauth.cache_handler.get_cached_token())
    if token:
        return redirect(url_for("index"))
    return redirect(sp_oauth.get_authorize_url())


@app.get("/callback")
def callback():
    code = request.args.get("code")
    if not code:
        error = request.args.get("error", "unknown_error")
        return f"Spotify authorization failed: {error}", 400

    try:
        sp_oauth.get_access_token(code, as_dict=True, check_cache=False)
        return """
        <h2>Spotify connected ✅</h2>
        <p>You can close this tab.</p>
        <p>The ESP32 can now control Spotify.</p>
        """
    except Exception as e:
        return f"Spotify authorization failed: {e}", 400


@app.get("/api/player")
@api_key_required
def api_player():
    try:
        playback = sp.current_playback()

        if not playback or not playback.get("item"):
            return jsonify({
                "playing": False,
                "title": "",
                "artist": "",
                "progress_ms": 0,
                "duration_ms": 0,
                "device": "",
            })

        item = playback["item"]

        artists = item.get("artists", [])
        artist = ", ".join(a.get("name", "") for a in artists)

        return jsonify({
            "playing": bool(playback.get("is_playing")),
            "title": item.get("name", ""),
            "artist": artist,
            "progress_ms": int(playback.get("progress_ms") or 0),
            "duration_ms": int(item.get("duration_ms") or 0),
            "device": (playback.get("device") or {}).get("name", ""),
        })

    except SpotifyException as e:
        return spotify_error(e)


@app.post("/api/playpause")
@api_key_required
def api_playpause():
    try:
        playback = sp.current_playback()

        if playback and playback.get("is_playing"):
            sp.pause_playback()
            action = "paused"
        else:
            sp.start_playback()
            action = "playing"

        return jsonify({"ok": True, "action": action})

    except SpotifyException as e:
        return spotify_error(e)


@app.post("/api/next")
@api_key_required
def api_next():
    try:
        sp.next_track()
        return jsonify({"ok": True})
    except SpotifyException as e:
        return spotify_error(e)


@app.post("/api/previous")
@api_key_required
def api_previous():
    try:
        sp.previous_track()
        return jsonify({"ok": True})
    except SpotifyException as e:
        return spotify_error(e)


@app.get("/api/devices")
@api_key_required
def api_devices():
    try:
        devices = sp.devices().get("devices", [])
        return jsonify({
            "devices": [
                {
                    "id": d.get("id"),
                    "name": d.get("name"),
                    "type": d.get("type"),
                    "is_active": d.get("is_active"),
                }
                for d in devices
            ]
        })
    except SpotifyException as e:
        return spotify_error(e)


if __name__ == "__main__":
    print("ESP32 Spotify Bridge")
    print(f"OAuth callback: {REDIRECT_URI}")
    print("LAN API: http://0.0.0.0:8888")
    app.run(host="0.0.0.0", port=8888, debug=False)


