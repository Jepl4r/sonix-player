#!/usr/bin/env python3
"""Un finto Qobuz, per provare src/system/streaming/qobuz.c senza un abbonamento.

Non ci sono chiavi vere qui dentro: se ne inventano due e le si mettono anche
nel file streaming-keys.ini che si passa al player. Quello che questo server
verifica sul serio e' la FORMA di quello che il client manda -- e in
particolare la firma MD5 di track/getFileUrl, che e' l'unico pezzo dove un
errore non si vedrebbe se non contro un server che la ricalcola.

    tools/fake_qobuz.py --port 8781 [--media brano.flac]

Le risposte hanno la stessa forma di quelle vere: annidate, con "image" dentro
"album", gli identificativi a volte numeri e a volte stringhe, accenti e
surrogati negli escape \\u. E' apposta: sono i casi su cui un parser scritto
male sbaglia.
"""

import argparse
import hashlib
import json
import os
import re
import sys
import time as _time
import urllib.parse
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

APP_ID = "123456789"
APP_SECRET = "0123456789abcdef0123456789abcdef"
USERNAME = "prova@esempio.it"
PASSWORD = "segreta"
TOKEN = "TOKENDIPROVA0123456789"

MEDIA_PATH = None
SLOW_BYTES_PER_SEC = 0
FAST_HEAD_BYTES = 0
API_DELAY_MS = 0

# Quanti scaricamenti del file audio sono aperti nello stesso momento, e il
# massimo mai visto. Serve a una prova sola ma importante: il player ne deve
# tenere aperto UNO. Cinque insieme erano quello che mandava in crisi il
# dispositivo su un brano in alta risoluzione.
import threading as _threading
MEDIA_LOCK = _threading.Lock()
MEDIA_OPEN = 0
MEDIA_PEAK = 0

# Quello che il player scrive sull'account: i preferiti e il contenuto delle
# playlist. Vivono qui perche' una prova possa guardarli (/_state) invece di
# fidarsi del fatto che una richiesta sia partita.
FAVORITE_TRACKS = set()
FAVORITE_ALBUMS = set()
PLAYLIST_TRACKS = {}

# Titoli scelti per far male al parser: virgolette, barre rovesciate, accenti,
# una chiave di sedicesimo livello, ed emoji fuori dal piano base.
TRACKS = [
    {
        "id": 101,
        "title": "Così parlò il \"mare\"",
        "duration": 251,
        "track_number": 1,
        "hires": True,
        "streamable": True,
        "maximum_bit_depth": 24,
        "maximum_sampling_rate": 96.0,
        "performer": {"id": 9, "name": "Ludovico Müller"},
        "album": {
            "id": "0060254798879",
            "title": "Notturni",
            "artist": {"id": 9, "name": "Ludovico Müller"},
            "image": {"thumbnail": "{HOST}/img/t_50.jpg", "small": "{HOST}/img/s_230.jpg",
                      "large": "{HOST}/img/l_600.jpg"},
        },
    },
    {
        "id": "102",  # a volte Qobuz manda l'identificativo come stringa
        "title": "Barra \\ rovesciata",
        "duration": 187,
        "track_number": 2,
        "hires": False,
        "streamable": True,
        "maximum_bit_depth": 16,
        "maximum_sampling_rate": 44.1,
        "performer": {"id": 12, "name": "🎵 Trio"},
        "album": {
            "id": "0060254798880",
            "title": "Secondo",
            "artist": {"id": 12, "name": "🎵 Trio"},
            "image": {"small": "{HOST}/img/s2.jpg"},
        },
    },
    {
        "id": 103,
        "title": "Non compreso",
        "duration": 300,
        "track_number": 3,
        "hires": True,
        "streamable": False,
        "maximum_bit_depth": 24,
        "maximum_sampling_rate": 192.0,
        "performer": {"name": "Terzo"},
        "album": {
            "id": "0060254798881",
            "title": "Terzo album",
            "artist": {"name": "Terzo"},
            "image": {"small": "{HOST}/img/s3.jpg"},
        },
    },
]

# Una manciata di brani in piu', tutti uguali e tutti suonabili. Servono a una
# prova sola: che una lista lunga si senta FINO IN FONDO. Con tre brani in
# catalogo non ci si accorgeva che la coda si fermava al terzo.
for _n in range(4, 13):
    TRACKS.append({
        "id": 100 + _n,
        "title": "Brano numero %d" % _n,
        "duration": 187,
        "track_number": _n,
        "hires": False,
        "streamable": True,
        "maximum_bit_depth": 16,
        "maximum_sampling_rate": 44.1,
        "performer": {"id": 12, "name": "Coro di prova"},
        "album": {
            "id": "0060254798880",
            "title": "Secondo",
            "artist": {"id": 12, "name": "Coro di prova"},
            "image": {"small": "{HOST}/img/s2_230.jpg", "large": "{HOST}/img/l2_600.jpg"},
        },
    })

ALBUMS = [
    {
        "id": "0060254798879",
        "title": "Notturni",
        "artist": {"id": 9, "name": "Ludovico Müller"},
        "image": {"small": "{HOST}/img/s.jpg"},
        "tracks_count": 2,
        "hires": True,
        "released_at": 1558656000,
        "release_date_original": "2019-05-24",
    },
    {
        "id": "0060254798880",
        "title": "Secondo",
        "artist": {"name": "🎵 Trio"},
        "image": {"small": "{HOST}/img/s2.jpg"},
        "tracks_count": 1,
        "hires": False,
        "released_at": 1400000000,
    },
]

# Centoventi album in vetrina, ognuno con la sua "copertina" (il PNG generato
# porta il nome nel disegno, quindi righe diverse hanno immagini diverse).
# Servono a due cose che con due album non si possono provare: le copertine
# oltre la dodicesima casella, e la paginazione -- la vetrina vera ne ha
# centinaia, e un finto che li manda tutti in una risposta farebbe "passare"
# l'offset senza che sia mai stato usato.
for _n in range(3, 121):
    ALBUMS.append({
        "id": "89%08d" % _n,
        "title": "Raccolta n. %d" % _n,
        "artist": {"id": 9, "name": "Ludovico Müller"},
        "image": {"small": os.environ.get("FAKE_QOBUZ_IMG_BASE", "{HOST}") + "/img/racc%d_230.jpg" % _n},
        "tracks_count": 3,
        "hires": False,
        "released_at": 1500000000 + _n * 86400,
    })

ARTISTS = [
    {"id": 9, "name": "Ludovico Müller", "albums_count": 4,
     "image": {"small": "{HOST}/img/a.jpg"}},
    {"id": 12, "name": "🎵 Trio", "albums_count": 1, "image": None},
]

PLAYLISTS = [
    {"id": 555, "name": "La mia «sera»", "tracks_count": 2,
     "owner": {"id": 1, "name": "prova"}, "images300": ["{HOST}/img/p.jpg"]},
]


def album_with_tracks(album_id):
    album = next((a for a in ALBUMS if a["id"] == album_id), None)
    if album is None:
        return None
    out = dict(album)
    # Dentro /album/get i brani NON ripetono l'album: e' il caso che fa uscire
    # le righe senza copertina se il client non se lo ricorda da solo.
    items = []
    for t in TRACKS:
        if str(t["album"]["id"]) == str(album_id):
            stripped = {k: v for k, v in t.items() if k != "album"}
            items.append(stripped)
    out["tracks"] = {"offset": 0, "limit": 50, "total": len(items), "items": items}
    return out


class Handler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.0"

    def log_message(self, fmt, *args):
        sys.stderr.write("[finto-qobuz] " + fmt % args + "\n")

    # -- risposte -----------------------------------------------------------

    def send_json(self, obj, status=200):
        # Gli indirizzi delle immagini portano il segnaposto {HOST}: qui
        # diventa l'indirizzo vero con la porta vera, che e' l'unico modo
        # perche' il dispositivo ci arrivi.
        host = "http://" + self.headers.get("Host", "127.0.0.1")
        obj = json.loads(json.dumps(obj).replace("{HOST}", host))

        # ensure_ascii: cosi' gli accenti e le emoji escono come \u..., che e'
        # esattamente quello che manda Qobuz e quello che il parser deve saper
        # rimettere insieme.
        body = json.dumps(obj, ensure_ascii=True).encode("ascii")
        self.send_response(status)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def send_error_json(self, status, message):
        self.send_json({"status": "error", "code": status, "message": message}, status)

    # -- controlli ----------------------------------------------------------

    def app_id_ok(self, params):
        if self.headers.get("X-App-Id") != APP_ID:
            self.send_error_json(400, "app_id non valido")
            return False
        return True

    def token_ok(self):
        if self.headers.get("X-User-Auth-Token") != TOKEN:
            self.send_error_json(401, "Sessione scaduta, rifai l'accesso")
            return False
        return True

    # -- instradamento ------------------------------------------------------

    def do_GET(self):
        parsed = urllib.parse.urlparse(self.path)
        params = urllib.parse.parse_qs(parsed.query)
        path = parsed.path

        if MEDIA_PATH and path.startswith("/media/"):
            return self.serve_media()

        # Il ritardo delle chiamate API, per imitare la rete vera: sul
        # dispositivo un getFileUrl costa centinaia di millisecondi, e le
        # corse fra "premo next" e "la risposta di PRIMA che arriva adesso"
        # esistono solo se quel tempo c'e'. Solo sulle API: il file audio ha
        # gia' --slow per conto suo.
        if API_DELAY_MS > 0 and path.startswith("/api.json/"):
            _time.sleep(API_DELAY_MS / 1000.0)

        if path.startswith("/img/"):
            return self.serve_cover()

        if path == "/_peak":
            body = str(MEDIA_PEAK).encode()
            self.send_response(200)
            self.send_header("Content-Type", "text/plain")
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            self.wfile.write(body)
            return

        prefix = "/api.json/0.2"
        if not path.startswith(prefix):
            return self.send_error_json(404, "percorso sconosciuto")
        route = path[len(prefix):]

        if not self.app_id_ok(params):
            return

        def one(name, default=None):
            return params.get(name, [default])[0]

        limit = int(one("limit", "50") or 50)
        offset = int(one("offset", "0") or 0)

        if route == "/user/login":
            return self.do_login()

        if route in ("/track/search", "/album/search", "/artist/search"):
            kind = route.split("/")[1]
            query = one("query", "")
            if not query:
                return self.send_error_json(400, "query mancante")
            data = {"track": ("tracks", TRACKS), "album": ("albums", ALBUMS),
                    "artist": ("artists", ARTISTS)}[kind]
            bucket, rows = data
            window = rows[offset:offset + limit]
            return self.send_json({"query": query, bucket: {
                "limit": limit, "offset": offset, "total": len(rows), "items": window}})

        if route == "/album/get":
            album = album_with_tracks(one("album_id"))
            if album is None:
                return self.send_error_json(404, "Album inesistente")
            return self.send_json(album)

        if route == "/artist/get":
            artist_id = one("artist_id")
            artist = next((a for a in ARTISTS if str(a["id"]) == str(artist_id)), None)
            if artist is None:
                return self.send_error_json(404, "Artista inesistente")
            out = dict(artist)
            out["albums"] = {"limit": limit, "offset": 0, "total": len(ALBUMS), "items": ALBUMS}
            return self.send_json(out)

        if route == "/playlist/get":
            pl = next((p for p in PLAYLISTS if str(p["id"]) == str(one("playlist_id"))), None)
            if pl is None:
                return self.send_error_json(404, "Playlist inesistente")
            out = dict(pl)
            out["tracks"] = {"limit": limit, "offset": 0, "total": len(TRACKS), "items": TRACKS}
            return self.send_json(out)

        if route == "/favorite/getUserFavorites":
            if not self.token_ok():
                return
            kind = one("type", "tracks")
            bucket, rows = {"tracks": ("tracks", TRACKS), "albums": ("albums", ALBUMS),
                            "artists": ("artists", ARTISTS)}.get(kind, ("tracks", TRACKS))
            if kind == "tracks":
                # I preferiti veri: quelli che il player ha messo, non tutto il
                # catalogo. Cosi' la stella del player mostra due stati diversi.
                rows = [t for t in TRACKS if str(t["id"]) in FAVORITE_TRACKS]
            elif kind == "albums":
                rows = [a for a in ALBUMS if str(a["id"]) in FAVORITE_ALBUMS]
            return self.send_json({bucket: {"limit": limit, "offset": offset,
                                            "total": len(rows), "items": rows[offset:offset + limit]}})

        if route == "/playlist/getUserPlaylists":
            if not self.token_ok():
                return
            return self.send_json({"playlists": {"limit": limit, "offset": offset,
                                                 "total": len(PLAYLISTS), "items": PLAYLISTS}})

        if route == "/album/getFeatured":
            # limit e offset si RISPETTANO, come fa il servizio vero: e' l'unico
            # modo di far scattare davvero la paginazione del client.
            page = ALBUMS[offset:offset + limit]
            return self.send_json({"albums": {"limit": limit, "offset": offset,
                                              "total": len(ALBUMS), "items": page}})

        if route == "/track/getFileUrl":
            return self.do_file_url(params)

        # --- scrivere sull'account -----------------------------------------
        #
        # Quello che il player fa quando si tocca la stella nel player o si
        # aggiunge un brano a una playlist. Il finto server tiene lo stato in
        # memoria, cosi' una prova puo' controllare che il brano sia DAVVERO
        # finito dove doveva -- e non solo che la richiesta sia partita.
        if route in ("/favorite/create", "/favorite/delete"):
            if not self.token_ok():
                return
            tracks = [i for i in (one("track_ids", "") or "").split(",") if i]
            albums = [i for i in (one("album_ids", "") or "").split(",") if i]
            if not tracks and not albums:
                return self.send_error_json(400, "track_ids o album_ids mancante")
            add = route.endswith("create")
            for track_id in tracks:
                (FAVORITE_TRACKS.add if add else FAVORITE_TRACKS.discard)(str(track_id))
            for album_id in albums:
                (FAVORITE_ALBUMS.add if add else FAVORITE_ALBUMS.discard)(str(album_id))
            self.log_message("preferiti adesso: brani %s album %s",
                             sorted(FAVORITE_TRACKS), sorted(FAVORITE_ALBUMS))
            return self.send_json({"status": "success"})

        if route == "/playlist/delete":
            if not self.token_ok():
                return
            playlist_id = str(one("playlist_id", ""))
            before = len(PLAYLISTS)
            PLAYLISTS[:] = [p for p in PLAYLISTS if str(p["id"]) != playlist_id]
            if len(PLAYLISTS) == before:
                return self.send_error_json(404, "Playlist inesistente")
            PLAYLIST_TRACKS.pop(playlist_id, None)
            self.log_message("cancellata playlist %s", playlist_id)
            return self.send_json({"status": "success"})

        if route == "/playlist/create":
            if not self.token_ok():
                return
            name = one("name", "")
            if not name:
                return self.send_error_json(400, "name mancante")
            new_id = max([int(p["id"]) for p in PLAYLISTS] + [1000]) + 1
            PLAYLISTS.append({"id": new_id, "name": name, "tracks_count": 0,
                              "owner": {"id": 42, "name": "Prova Utente"}})
            PLAYLIST_TRACKS[str(new_id)] = []
            self.log_message("creata playlist %d \"%s\"", new_id, name)
            return self.send_json({"id": new_id, "name": name, "tracks_count": 0})

        if route == "/playlist/addTracks":
            if not self.token_ok():
                return
            playlist_id = str(one("playlist_id", ""))
            pl = next((p for p in PLAYLISTS if str(p["id"]) == playlist_id), None)
            if pl is None:
                return self.send_error_json(404, "Playlist inesistente")
            ids = [i for i in (one("track_ids", "") or "").split(",") if i]
            if not ids:
                return self.send_error_json(400, "track_ids mancante")
            PLAYLIST_TRACKS.setdefault(playlist_id, []).extend(ids)
            pl["tracks_count"] = len(PLAYLIST_TRACKS[playlist_id])
            self.log_message("playlist %s adesso: %s", playlist_id, PLAYLIST_TRACKS[playlist_id])
            return self.send_json({"id": int(playlist_id), "tracks_count": pl["tracks_count"]})

        # Uno sguardo da fuori a com'e' rimasto l'account, per le prove.
        if route == "/_state":
            return self.send_json({"favorites": sorted(FAVORITE_TRACKS),
                                   "favorite_albums": sorted(FAVORITE_ALBUMS),
                                   "playlists": {str(p["id"]): {"name": p["name"],
                                                                "tracks": PLAYLIST_TRACKS.get(str(p["id"]), [])}
                                                 for p in PLAYLISTS}})

        return self.send_error_json(404, "endpoint sconosciuto: " + route)

    # -- accesso ------------------------------------------------------------

    def do_login(self):
        user = self.headers.get("username")
        password = self.headers.get("password")
        manufacturer = self.headers.get("device_manufacturer_id")

        if manufacturer != APP_SECRET:
            return self.send_error_json(400, "device_manufacturer_id mancante o sbagliato")
        if user != USERNAME or password != PASSWORD:
            return self.send_error_json(401, "Nome utente o password non validi")

        return self.send_json({
            "user_auth_token": TOKEN,
            "user": {
                "id": 4242,
                "login": USERNAME,
                "display_name": "Prova Utente",
                "email": USERNAME,
                "country_code": "IT",
                "credential": {"label": "Studio Premier",
                               "parameters": {"lossy_streaming": True, "lossless_streaming": True,
                                              "hires_streaming": True}},
            },
        })

    # -- l'URL firmato ------------------------------------------------------

    def do_file_url(self, params):
        if not self.token_ok():
            return

        def one(name, default=""):
            return params.get(name, [default])[0]

        track_id = one("track_id")
        format_id = one("format_id")
        ts = one("request_ts")
        got = one("request_sig")

        if not (track_id and format_id and ts and got):
            return self.send_error_json(400, "parametri della firma mancanti")

        # QUESTO e' il controllo che conta, e si fa con la REGOLA, non con una
        # stringa scritta a mano:
        #
        #     MD5( oggetto metodo
        #          chiave+valore di ogni parametro ricevuto, in ordine alfabetico
        #          request_ts app_secret )
        #
        # Rifarla sui parametri effettivamente arrivati e' l'unico modo di
        # accorgersi che il client ne manda uno che non ha firmato -- che e'
        # esattamente come si rompe: `intent=stream` nell'indirizzo e non nella
        # firma, e Qobuz risponde "Invalid Request Signature parameter".
        signed = {k: v[0] for k, v in params.items()
                  if k not in ("app_id", "user_auth_token", "request_ts", "request_sig")}
        raw = "track" + "getFileUrl"
        for key in sorted(signed):
            raw += key + signed[key]
        raw += ts + APP_SECRET

        want = hashlib.md5(raw.encode("utf-8")).hexdigest()
        if got != want:
            self.log_message("firma sbagliata: attesa %s, arrivata %s (su <%s>)", want, got, raw)
            return self.send_error_json(400, "Invalid Request Signature parameter (request_sig)")

        track = next((t for t in TRACKS if str(t["id"]) == str(track_id)), None)
        if track is None:
            return self.send_error_json(404, "Brano inesistente")
        if not track["streamable"]:
            # Il caso che conta davvero: 200, ma senza url.
            return self.send_json({"track_id": int(track_id), "streamable": False,
                                   "restrictions": [{"code": "TrackNotAvailable"}]})

        host = self.headers.get("Host", "127.0.0.1")
        return self.send_json({
            "track_id": int(track_id),
            "duration": track["duration"],
            "url": "http://%s/media/%s.flac" % (host, track_id),
            "format_id": int(format_id),
            "mime_type": "audio/flac" if format_id != "5" else "audio/mpeg",
            "restrictions": [],
            "sampling_rate": track["maximum_sampling_rate"],
            "bit_depth": track["maximum_bit_depth"],
        })

    # -- il file audio ------------------------------------------------------

    # Una copertina qualsiasi, generata al volo: un PNG 300x300 a tinta unita.
    # Serve solo a vedere che arriva e che il player la mostra.
    def serve_cover(self):
        import struct as _struct
        import zlib as _zlib

        # La misura sta nel nome, come nelle URL vere di Qobuz ("..._600.jpg"):
        # cosi' una prova puo' controllare QUALE misura il client ha chiesto,
        # che e' l'unico modo di accorgersi se torna a prendere la piccola.
        side = 300
        m = re.search(r"_(\d+)\.", self.path)
        if m:
            side = max(16, min(1200, int(m.group(1))))
        rows = b"".join(b"\x00" + bytes([40, 90, 160] * side) for _ in range(side))

        def chunk(kind, data):
            return (_struct.pack(">I", len(data)) + kind + data
                    + _struct.pack(">I", _zlib.crc32(kind + data) & 0xFFFFFFFF))

        png = (b"\x89PNG\r\n\x1a\n"
               + chunk(b"IHDR", _struct.pack(">IIBBBBB", side, side, 8, 2, 0, 0, 0))
               + chunk(b"IDAT", _zlib.compress(rows, 6))
               + chunk(b"IEND", b""))

        self.send_response(200)
        self.send_header("Content-Type", "image/png")
        self.send_header("Content-Length", str(len(png)))
        self.end_headers()
        self.wfile.write(png)

    def serve_media(self):
        global MEDIA_OPEN, MEDIA_PEAK
        with MEDIA_LOCK:
            MEDIA_OPEN += 1
            MEDIA_PEAK = max(MEDIA_PEAK, MEDIA_OPEN)
            self.log_message("scaricamenti aperti: %d (massimo %d)", MEDIA_OPEN, MEDIA_PEAK)
        try:
            self._serve_media_body()
        finally:
            with MEDIA_LOCK:
                MEDIA_OPEN -= 1

    def _serve_media_body(self):
        try:
            size = os.path.getsize(MEDIA_PATH)
            with open(MEDIA_PATH, "rb") as f:
                data = f.read()
        except OSError as e:
            return self.send_error_json(404, "niente file: %s" % e)

        self.send_response(200)
        self.send_header("Content-Type", "audio/flac")
        self.send_header("Content-Length", str(size))
        self.end_headers()

        if SLOW_BYTES_PER_SEC <= 0:
            self.wfile.write(data)
            return

        # A rate umane: serve a provare che chi legge il file mentre cresce
        # aspetta il pezzo mancante invece di credere che il brano sia finito.
        #
        # FAST_HEAD_BYTES > 0 e' il Wi-Fi vero: il primo tratto arriva di colpo
        # dal buffer (e la misura della rete dice "velocissima"), POI si va al
        # ritmo di --slow. E' la condizione che sul dispositivo faceva partire
        # il brano con poca scorta e lo mandava a sbattere sul bordo.
        import time as _time
        step = max(4096, SLOW_BYTES_PER_SEC // 10)
        start = 0
        if FAST_HEAD_BYTES > 0:
            head = min(FAST_HEAD_BYTES, len(data))
            self.wfile.write(data[:head])
            self.wfile.flush()
            start = head
        while start < len(data):
            self.wfile.write(data[start:start + step])
            self.wfile.flush()
            start += step
            _time.sleep(step / float(SLOW_BYTES_PER_SEC))


def main():
    global MEDIA_PATH
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", type=int, default=8781)
    ap.add_argument("--media", help="file audio da servire come brano")
    ap.add_argument("--user", default=USERNAME, help="nome utente accettato")
    ap.add_argument("--password", default=PASSWORD, help="password accettata")
    ap.add_argument("--slow", type=int, default=0,
                    help="byte al secondo per il file audio (0 = a tutta velocita')")
    ap.add_argument("--fast-head", type=int, default=0,
                    help="byte serviti a tutta velocita' PRIMA che --slow entri: imita il "
                         "Wi-Fi vero, dove il primo tratto arriva dal buffer e inganna la "
                         "misura della rete")
    ap.add_argument("--duration", type=int, default=0,
                    help="durata in secondi da dichiarare per ogni brano; serve quando --media "
                         "e' un file solo che fa da brano per tutti, perche' il pre-caricamento "
                         "si calcola da durata e dimensione e con due numeri incoerenti la prova "
                         "non prova niente")
    ap.add_argument("--api-delay", type=int, default=0,
                    help="millisecondi di ritardo su ogni chiamata API (non sul file audio): "
                         "imita la latenza della rete vera, dove un getFileUrl costa centinaia "
                         "di ms e le corse coi next veloci esistono davvero")
    args = ap.parse_args()
    MEDIA_PATH = args.media
    globals()["SLOW_BYTES_PER_SEC"] = args.slow
    globals()["FAST_HEAD_BYTES"] = args.fast_head
    globals()["API_DELAY_MS"] = args.api_delay
    if args.duration > 0:
        for track in TRACKS:
            track["duration"] = args.duration
    # Comodi per la prova a video: sulla tastiera del dispositivo scrivere una
    # email intera e' una ventina di tocchi, e non e' quello che si sta
    # provando.
    globals()["USERNAME"] = args.user
    globals()["PASSWORD"] = args.password

    server = ThreadingHTTPServer(("127.0.0.1", args.port), Handler)
    sys.stderr.write("[finto-qobuz] in ascolto su http://127.0.0.1:%d/api.json/0.2\n" % args.port)
    sys.stderr.write("[finto-qobuz] app_id=%s utente=%s password=%s\n" % (APP_ID, USERNAME, PASSWORD))
    server.serve_forever()


if __name__ == "__main__":
    main()
