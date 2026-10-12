#!/usr/bin/env python3
"""A stand-in for Yandex Music's API and OAuth server, for the simulator.

    python3 tools/yandex_mock.py FILES_DIR [PORT]

Point the player at it with, in device_config.ini:

    [yandex]
    base_url = http://127.0.0.1:PORT
    auth_url = http://127.0.0.1:PORT

and put the public desktop values of streaming-keys.ini.example in a plain
streaming-keys.ini. FILES_DIR holds the audio the tracks resolve to:

    t101.flac   track 101, plain FLAC (codec "flac", transport raw)
    t102.enc    track 102, FLAC encrypted with AES-128-CTR, zero IV, key
                00112233445566778899aabbccddeeff (codec "flac", "encraw")
    t103.mp3    track 103 (codec "mp3")
    t104.mp4    track 104, FLAC in fragmented MP4 (codec "flac-mp4")
    t105.m4a    track 105, AAC in fragmented MP4 (codec "aac-mp4")

What it checks, beside answering: the OAuth header on every API call, the
device-flow sequence (two "pending" polls before the token), and the
get-file-info signature -- HMAC-SHA256 of ts + trackId + quality + codecs
without commas + transports, base64 without '=' -- which it recomputes with
the public sign key and refuses with 403 when it differs. Feedback events are
printed, so a test can read what My Wave was told.

YANDEX_MOCK_KBPS=N in the environment serves the audio at N KB/s, slow
enough to watch a download in progress (the Wi-Fi keep-alive, the cache's
"busy" state).
"""

import base64
import hashlib
import hmac
import json
import os
import sys
import threading
import time
import urllib.parse
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

SIGN_KEY = b"kzqU4XhfCaY6B6JTHODeq5"
TOKEN = "mock-access-token"
UID = 12345
KEY_102 = "00112233445566778899aabbccddeeff"

FILES = {
    "101": ("flac", "t101.flac", None),
    "102": ("flac", "t102.enc", KEY_102),
    "103": ("mp3", "t103.mp3", None),
    "104": ("flac-mp4", "t104.mp4", None),
    "105": ("aac-mp4", "t105.m4a", None),
}


def track(tid, title, artist, album_id, album, secs):
    return {
        "id": str(tid),
        "title": title,
        "available": True,
        "durationMs": secs * 1000,
        "artists": [{"id": 7, "name": artist}],
        "albums": [{"id": album_id, "title": album, "trackPosition": {"index": tid % 100}}],
    }


TRACKS = {
    "101": track(101, "Pink Noise (FLAC)", "Mock Artist", 11, "Mock Album", 45),
    "102": track(102, "Encrypted Sine", "Mock Artist", 11, "Mock Album", 25),
    "103": track(103, "MP3 Sine", "Другой Артист", 12, "Second Album", 25),
    "104": track(104, "FLAC in MP4", "Mock Artist", 12, "Second Album", 25),
    "105": track(105, "AAC in MP4", "Mock Artist", 12, "Second Album", 30),
}

state = {"polls": 0, "batch": 0, "liked": ["101", "102", "103", "104", "105"]}
lock = threading.Lock()


class Handler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def log_message(self, fmt, *args):
        sys.stderr.write("mock: " + (fmt % args) + "\n")

    def send_json(self, obj, status=200):
        body = json.dumps(obj, ensure_ascii=False).encode()
        self.send_response(status)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def form(self):
        n = int(self.headers.get("Content-Length") or 0)
        raw = self.rfile.read(n).decode() if n else ""
        if self.headers.get("Content-Type", "").startswith("application/json"):
            return json.loads(raw or "{}")
        return {k: v[0] for k, v in urllib.parse.parse_qs(raw).items()}

    def authorised(self):
        if self.headers.get("Authorization") != "OAuth " + TOKEN:
            self.send_json({"error": {"name": "session-expired"}}, 401)
            return False
        return True

    def do_GET(self):
        url = urllib.parse.urlparse(self.path)
        q = {k: v[0] for k, v in urllib.parse.parse_qs(url.query).items()}
        p = url.path
        if p.startswith("/files/"):
            return self.serve_file(p[len("/files/"):])
        if not self.authorised():
            return
        if p == "/account/status":
            return self.send_json({"result": {"account": {"uid": UID, "login": "tester", "displayName": "Test User"}}})
        if p == f"/users/{UID}/likes/tracks":
            with lock:
                ids = [{"id": t, "albumId": "11"} for t in state["liked"]]
            return self.send_json({"result": {"library": {"uid": UID, "tracks": ids}}})
        if p == f"/users/{UID}/playlists/list":
            return self.send_json({"result": [{"kind": 3, "title": "Road Trip", "trackCount": 2, "owner": {"uid": UID}}]})
        if p == f"/users/{UID}/playlists/3":
            return self.send_json({"result": {"tracks": [{"id": 103, "track": TRACKS["103"]}, {"id": 101, "track": TRACKS["101"]}]}})
        if p.startswith("/albums/") and p.endswith("/with-tracks"):
            return self.send_json({"result": {"id": 11, "title": "Mock Album", "volumes": [[TRACKS["101"], TRACKS["102"]]]}})
        if p.startswith("/artists/") and p.endswith("/direct-albums"):
            return self.send_json({"result": {"albums": [{"id": 11, "title": "Mock Album", "year": 2024, "artists": [{"name": "Mock Artist"}]},
                                                         {"id": 12, "title": "Second Album", "year": 2025, "artists": [{"name": "Mock Artist"}]}]}})
        if p == "/search":
            text = q.get("text", "").lower()
            hits = [t for t in TRACKS.values() if text in t["title"].lower() or text in t["artists"][0]["name"].lower()]
            return self.send_json({"result": {
                "tracks": {"results": hits},
                "albums": {"results": [{"id": 11, "title": "Mock Album", "year": 2024, "artists": [{"name": "Mock Artist"}]}]},
                "artists": {"results": [{"id": 7, "name": "Mock Artist"}]},
            }})
        if p == "/get-file-info":
            return self.file_info(q)
        self.send_json({"error": "not found"}, 404)

    def file_info(self, q):
        message = q["ts"] + q["trackId"] + q["quality"] + q["codecs"].replace(",", "") + q["transports"]
        want = base64.b64encode(hmac.new(SIGN_KEY, message.encode(), hashlib.sha256).digest()).decode().rstrip("=")
        if q.get("sign") != want:
            sys.stderr.write(f"mock: BAD SIGNATURE for {message!r}: got {q.get('sign')} want {want}\n")
            return self.send_json({"error": {"name": "bad-sign"}}, 403)
        sys.stderr.write(f"mock: signature OK for track {q['trackId']}\n")
        codec, name, key = FILES.get(q["trackId"], FILES["103"])
        host = self.headers.get("Host")
        info = {"trackId": q["trackId"], "quality": "lossless", "codec": codec, "bitrate": 0,
                "transport": "encraw" if key else "raw", "urls": [f"http://{host}/files/{name}"]}
        if key:
            info["key"] = key
        self.send_json({"result": {"downloadInfo": info}})

    def serve_file(self, name):
        path = os.path.join(FILES_DIR, os.path.basename(name))
        if not os.path.exists(path):
            return self.send_json({"error": "gone"}, 404)
        size = os.path.getsize(path)
        start, end = 0, size - 1
        rng = self.headers.get("Range")
        if rng and rng.startswith("bytes="):
            a, _, b = rng[6:].partition("-")
            start = int(a or 0)
            end = int(b) if b else size - 1
        with open(path, "rb") as f:
            f.seek(start)
            data = f.read(end - start + 1)
        self.send_response(206 if rng else 200)
        self.send_header("Content-Type", "application/octet-stream")
        self.send_header("Content-Length", str(len(data)))
        if rng:
            self.send_header("Content-Range", f"bytes {start}-{end}/{size}")
        self.end_headers()
        kbps = int(os.environ.get("YANDEX_MOCK_KBPS", "0"))
        chunk = kbps * 1024 // 10 if kbps > 0 else len(data) or 1
        try:
            for at in range(0, len(data), chunk):
                self.wfile.write(data[at:at + chunk])
                if kbps > 0:
                    time.sleep(0.1)
        except (BrokenPipeError, ConnectionResetError):
            pass

    def do_POST(self):
        p = urllib.parse.urlparse(self.path).path
        body = self.form()
        if p == "/device/code":
            assert body.get("client_id"), "client_id missing"
            assert len(body.get("device_id", "")) == 10, "device_id must be 10 characters"
            with lock:
                state["polls"] = 0
            return self.send_json({"device_code": "mock-device-code", "user_code": "MOCK-1234",
                                   "verification_url": "https://ya.ru/device", "interval": 2, "expires_in": 300})
        if p == "/token":
            if not body.get("client_secret"):
                return self.send_json({"error": "invalid_client"}, 400)
            if body.get("grant_type") == "refresh_token":
                return self.send_json({"access_token": TOKEN, "refresh_token": "mock-refresh", "expires_in": 31536000})
            with lock:
                state["polls"] += 1
                polls = state["polls"]
            if polls < 3:
                return self.send_json({"error": "authorization_pending"}, 400)
            return self.send_json({"access_token": TOKEN, "refresh_token": "mock-refresh", "expires_in": 31536000})
        if not self.authorised():
            return
        if p == "/tracks":
            ids = body.get("track-ids", "").split(",")
            return self.send_json({"result": [TRACKS[i.split(":")[0]] for i in ids if i.split(":")[0] in TRACKS]})
        if p == "/rotor/session/new":
            with lock:
                state["batch"] = 1
            seq = [{"track": TRACKS[t]} for t in ("102", "103", "101")]
            return self.send_json({"result": {"radioSessionId": "mock-wave", "batchId": "batch-1", "sequence": seq}})
        if p == "/rotor/session/mock-wave/tracks":
            with lock:
                state["batch"] += 1
                n = state["batch"]
            seq = [{"track": TRACKS[t]} for t in ("104", "105")]
            sys.stderr.write(f"mock: wave batch {n}, queue={body.get('queue')}\n")
            return self.send_json({"result": {"radioSessionId": "mock-wave", "batchId": f"batch-{n}", "sequence": seq}})
        if p == "/rotor/session/mock-wave/feedback":
            sys.stderr.write(f"mock: FEEDBACK {json.dumps(body)}\n")
            return self.send_json({"result": "ok"})
        if p.startswith(f"/users/{UID}/likes/tracks/") or p.startswith(f"/users/{UID}/dislikes/tracks/"):
            sys.stderr.write(f"mock: {p} {body}\n")
            # The likes are remembered, so a reload shows what the player wrote.
            ids = [i.split(":")[0] for i in body.get("track-ids", "").split(",") if i]
            if "/likes/" in p:
                with lock:
                    for i in ids:
                        if p.endswith("/remove") and i in state["liked"]:
                            state["liked"].remove(i)
                        elif p.endswith("/add-multiple") and i not in state["liked"]:
                            state["liked"].insert(0, i)
                    sys.stderr.write(f"mock: LIKED now {state['liked']}\n")
            return self.send_json({"result": {"revision": 2}})
        self.send_json({"error": "not found"}, 404)


if __name__ == "__main__":
    FILES_DIR = sys.argv[1]
    port = int(sys.argv[2]) if len(sys.argv) > 2 else 8765
    ThreadingHTTPServer(("127.0.0.1", port), Handler).serve_forever()
