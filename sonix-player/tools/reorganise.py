#!/usr/bin/env python3
"""Sposta i sorgenti in sottocartelle per funzione e riscrive gli #include.

    tools/reorganise.py plan     dice cosa farebbe
    tools/reorganise.py apply    lo fa

src/gui e src/system erano due cartelle da quasi ottanta file l'una. Il
Makefile trova i sorgenti con `find src -name '*.c'`, quindi annidarli non gli
cambia niente; quello che cambia sono gli #include, che in questo progetto si
scrivono dalla radice ("src/gui/theme.h") e quindi portano dentro il percorso.

src/ebook finisce in src/system/ebook: e' il lettore di EPUB -- zip, xml, css,
arena -- cioe' sistema. Le pagine che lo mostrano stanno gia' in gui.

src/gb, src/system/db, src/system/decode, src/system/image e src/gui/fonts non
si toccano: sono gia' raggruppate, e le prime tre sono codice di terzi che
include i propri header per nome.
"""
import os
import re
import shutil
import sys
import glob

GUI = {
    # Il telaio: quello che c'e' prima di qualunque pagina, e i pezzi che ogni
    # pagina riusa per costruirsi.
    "shell": """gui switcher topbar theme icons settingsrow popover confirm toast spinner
               scrolltext keyboard gridpage main_menu morepage quickpanel volume_overlay
               easteregg welcome powermenu""",
    # La schermata di riproduzione e tutto quello che ci si vede dentro.
    "nowplaying": "player cover coverloader coverflow trackmenu chapters",
    # Sfogliare quello che c'e' sulla scheda.
    "library": "browser medialist music playlistpage search libraryscan filespage audiobooks",
    # Le pagine delle impostazioni.
    "settings": """settings appearance musicsettings powersettings systempage devoptions
                  processespage language kblayoutpage remap screensaver timeset ccsettings""",
    # Le pagine che modellano il suono.
    "audio": "eqsettings peqpage peqsettings msebsettings dacpage",
    "bluetooth": "btsettings btaudio btreceiverpage airpodspage",
    "wireless": "wireless wifisettings wifitransfer airplay dlna sonixlink",
    "streaming": "streaming qobuzpage qobuzart tidalpage podcastpage radiopage",
    "ebook": """ebookbar ebookcovers ebookfonts ebookmarkspage ebookpage ebookreader
               ebooksettings""",
    "gearboy": "gearboypage gearboyplay gearboysettings",
}

SYSTEM = {
    # Quello che serve a tutti e non appartiene a nessuno.
    "core": "config utils logging lang json base64 md5 sha1 glibc_compat panel",
    # Il suono: dalla sorgente al DAC.
    "audio": """audio swvolume alsa-controls replaygain eq speed waveform headset
               usbaudio usbdac""",
    # Cosa si sta riproducendo e cosa viene dopo.
    "playback": "device_state playlist sleeptimer audiobook",
    # Cosa c'e' sulla scheda e cosa ne sappiamo.
    "library": "library playlists metadata albumart cue id3chap mp4flac audiobookdb jpeginfo",
    "bluetooth": "bluetooth btstack btplayer btreceiver btvolume airpods dbuslite",
    "net": "http tls hls mdns wifi wifitransfer webcgi webpage",
    "streaming": """qobuz qobuzcache qobuzsync tidal tidalcache tidalsync podcast
                   podcastcache podcastsubs radio streamkeys streamturn""",
    # Il lettore comandato o ascoltato da un'altra macchina.
    "remote": "sonixlink airplay dlna",
    # Il dispositivo in quanto tale.
    "device": """power led clock factoryreset sysinfo system sysserver firmware adb
                screenshot bootlogo screensaverpics usb""",
    "input": "keymap kblayout hookclicks",
    "gearboy": "gearboy gbdb gbinput",
}

# Gia' raggruppate, o codice di terzi che include i propri header per nome.
KEEP = {"fonts", "db", "decode", "image", "alac", "miniz", "tjpgd", "core", "audio"}


def plan():
    """(vecchio percorso, nuovo percorso) per ogni file che si sposta."""
    moves = []
    for area, table in (("gui", GUI), ("system", SYSTEM)):
        assigned = {}
        for folder, names in table.items():
            for name in names.split():
                assert name not in assigned, (area, name, "in due cartelle")
                assigned[name] = folder
        here = set()
        for p in glob.glob("src/%s/*.c" % area) + glob.glob("src/%s/*.h" % area):
            here.add(os.path.basename(p).rsplit(".", 1)[0])
        missing = sorted(here - set(assigned))
        extra = sorted(set(assigned) - here)
        if missing:
            sys.exit("src/%s: non so dove mettere %s" % (area, missing))
        if extra:
            sys.exit("src/%s: nominati ma non esistono %s" % (area, extra))
        for p in sorted(glob.glob("src/%s/*.c" % area) + glob.glob("src/%s/*.h" % area)):
            stem = os.path.basename(p).rsplit(".", 1)[0]
            moves.append((p, "src/%s/%s/%s" % (area, assigned[stem], os.path.basename(p))))

    # Il lettore di EPUB, da src/ebook a src/system/ebook.
    for p in sorted(glob.glob("src/ebook/*")):
        if os.path.isfile(p):
            moves.append((p, "src/system/ebook/" + os.path.basename(p)))
    return moves


def rewrite_includes(moves):
    """Da "src/gui/theme.h" a "src/gui/shell/theme.h", ovunque sia scritto.

    Anche gli include locali per nome: un file che includeva un vicino con
    "utils.h" ora ce l'ha in un'altra cartella, e la forma dalla radice -- che
    e' quella che il progetto usa quasi ovunque -- non dipende da dove si trova
    chi include.
    """
    old_to_new = {}
    for old, new in moves:
        old_to_new[old] = new
        old_to_new[os.path.basename(old)] = new

    sources = [p for p in glob.glob("src/**/*.c", recursive=True) +
               glob.glob("src/**/*.h", recursive=True) +
               glob.glob("src/**/*.cpp", recursive=True)]
    changed = 0
    for p in sources:
        if "/db/" in p or "/gb/" in p or "/image/" in p or "/decode/alac/" in p:
            continue
        text = open(p, encoding="utf-8", errors="replace").read()
        original = text

        def fix(m):
            inc = m.group(1)
            if inc in old_to_new and inc.startswith("src/"):
                return '#include "%s"' % old_to_new[inc]
            # forma locale: "utils.h", ma solo se e' un nostro header spostato
            if "/" not in inc and inc in old_to_new:
                own = os.path.basename(p).rsplit(".", 1)[0] + ".h"
                if inc != own:
                    return '#include "%s"' % old_to_new[inc]
            return m.group(0)

        text = re.sub(r'#include "([^"]+)"', fix, text)
        if text != original:
            open(p, "w", encoding="utf-8").write(text)
            changed += 1
    return changed


def main():
    if len(sys.argv) < 2 or sys.argv[1] not in ("plan", "apply"):
        print(__doc__)
        return 2
    moves = plan()
    print("%d file da spostare" % len(moves))
    if sys.argv[1] == "plan":
        for old, new in moves[:12]:
            print("   %-38s -> %s" % (old, new))
        print("   ...")
        return 0

    for old, new in moves:
        os.makedirs(os.path.dirname(new), exist_ok=True)
        shutil.move(old, new)
    for d in ("src/ebook",):
        if os.path.isdir(d) and not os.listdir(d):
            os.rmdir(d)
    print("include riscritti in %d file" % rewrite_includes(moves))
    return 0


if __name__ == "__main__":
    sys.exit(main())
