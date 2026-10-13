#!/usr/bin/env python3
"""A stand-in for Yandex Music's API and OAuth server, for the simulator.

    python3 tools/yandex_mock.py FILES_DIR [PORT]

Point the player at it with, in device_config.ini:

    [yandex]
    base_url = http://127.0.0.1:PORT
    auth_url = http://127.0.0.1:PORT

and put the public desktop values of streaming-keys.ini.example in a plain
streaming-keys.ini. FILES_DIR holds the audio the tracks resolve to:

    t101.lossless.enc  track 101 lossless: FLAC in fragmented MP4, encrypted
                       with AES-256-CTR, zero IV, key KEY_256 below
                       (codec "flac-mp4", transport "encraw")
    t102.enc    track 102 lossless: plain FLAC encrypted with AES-128-CTR,
                zero IV, key 00112233445566778899aabbccddeeff (an older answer)
    t103.mp3    track 103 (codec "mp3"): lossy only, and every track's "nq"
    t104.whole.enc  track 104 lossless: FLAC in a whole (not fragmented) MP4,
                    moov first, AES-256-CTR with KEY_256
    t105.m4a    AAC in fragmented MP4 (codec "aac-mp4"): the lossy file of
                every track that has one, and track 105's only file
    cover.jpg   every album's cover (/cover/<size>)

    (t101.flac, t104.mp4 are kept for older tests; nothing serves them now.)

get-file-info behaves like the real service does since 2025: a lossless file
only goes out over transports=encraw; a "raw" request, or one for a track that
has no lossless file, gets the best lossy file among the codecs asked for
(AAC 256 in MP4, else MP3 320). quality=nq gets MP3 192. Set
YANDEX_MOCK_REFUSE_ENCRAW=1 to have encraw refused with a 400 (the player must
fall back to raw), YANDEX_MOCK_NO_PLUS=1 to have the account without Plus.

The likes are checked strictly, as the service would refuse them: POST only
(a GET on add-multiple or remove is 405), the uid in the path must be the
signed-in one, the body must be a form (application/x-www-form-urlencoded)
whose "track-ids" is a comma list of "track" or "track:album" numbers, and the
X-Yandex-Music-Client header must be there. Every like, accepted or not, is
printed as "mock: LIKE ..." so a test can read what the player sent.

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
import re
import sys
import threading
import time
import urllib.parse
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

SIGN_KEY = b"kzqU4XhfCaY6B6JTHODeq5"
TOKEN = "mock-access-token"
UID = 12345
KEY_102 = "00112233445566778899aabbccddeeff"
KEY_256 = "8f1e2d3c4b5a69788796a5b4c3d2e1f000112233445566778899aabbccddeeff"

# The lossless file of each track that has one: (codec, file, key).
LOSSLESS = {
    "101": ("flac-mp4", "t101.lossless.enc", KEY_256),
    "102": ("flac", "t102.enc", KEY_102),
    "104": ("flac-mp4", "t104.whole.enc", KEY_256),
}
# Tracks whose only lossy file is an MP3; the others have AAC in MP4.
MP3_ONLY = {"103"}


def track(tid, title, artist, album_id, album, secs, index, volume=1):
    return {
        "id": str(tid),
        "title": title,
        "available": True,
        "durationMs": secs * 1000,
        "artists": [{"id": 7, "name": artist}],
        "albums": [{"id": album_id, "title": album, "trackPosition": {"index": index, "volume": volume}}],
    }


TRACKS = {
    "101": track(101, "Pink Noise (FLAC)", "Mock Artist", 11, "Mock Album", 45, 1),
    "102": track(102, "Encrypted Sine", "Mock Artist", 11, "Mock Album", 25, 2),
    "103": track(103, "MP3 Sine", "Другой Артист", 12, "Second Album", 25, 1),
    "104": track(104, "FLAC in MP4", "Mock Artist", 12, "Second Album", 25, 2),
    "105": track(105, "AAC: in MP4?", "Mock Artist", 12, "Second Album", 30, 1, 2),
}

# The albums, as /albums/<id>/with-tracks gives them: the second has two
# discs (volumes) and the artist of its first track differs from the album's.
ALBUMS = {
    "11": {"id": 11, "title": "Mock Album", "year": 2024, "genre": "rock", "trackCount": 2,
           "artists": [{"id": 7, "name": "Mock Artist"}], "volumes": [["101", "102"]]},
    "12": {"id": 12, "title": "Second Album", "year": 2025, "genre": "rusrock", "trackCount": 3,
           "artists": [{"id": 7, "name": "Mock Artist"}], "volumes": [["103", "104"], ["105"]]},
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

    LIKE_PATH = re.compile(r"^/users/([^/]*)/(likes|dislikes)/tracks/(add-multiple|remove)$")
    TRACK_IDS = re.compile(r"^\d+(:\d+)?(,\d+(:\d+)?)*$")

    def like_refused(self, method, p, body=None):
        """None when the like is well formed, else the answer already sent."""
        m = self.LIKE_PATH.match(p)
        if not m:
            return None
        why = None
        if method != "POST":
            why = (405, f"method {method}")
        elif m.group(1) != str(UID):
            why = (403, f"uid {m.group(1)!r} is not {UID}")
        elif not self.headers.get("Content-Type", "").startswith("application/x-www-form-urlencoded"):
            why = (400, f"content type {self.headers.get('Content-Type')!r}")
        elif not self.headers.get("X-Yandex-Music-Client"):
            why = (400, "no X-Yandex-Music-Client")
        elif not self.TRACK_IDS.match((body or {}).get("track-ids", "")):
            why = (400, f"track-ids {(body or {}).get('track-ids')!r}")
        if why:
            sys.stderr.write(f"mock: LIKE REFUSED {method} {p}: {why[1]}\n")
            self.send_json({"error": {"name": "validate", "message": why[1]}}, why[0])
            return True
        return None

    def do_GET(self):
        url = urllib.parse.urlparse(self.path)
        q = {k: v[0] for k, v in urllib.parse.parse_qs(url.query).items()}
        p = url.path
        if p.startswith("/files/"):
            return self.serve_file(p[len("/files/"):])
        if p.startswith("/cover/"):
            return self.serve_file("cover.jpg")  # covers are public, as on avatars.yandex.net
        if not self.authorised():
            return
        if self.like_refused("GET", p):
            return
        if p == "/account/status":
            plus = not os.environ.get("YANDEX_MOCK_NO_PLUS")
            return self.send_json({"result": {"account": {"uid": UID, "login": "tester", "displayName": "Test User"},
                                              "plus": {"hasPlus": plus}}})
        if p == f"/users/{UID}/likes/tracks":
            with lock:
                ids = [{"id": t, "albumId": "11"} for t in state["liked"]]
            return self.send_json({"result": {"library": {"uid": UID, "tracks": ids}}})
        if p == f"/users/{UID}/playlists/list":
            return self.send_json({"result": [{"kind": 3, "title": "Road Trip", "trackCount": 2, "owner": {"uid": UID}}]})
        if p == f"/users/{UID}/playlists/3":
            return self.send_json({"result": {"tracks": [{"id": 103, "track": TRACKS["103"]}, {"id": 101, "track": TRACKS["101"]}]}})
        if p.startswith("/albums/") and p.endswith("/with-tracks"):
            album = ALBUMS.get(p.split("/")[2])
            if not album:
                return self.send_json({"error": {"name": "not-found"}}, 404)
            host = self.headers.get("Host")
            result = dict(album, coverUri=f"http://{host}/cover/%%",
                          volumes=[[TRACKS[t] for t in v] for v in album["volumes"]])
            return self.send_json({"result": result})

        if p.startswith("/artists/") and p.endswith("/direct-albums"):
            return self.send_json({"result": {"albums": [{"id": 11, "title": "Mock Album", "year": 2024, "artists": [{"name": "Mock Artist"}]},
                                                         {"id": 12, "title": "Second Album", "year": 2025, "artists": [{"name": "Mock Artist"}]}]}})
        if p == "/search":
            text = q.get("text", "").lower()
            hits = [t for t in TRACKS.values() if text in t["title"].lower() or text in t["artists"][0]["name"].lower()]
            return self.send_json({"result": {
                "tracks": {"results": hits},
                "albums": {"results": [{"id": 11, "title": "Mock Album", "year": 2024, "artists": [{"name": "Mock Artist"}]},
                                       {"id": 12, "title": "Second Album", "year": 2025, "artists": [{"name": "Mock Artist"}]}]},
                "artists": {"results": [{"id": 7, "name": "Mock Artist"}]},
            }})
        if p == "/get-file-info":
            return self.file_info(q)
        self.send_json({"error": "not found"}, 404)

    def file_info(self, q):
        if not self.headers.get("X-Yandex-Music-Client", "").startswith("YandexMusic"):
            sys.stderr.write("mock: get-file-info without X-Yandex-Music-Client\n")
            return self.send_json({"error": {"name": "bad-client"}}, 400)
        message = q["ts"] + q["trackId"] + q["quality"] + q["codecs"].replace(",", "") + q["transports"]
        want = base64.b64encode(hmac.new(SIGN_KEY, message.encode(), hashlib.sha256).digest()).decode().rstrip("=")
        if q.get("sign") != want:
            sys.stderr.write(f"mock: BAD SIGNATURE for {message!r}: got {q.get('sign')} want {want}\n")
            return self.send_json({"error": {"name": "bad-sign"}}, 403)
        tid, quality, transport = q["trackId"], q["quality"], q["transports"]
        codecs = q["codecs"].split(",")
        if transport == "encraw" and os.environ.get("YANDEX_MOCK_REFUSE_ENCRAW"):
            sys.stderr.write(f"mock: file-info {tid} {quality}/{transport}: encraw REFUSED\n")
            return self.send_json({"error": {"name": "transport-not-allowed"}}, 400)
        plus = not os.environ.get("YANDEX_MOCK_NO_PLUS")
        lossless = LOSSLESS.get(tid)
        key = None
        if quality == "lossless" and transport == "encraw" and plus and lossless and lossless[0] in codecs:
            codec, name, key = lossless
            got_quality, bitrate = "lossless", 0
        elif quality == "nq":
            codec, name, got_quality, bitrate = "mp3", "t103.mp3", "nq", 192
        elif tid not in MP3_ONLY and "aac-mp4" in codecs:
            codec, name, got_quality, bitrate = "aac-mp4", "t105.m4a", "hq", 256
        else:
            codec, name, got_quality, bitrate = "mp3", "t103.mp3", "hq", 320
        sys.stderr.write(f"mock: file-info {tid} asked {quality}/{transport} codecs={q['codecs']} -> {codec} {bitrate} ({got_quality}{', encrypted' if key else ''})\n")
        host = self.headers.get("Host")
        info = {"trackId": tid, "quality": got_quality, "codec": codec, "bitrate": bitrate,
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
        kbps = int(os.environ.get("YANDEX_MOCK_KBPS") or 0)
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
        if self.like_refused("POST", p, body):
            return
        if p.startswith(f"/users/{UID}/likes/tracks/") or p.startswith(f"/users/{UID}/dislikes/tracks/"):
            sys.stderr.write(f"mock: LIKE {p} {body}\n")
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
