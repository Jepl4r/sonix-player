#!/usr/bin/env python3
"""Un finto Podcast Index, per provare il client senza consumare chiamate vere.

    python3 tools/fake_podcastindex.py [porta]

Poi, in device_config.ini:

    [podcast]
    api_base = http://127.0.0.1:8123/api/1.0

e una coppia di chiavi qualunque in streaming-keys.ini: questo server le
accetta tutte, ma CONTROLLA che la firma sia quella giusta per quelle chiavi.

PERCHE' ESISTE

Il gemello di tools/fake_qobuz.py, e per la stessa ragione: le chiamate vere
costano, hanno un tetto giornaliero, e non si possono fare in un ambiente senza
rete. Ma soprattutto -- e questo qui conta piu' che per Qobuz -- il catalogo
vero risponde 401 senza dire perche', quindi con lui solo NON si distingue una
firma sbagliata da un orologio sbagliato da una chiave sbagliata. Questo server
invece lo dice, ed e' quello che serve mentre si scrive la firma.

COSA CONTROLLA

  * X-Auth-Key c'e';
  * X-Auth-Date c'e' ed e' un numero vicino all'ora vera (lo stesso scarto che
    accetta il catalogo);
  * Authorization e' esattamente sha1(chiave + segreto + data), calcolato qui
    con hashlib. Il segreto lo si passa con --secret; senza, si accetta
    qualunque firma e si stampa solo cosa e' arrivato.
  * User-Agent c'e' (il catalogo vero rifiuta senza).

Ogni richiesta si stampa con l'esito del controllo, cosi' si vede a occhio
quale delle quattro cose manca.
"""
import hashlib
import json
import os
import sys
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import parse_qs, urlparse

SECRET = None

# Due podcast e i loro episodi. I campi sono quelli veri dello schema, compresi
# quelli che il client NON legge: servono a provare che leggerne alcuni in
# mezzo a molti funzioni.
FEEDS = [
    {
        "id": 920666,
        "title": "Il Podcast di Prova",
        "url": "https://example.com/feed.xml",
        "author": "Radio Finta",
        "ownerName": "Radio Finta",
        "image": "http://127.0.0.1:8123/art/art1.jpg",
        "artwork": "http://127.0.0.1:8123/art/art1.jpg",
        "episodeCount": 142,
        "language": "it",
        "categories": {"9": "Music"},
    },
    {
        "id": 41504,
        "title": "Another Show",
        "url": "https://example.org/rss",
        "author": "Someone Else",
        "ownerName": "Someone Else",
        "image": "http://127.0.0.1:8123/art/art3.jpg",
        "artwork": "",  # vuoto, non assente: il client deve ripiegare su image
        "episodeCount": 0,
        "language": "en",
    },
]

# GLI IDENTIFICATIVI SONO QUELLI VERI, cioe' a UNDICI CIFRE. Non e' un
# dettaglio del finto: Podcast Index numera gli episodi sopra i dieci miliardi,
# e su un `long` da trentadue bit -- che e' quello del dispositivo -- strtol li
# satura tutti a 2147483647. Con dei numeri piccoli qui dentro quel guasto non
# si vede, e infatti non si e' visto: la prima versione di questo finto usava
# otto cifre e le prove passavano mentre sul lettore partiva sempre lo stesso
# episodio. Chi cambia questi numeri li lasci a undici cifre.
EPISODES = {
    920666: [
        {
            "id": 16795090626,
            "title": "Puntata 142 \u2014 accenti, virgolette \"e\" barre / rovesce \\\\",
            "feedId": 920666,
            "feedTitle": "Il Podcast di Prova",
            "datePublished": 1771200000,
            "duration": 3600,
            "enclosureUrl": "http://127.0.0.1:%d/media/hour.mp3",
            "enclosureType": "audio/mpeg",
            "enclosureLength": 57600626,
            "image": "http://127.0.0.1:8123/art/art2.jpg",
            "feedImage": "http://127.0.0.1:8123/art/art1.jpg",
            "explicit": 0,
        },
        {
            "id": 16795089123,
            "title": "Puntata 141",
            "feedId": 920666,
            "feedTitle": "Il Podcast di Prova",
            "datePublished": 1770595200,
            "duration": 60,
            "enclosureUrl": "http://127.0.0.1:%d/media/ep2.mp3",
            "enclosureType": "audio/mpeg",
            "image": "",  # vuoto: deve ripiegare su feedImage
            "feedImage": "http://127.0.0.1:8123/art/art1.jpg",
        },
        {
            "id": 16795088044,
            "title": "Puntata 140",
            "feedId": 920666,
            "feedTitle": "Il Podcast di Prova",
            "datePublished": 1769990400,
            "duration": 30,
            "enclosureUrl": "http://127.0.0.1:%d/media/ep1.mp3",
            "enclosureType": "audio/mpeg",
            "image": "",
            "feedImage": "http://127.0.0.1:8123/art/art1.jpg",
        },
        {
            # Senza enclosure: il client deve SCARTARLO, non mostrarne una riga
            # che non fa niente.
            "id": 16795087001,
            "title": "Puntata rotta",
            "feedId": 920666,
            "feedTitle": "Il Podcast di Prova",
            "datePublished": 1769385600,
            "duration": 100,
            "enclosureUrl": "",
            "enclosureType": "audio/mpeg",
        },
    ],
    41504: [],
}


# Centotrenta episodi, ognuno con una copertina (le ventiquattro disponibili,
# a rotazione). Due ragioni, tutte e due vissute: le caselle del caricatore
# sono dodici, quindi una lista piu' lunga e' l'unico modo di accorgersi che
# le righe oltre la dodicesima restavano senza immagine; e la pagina e' di
# sessanta, quindi un feed che ne ha centotrenta e' l'unico modo di provare
# che arrivati in fondo se ne chiedono altre -- con meno episodi la
# paginazione "passa" senza essere mai scattata.
for _n in range(4, 131):
    EPISODES[920666].append({
        "id": 16795090000 + _n,
        "title": "Puntata %d" % (200 - _n),
        "feedId": 920666,
        "feedTitle": "Il Podcast di Prova",
        "datePublished": 1769000000 - _n * 86400,
        "duration": 30,
        "enclosureUrl": "http://127.0.0.1:%d/media/ep1.mp3",
        "enclosureType": "audio/mpeg",
        "image": "http://127.0.0.1:8123/art/art" + str((_n - 1) % 24 + 1) + ".jpg",
        "feedImage": "http://127.0.0.1:8123/art/art1.jpg",
    })

PORT = 8123
MEDIA_DIR = os.environ.get("FAKE_PODCAST_MEDIA", "/tmp/podmedia")
SLOW_KBPS = int(os.environ.get("FAKE_PODCAST_SLOW_KBPS", "0"))
ART_DIR = os.environ.get("FAKE_PODCAST_ART", "/tmp/podart")


def signature_ok(headers):
    """Torna (ok, spiegazione)."""
    key = headers.get("X-Auth-Key")
    date = headers.get("X-Auth-Date")
    auth = headers.get("Authorization")
    agent = headers.get("User-Agent")

    if not key:
        return False, "manca X-Auth-Key"
    if not agent:
        return False, "manca User-Agent (il catalogo vero rifiuta senza)"
    if not date:
        return False, "manca X-Auth-Date"
    try:
        when = int(date)
    except ValueError:
        return False, "X-Auth-Date non e' un numero: %r" % date
    drift = abs(int(time.time()) - when)
    if drift > 600:
        return False, "X-Auth-Date e' fuori di %d s (il catalogo accetta pochi minuti)" % drift
    if not auth:
        return False, "manca Authorization"
    if SECRET is None:
        return True, "firma non controllata (nessun --secret), ricevuta %s" % auth[:12]

    want = hashlib.sha1((key + SECRET + date).encode()).hexdigest()
    if auth != want:
        return False, "firma sbagliata: attesa %s, arrivata %s" % (want, auth)
    return True, "firma giusta"


class Handler(BaseHTTPRequestHandler):
    def log_message(self, *args):
        pass  # si stampa da soli, in modo leggibile

    def send_json(self, payload, status=200):
        body = json.dumps(payload).encode()
        self.send_response(status)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def do_GET(self):
        parsed = urlparse(self.path)
        query = parse_qs(parsed.query)

        if parsed.path.startswith("/art/"):
            # Le copertine, servite davvero: con indirizzi finti che rispondono
            # 426 non si puo' vedere se le righe di una lista lunga la
            # copertina la ricevono TUTTE.
            name = os.path.basename(parsed.path)
            full = os.path.join(ART_DIR, name)
            if not os.path.isfile(full):
                self.send_error(404)
                return
            with open(full, "rb") as f:
                body = f.read()
            self.send_response(200)
            self.send_header("Content-Type", "image/jpeg")
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            self.wfile.write(body)
            return

        if parsed.path.startswith("/media/"):
            # MP3 VERI, uno per episodio e di durate diverse (30, 60 e 90
            # secondi): sono il solo modo di accorgersi che a partire e' stato
            # l'episodio sbagliato. Con un unico finto silenzio uguale per tutti
            # l'episodio giusto e quello sbagliato suonano identici, e la prova
            # non prova niente.
            name = os.path.basename(parsed.path)
            full = os.path.join(MEDIA_DIR, name)
            if not os.path.isfile(full):
                self.send_error(404)
                print("  media %s: non ce l'ho" % name)
                return
            with open(full, "rb") as f:
                body = f.read()
            self.send_response(200)
            self.send_header("Content-Type", "audio/mpeg")
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()

            # A RITMO, se richiesto (--slow N = N KB/s). Un episodio vero pesa
            # sessanta megabyte e arriva in minuti: servito tutto in un colpo,
            # il banco non puo' accorgersi di niente che riguardi "il file sta
            # ancora scendendo", che e' esattamente dove stanno i guasti.
            if SLOW_KBPS > 0:
                chunk = SLOW_KBPS * 1024 // 10  # un decimo di secondo alla volta
                sent = 0
                while sent < len(body):
                    self.wfile.write(body[sent:sent + chunk])
                    self.wfile.flush()
                    sent += chunk
                    time.sleep(0.1)
            else:
                self.wfile.write(body)
            print("  media %s: %d byte" % (name, len(body)))
            return

        # `max` si rispetta come fa il catalogo vero: e' cosi' che il client
        # pagina (non c'e' un offset -- si rifa' la richiesta con un tetto
        # piu' alto), e un finto che manda sempre tutto farebbe "passare" la
        # paginazione senza che sia mai scattata.
        def _max(default=60):
            try:
                return max(1, min(1000, int(query.get("max", [str(default)])[0])))
            except ValueError:
                return default

        if parsed.path.endswith("/search/byterm"):
            term = query.get("q", [""])[0]
            hits = [f for f in FEEDS if term.lower() in f["title"].lower()] if term else []
            hits = hits[:_max()]
            self.send_json({"status": "true", "feeds": hits, "count": len(hits), "query": term})
            return

        if parsed.path.endswith("/podcasts/trending"):
            feeds = FEEDS[:_max()]
            self.send_json({"status": "true", "feeds": feeds, "count": len(feeds)})
            return

        if parsed.path.endswith("/episodes/byfeedid"):
            feed_id = int(query.get("id", ["0"])[0])
            items = []
            for e in EPISODES.get(feed_id, [])[:_max()]:
                copy = dict(e)
                if "%d" in copy.get("enclosureUrl", ""):
                    copy["enclosureUrl"] = copy["enclosureUrl"] % PORT
                items.append(copy)
            self.send_json({"status": "true", "items": items, "count": len(items)})
            return

        if parsed.path.endswith("/podcasts/byfeedid"):
            feed_id = int(query.get("id", ["0"])[0])
            found = next((f for f in FEEDS if f["id"] == feed_id), None)
            if not found:
                self.send_json({"status": "false", "description": "no such feed"})
                return
            self.send_json({"status": "true", "feed": found})
            return

        if parsed.path.endswith("/episodes/byid"):
            wanted = int(query.get("id", ["0"])[0])
            for items in EPISODES.values():
                for e in items:
                    if e["id"] == wanted:
                        copy = dict(e)
                        if "%d" in copy.get("enclosureUrl", ""):
                            copy["enclosureUrl"] = copy["enclosureUrl"] % PORT
                        self.send_json({"status": "true", "episode": copy})
                        return
            self.send_json({"status": "false", "description": "no such episode"})
            return

        self.send_json({"status": "false", "description": "endpoint sconosciuto"}, 404)


def main():
    global SECRET, PORT
    args = sys.argv[1:]
    if "--secret" in args:
        i = args.index("--secret")
        SECRET = args[i + 1]
        del args[i : i + 2]
    if args:
        PORT = int(args[0])

    print("finto Podcast Index su http://127.0.0.1:%d/api/1.0" % PORT)
    print("firma: %s" % ("controllata" if SECRET else "NON controllata (passa --secret <segreto>)"))
    # A THREAD, se no servire un episodio a ritmo blocca anche le
    # chiamate all'API: il finto sembrerebbe lento dove il player non lo e'.
    ThreadingHTTPServer(("127.0.0.1", PORT), Handler).serve_forever()


if __name__ == "__main__":
    main()
