#!/usr/bin/env python3
"""A stand-in for zvuk.com (GraphQL, the Tiny profile and the CDN), for the
simulator.

    python3 tools/zvuk_mock.py FILES_DIR [PORT]
    python3 tools/zvuk_mock.py --make-fixtures FILES_DIR   (needs ffmpeg)

Point the player at it in device_config.ini:

    [zvuk]
    base_url = http://127.0.0.1:PORT

and put a token from TOKENS below into zvuk_token.txt at the root of the card.
FILES_DIR holds the audio, made by --make-fixtures (make_fixtures() below):

    moovfirst.mp4  FLAC in a plain MP4, moov before mdat   (track 1001's flac)
    moovlast.mp4   FLAC in a plain MP4, mdat before moov   (track 1002's flac)
    native.flac    a native FLAC behind the "flac" address (track 1003)
    high.mp3       MP3 320, every track's "high"
    mid.mp3        MP3 128, every track's "mid"
    cover.jpg      every cover (/cover/...)

The tokens (32 hex digits, as the real ones):

    a1b2c3d4e5f60718293a4b5c6d7e8f90  an account with a subscription
    f1f2f3f4f5f6f7f8f9fafbfcfdfeff00  without one: "mid" only
    a0a0a0a0a0a0a0a0a0a0a0a0a0a0a0a0  an anonymous (guest) token
    b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0  the anti-bot filter answers everything
                                      with 418 and an HTML page
    anything else                     401

What the real service is said to check, this one checks: a browser
User-Agent ("Mozilla/"), an Origin and a Referer of zvuk.com on GraphQL
(a missing one is a 418 page, as ServicePipe answers); a Referer on the Tiny
profile; X-Auth-Token everywhere. Every request is printed as "mock: ...".

Faults, by the first word of the file named by ZVUK_MOCK_MODE (re-read on
every request, so a test can switch them while the player runs):

    ok      normal
    418     every API request blocked: 418 + HTML
    html    every API request answered 200 with an HTML page
    429     rate limited
    500     server error
    401     the token refused (the session expires)
    expire  every stream address handed out is already expired: the CDN
            answers 403 until the player asks getStream again (which then
            hands out a good one, once)
    drop    the CDN cuts each download once, half way (Range resume)
    slow    the CDN sends 64 KB/s

The catalogue: two artists, three releases, tracks 1001-1009 with the stream
shapes worth testing (1004 has only a DRM FLAC, 1005 no FLAC at all, 1009 no
file), and 300 generated tracks (2000-2299) liked, for the paging.
"""

import json
import os
import re
import sys
import threading
import time
import urllib.parse
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

FILES = sys.argv[1] if len(sys.argv) > 1 else "."
PORT = int(sys.argv[2]) if len(sys.argv) > 2 and sys.argv[2].isdigit() else 6301
MODE_FILE = os.environ.get("ZVUK_MOCK_MODE", "")
LINK_SECS = int(os.environ.get("ZVUK_MOCK_LINK_SECS", "600"))

T_SUB = "a1b2c3d4e5f60718293a4b5c6d7e8f90"
T_FREE = "f1f2f3f4f5f6f7f8f9fafbfcfdfeff00"
T_ANON = "a0a0a0a0a0a0a0a0a0a0a0a0a0a0a0a0"
T_BLOCK = "b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0"

BLOCK_PAGE = (b"<!DOCTYPE html><html><head><title>ServicePipe</title></head><body>"
              b"<h1>Bot activity detected</h1><p>Please enable JavaScript.</p></body></html>")

ARTISTS = {
    "501": "Кино",
    "502": "Pink Floyd",
}
RELEASES = {
    # id: (title, date, artist, [track ids])
    "701": ("Группа крови", "1988-01-01T00:00:00", "501",
            ["1001", "1002", "1003", "1004"]),
    "702": ("The Dark Side of the Moon", "1973-03-01T00:00:00", "502", ["1005", "1006", "1007"]),
    "703": ("Animals", "1977-01-23T00:00:00", "502", ["1008", "1009"]),
}
TITLES = {
    "1001": "Группа крови",
    "1002": "Закрой за мной дверь",
    "1003": "Война",
    "1004": "Спокойная ночь",
    "1005": "Speak to Me",
    "1006": "Breathe",
    "1007": "Time",
    "1008": "Dogs",
    "1009": "Sheep",
}
for i in range(2000, 2300):
    TITLES[str(i)] = "Track %d" % i

# What each track's getStream holds: flac -> file, flacdrm, high, mid.
FLAC_FILE = {"1001": "moovfirst.mp4", "1002": "moovlast.mp4", "1003": "native.flac", "1006": "moovfirst.mp4",
             "1007": "moovlast.mp4", "1008": "moovfirst.mp4"}
DRM_ONLY = {"1004"}
NO_FILE = {"1009"}

lock = threading.Lock()
liked = {
    "track": ["1001", "1005", "1003", "1004"] + [str(i) for i in range(2000, 2300)],
    "release": ["701", "702"],
    "artist": ["502"],
    "playlist": ["901"],
}
PLAYLISTS = {"901": ("Mock mix", ["1006", "1001", "1007", "1002", "1003"]),
             "902": ("Edited", ["1008", "1005"])}
expired_given = set()  # links handed out expired in "expire" mode
dropped = set()


def mode():
    try:
        with open(MODE_FILE) as f:
            return (f.read().split() or ["ok"])[0]
    except OSError:
        return "ok"


def track_obj(tid, with_release=True):
    rel = next((r for r, v in RELEASES.items() if tid in v[3]), None)
    t = {"id": tid, "title": TITLES.get(tid, "?"), "duration": 40,
         "artists": [{"id": RELEASES[rel][2] if rel else "502",
                      "title": ARTISTS[RELEASES[rel][2]] if rel else "Pink Floyd"}]}
    if with_release:
        if rel:
            t["release"] = release_short(rel)
        else:
            t["release"] = {"id": "799", "title": "Generated", "date": "2020-01-01T00:00:00",
                            "image": {"src": "/cover/799?size=300x300"}}
    return t


def release_short(rid):
    title, date, artist, _ = RELEASES[rid]
    return {"id": rid, "title": title, "date": date, "image": {"src": "/cover/%s?size=300x300" % rid},
            "artists": [{"id": artist, "title": ARTISTS[artist]}]}


def release_full(rid, short_tracks=False):
    r = release_short(rid)
    if short_tracks:
        r["tracks"] = [{"id": t} for t in RELEASES[rid][3]]
    else:
        r["tracks"] = [{k: v for k, v in track_obj(t, False).items()} for t in RELEASES[rid][3]]
    return r


class Handler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.0"

    def log_message(self, *args):
        pass

    def say(self, text):
        print("mock: " + text, flush=True)

    def send(self, status, body, ctype="application/json; charset=utf-8", extra=None):
        if isinstance(body, (dict, list)):
            body = json.dumps(body, ensure_ascii=False).encode()
        self.send_response(status)
        self.send_header("Content-Type", ctype)
        self.send_header("Content-Length", str(len(body)))
        for k, v in (extra or {}).items():
            self.send_header(k, v)
        self.end_headers()
        self.wfile.write(body)

    def blocked(self):
        self.send(418, BLOCK_PAGE, "text/html; charset=utf-8")

    # Returns the token, or None after answering the refusal.
    def gate(self, graphql):
        token = self.headers.get("X-Auth-Token", "")
        ua = self.headers.get("User-Agent", "")
        m = mode()
        why = None
        if "Mozilla/" not in ua:
            why = "no browser User-Agent (%s)" % ua
        elif graphql and self.headers.get("Origin") != "https://zvuk.com":
            why = "no Origin"
        elif not self.headers.get("Referer", "").startswith(("https://zvuk.com/", "http://127.0.0.1")):
            why = "no Referer"
        elif token == T_BLOCK or m == "418":
            why = "anti-bot mode"
        if why:
            self.say("BLOCKED %s %s: %s" % (self.command, self.path, why))
            self.blocked()
            return None
        if m == "html":
            self.say("HTML instead of JSON for %s" % self.path)
            self.send(200, BLOCK_PAGE, "text/html")
            return None
        if m == "429":
            self.send(429, {"error": "too many requests"}, extra={"Retry-After": "30"})
            return None
        if m == "500":
            self.send(500, {"error": "server"})
            return None
        if token not in (T_SUB, T_FREE, T_ANON) or m == "401":
            self.say("401 for token of %d characters" % len(token))
            self.send(401, {"detail": "Unauthorized"})
            return None
        return token

    def do_GET(self):
        url = urllib.parse.urlparse(self.path)
        if url.path in ("/api/tiny/profile", "/api/v2/tiny/profile"):
            token = self.gate(False)
            if not token:
                return
            self.say("PROFILE (%s)" % ("sub" if token == T_SUB else "free" if token == T_FREE else "anon"))
            res = {"id": 4242, "name": "Mock Listener" if token != T_ANON else None, "is_anonymous": token == T_ANON,
                   "token": token}
            if token == T_SUB:
                res["subscription"] = {"title": "Звук Премиум",
                                       "expiration": int(time.time() + 30 * 86400) * 1000}
            self.send(200, {"result": res})
            return
        if url.path.startswith("/cover/"):
            with open(os.path.join(FILES, "cover.jpg"), "rb") as f:
                self.send(200, f.read(), "image/jpeg")
            return
        if url.path.startswith("/cdn/"):
            self.cdn(url)
            return
        self.send(404, {"error": "not found"})

    def cdn(self, url):
        name = url.path[len("/cdn/"):]
        q = urllib.parse.parse_qs(url.query)
        exp = int(q.get("exp", ["0"])[0])
        sig = q.get("sig", [""])[0]
        if exp < time.time():
            self.say("CDN 403 expired link for %s (%s)" % (name, sig))
            self.send(403, b"expired", "text/plain")
            return
        path = os.path.join(FILES, os.path.basename(name))
        if not os.path.exists(path):
            self.send(404, b"no", "text/plain")
            return
        size = os.path.getsize(path)
        start = 0
        rng = self.headers.get("Range")
        m = re.match(r"bytes=(\d+)-", rng or "")
        if m:
            start = int(m.group(1))
        self.say("CDN %s from %d of %d (%s)" % (name, start, size, sig))
        self.send_response(206 if start else 200)
        self.send_header("Content-Type", "audio/mpeg" if name.endswith(".mp3") else
                         "audio/flac" if name.endswith(".flac") else "audio/mp4")
        self.send_header("Content-Length", str(size - start))
        if start:
            self.send_header("Content-Range", "bytes %d-%d/%d" % (start, size - 1, size))
        self.send_header("Accept-Ranges", "bytes")
        self.end_headers()
        md = mode()
        cut = size // 2 if md == "drop" and sig not in dropped else -1
        with open(path, "rb") as f:
            f.seek(start)
            pos = start
            while True:
                chunk = f.read(65536)
                if not chunk:
                    break
                if 0 <= cut < pos + len(chunk) and pos <= cut:
                    self.wfile.write(chunk[:cut - pos])
                    dropped.add(sig)
                    self.say("CDN dropped %s at %d" % (name, cut))
                    return
                try:
                    self.wfile.write(chunk)
                except (BrokenPipeError, ConnectionResetError):
                    return
                pos += len(chunk)
                if md == "slow":
                    time.sleep(1)

    def link(self, tid, name, expired=False):
        # "expire" mode: the first getStream of a track hands out every
        # quality's address already expired (stale), the next one good ones.
        exp = int(time.time()) + (-5 if expired else LINK_SECS)
        sig = "%s-%d-%s" % (tid, time.time_ns() % 100000, name.split(".")[0])
        return "http://127.0.0.1:%d/cdn/%s?exp=%d&sig=%s" % (PORT, name, exp, sig)

    def do_POST(self):
        url = urllib.parse.urlparse(self.path)
        if url.path != "/api/v1/graphql":
            self.send(404, {"error": "not found"})
            return
        length = int(self.headers.get("Content-Length", "0"))
        try:
            req = json.loads(self.rfile.read(length) or b"{}")
        except ValueError:
            self.send(400, {"errors": [{"message": "bad json"}]})
            return
        token = self.gate(True)
        if not token:
            return
        op = req.get("operationName", "")
        v = req.get("variables") or {}
        query = req.get("query", "")
        if not query.strip().startswith(("query " + op, "mutation " + op)):
            self.send(400, {"errors": [{"message": "operation name does not match the query"}]})
            return
        self.say("GQL %s %s" % (op, json.dumps(v, ensure_ascii=False)))
        data = self.graphql(op, v, token, query)
        if data is None:
            self.send(200, {"errors": [{"message": "unknown operation " + op}], "data": None})
            return
        self.send(200, {"data": data})

    def graphql(self, op, v, token, query):
        ids = [str(i) for i in v.get("ids", [])]
        if op == "getStream":
            out = []
            for tid in ids:
                if tid in NO_FILE:
                    out.append({"__typename": "Track", "stream": None})
                    continue
                stale = mode() == "expire" and tid not in expired_given
                if stale:
                    expired_given.add(tid)
                    self.say("STREAM %s handed out expired" % tid)
                s = {"expire": time.strftime("%Y-%m-%dT%H:%M:%S", time.gmtime(time.time() + LINK_SECS)),
                     "expireDelta": LINK_SECS, "flac": None, "flacdrm": None, "high": None,
                     "mid": self.link(tid, "mid.mp3", stale)}
                if token == T_SUB:
                    s["high"] = self.link(tid, "high.mp3", stale)
                    if tid in DRM_ONLY:
                        s["flacdrm"] = "http://127.0.0.1:%d/cdn/drm.bin?exp=0" % PORT
                    elif tid in FLAC_FILE:
                        s["flac"] = self.link(tid, FLAC_FILE[tid], stale)
                out.append({"__typename": "Track", "stream": s})
            return {"mediaContents": out}
        if op == "getTracks":
            return {"getTracks": [track_obj(t) for t in ids if t in TITLES]}
        if op == "userTracks":
            with lock:
                return {"collection": {"tracks": [{"id": t} for t in liked["track"]]}}
        if op in ("userCollection", "userPlaylists"):
            with lock:
                c = {k + "s" if k != "release" else "releases": [{"id": i} for i in val] for k, val in liked.items()}
            c = {"tracks": c["tracks"], "releases": c["releases"], "artists": c["artists"],
                 "playlists": c["playlists"] + [{"id": "902"}]}
            return {"collection": c}
        if op in ("getShortPlaylist", "getPlaylists"):
            out = []
            for p in ids:
                if p in PLAYLISTS:
                    o = {"id": p, "title": PLAYLISTS[p][0]}
                    if "tracks" in query:
                        o["tracks"] = [{"id": t} for t in PLAYLISTS[p][1]]
                    out.append(o)
            return {"getPlaylists": out}
        if op == "getReleases":
            short = "tracks { id }" in query
            return {"getReleases": [release_full(r, short) for r in ids if r in RELEASES]}
        if op == "getArtists":
            out = []
            for a in ids:
                if a not in ARTISTS:
                    continue
                o = {"id": a, "title": ARTISTS[a]}
                if v.get("withPopTracks"):
                    o["popularTracks"] = [track_obj(t) for r, rv in RELEASES.items() if rv[2] == a for t in rv[3]]
                if v.get("withReleases"):
                    o["releases"] = [release_short(r) for r, rv in RELEASES.items() if rv[2] == a]
                out.append(o)
            return {"getArtists": out}
        if op == "search":
            text = (v.get("query") or "").lower()
            limit = int(v.get("limit") or 2)
            res = {}
            if "tracks(" in query:
                res["tracks"] = {"items": [track_obj(t) for t in sorted(TITLES) if text in TITLES[t].lower()
                                           or any(text in ARTISTS[RELEASES[r][2]].lower()
                                                  for r in RELEASES if t in RELEASES[r][3])][:limit]}
            if "releases(" in query:
                res["releases"] = {"items": [release_short(r) for r in RELEASES
                                             if text in RELEASES[r][0].lower()
                                             or text in ARTISTS[RELEASES[r][2]].lower()][:limit]}
            if "artists(" in query:
                res["artists"] = {"items": [{"id": a, "title": n} for a, n in ARTISTS.items()
                                            if text in n.lower()][:limit]}
            if "playlists(" in query:
                res["playlists"] = {"items": [{"id": p, "title": t} for p, (t, _) in PLAYLISTS.items()
                                              if text in t.lower()][:limit]}
            return {"search": res}
        if op in ("addItemToCollection", "removeItemFromCollection"):
            kind = v.get("type")
            item = str(v.get("id"))
            if kind not in liked:
                return None
            with lock:
                if op.startswith("add") and item not in liked[kind]:
                    liked[kind].insert(0, item)
                if op.startswith("remove") and item in liked[kind]:
                    liked[kind].remove(item)
            self.say("LIKE %s %s %s" % ("add" if op.startswith("add") else "remove", kind, item))
            return {"collection": {"addItem" if op.startswith("add") else "removeItem": True}}
        return None


def make_fixtures(out):
    """Writes the files the mock serves: 40 s of a stereo tone, 44.1 kHz
    24-bit, as a native FLAC, the same FLAC stream in a plain MP4 with the
    moov first and with it last (what zvuk.com's "flac" addresses give), MP3
    320 and 128, and a cover. The MP4s carry the FLAC's own STREAMINFO (dfLa),
    so the file the player unwraps must decode bit for bit to the native one
    (flac -t checks its MD5)."""
    import subprocess
    os.makedirs(out, exist_ok=True)

    def ff(*args):
        subprocess.run(["ffmpeg", "-v", "error", "-y"] + list(args), check=True)

    j = lambda name: os.path.join(out, name)
    ff("-f", "lavfi", "-i", "sine=frequency=440:duration=40:sample_rate=44100", "-f", "lavfi", "-i",
       "sine=frequency=660:duration=40:sample_rate=44100", "-filter_complex", "[0][1]amerge=inputs=2",
       "-c:a", "flac", "-sample_fmt", "s32", "-bits_per_raw_sample", "24", j("native.flac"))
    ff("-i", j("native.flac"), "-c:a", "copy", "-strict", "-2", "-movflags", "+faststart", j("moovfirst.mp4"))
    ff("-i", j("native.flac"), "-c:a", "copy", "-strict", "-2", j("moovlast.mp4"))
    ff("-i", j("native.flac"), "-c:a", "libmp3lame", "-b:a", "320k", j("high.mp3"))
    ff("-i", j("native.flac"), "-c:a", "libmp3lame", "-b:a", "128k", j("mid.mp3"))
    ff("-f", "lavfi", "-i", "testsrc=size=600x600", "-frames:v", "1", j("cover.jpg"))
    print("fixtures in %s" % out)


def main():
    if len(sys.argv) > 2 and sys.argv[1] == "--make-fixtures":
        make_fixtures(sys.argv[2])
        return
    print("zvuk mock on %d, files in %s, mode file %s" % (PORT, FILES, MODE_FILE or "-"), flush=True)
    ThreadingHTTPServer(("127.0.0.1", PORT), Handler).serve_forever()


if __name__ == "__main__":
    main()
