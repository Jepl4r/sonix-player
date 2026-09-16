#!/usr/bin/env python3
"""Checks a language file against the tags the source actually uses.

    tools/make_language_ini.py check English.ini [Italiano.ini]
    tools/make_language_ini.py skeleton

DALL'ERA DEI TAG IN POI (round 96): nel sorgente non ci sono piu' stringhe
italiane -- ci sono tag ("search_for_a_podcast"), e ogni lingua li traduce nel
suo .ini, Italiano.ini compreso. Il testo italiano vive SOLO in Italiano.ini,
che quindi non si genera piu' dal sorgente: e' un file mantenuto, come gli
altri quattro.

Il controllo resta quello che evita i due guai veri:
  * MANCA: un tag usato dal sorgente senza una riga nel file = il tag nudo
    sullo schermo;
  * FORMATI: una traduzione che perde o inventa un "%s" = un crash, non un
    refuso. I formati si confrontano con quelli di Italiano.ini, che e' il
    riferimento -- il tag non ne ha e non puo' fare da metro.

`skeleton` stampa "tag = " per ogni tag del sorgente: la base per una lingua
nuova, o per vedere al volo cosa manca.
"""
import re
import sys
import os

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import extract_strings as E  # noqa: E402

import glob


def c_unescape(s):
    """Source text to the bytes the program actually holds.

    The extractor hands back what is written between the quotes, so an accented
    letter arrives as the `\\xC3\\xA8` the source spells it with. tr() is called
    with the real bytes, so the key in the file has to be the real bytes too.
    """
    out = bytearray()
    i, n = 0, len(s)
    simple = {"n": 10, "t": 9, "r": 13, "\\": 92, '"': 34, "'": 39, "0": 0}
    while i < n:
        c = s[i]
        if c != "\\" or i + 1 >= n:
            out.extend(c.encode("utf-8"))
            i += 1
            continue
        nxt = s[i + 1]
        if nxt == "x":
            j = i + 2
            hexpart = ""
            while j < n and len(hexpart) < 2 and s[j] in "0123456789abcdefABCDEF":
                hexpart += s[j]
                j += 1
            if hexpart:
                out.append(int(hexpart, 16))
                i = j
                continue
        if nxt in simple:
            out.append(simple[nxt])
            i += 2
            continue
        out.extend(nxt.encode("utf-8"))
        i += 2
    return out.decode("utf-8", "replace")


def ini_escape(s):
    """One line per entry, so the two characters that would break that go out."""
    return s.replace("\\", "\\\\").replace("\n", "\\n").replace("\t", "\\t")


def specifiers(s):
    """What a translation has to keep byte for byte.

    Two kinds: the C printf conversions, and the {1} {2} the Wi-Fi transfer
    page fills in from JavaScript. The braces are sorted because a translation
    IS allowed to put them in a different order -- that is the whole reason
    they are numbered -- while the printf ones are positional and are not.
    """
    printf = re.findall(r"%[-+ #0-9.]*[a-zA-Z]", s)
    braces = sorted(re.findall(r"\{\d\}", s))
    return printf + braces


def source_strings():
    paths = sorted(
        p for p in glob.glob("src/gui/**/*.c", recursive=True) + glob.glob("src/system/**/*.c", recursive=True)
        if not p.endswith("icons.c")
    )
    E.collect_root[0] = "."
    found, _, _ = E.collect(paths)
    out = []
    for raw, path in found:
        out.append((c_unescape(raw), os.path.basename(path)))
    return out


def read_ini(path):
    entries = {}
    for line in open(path, encoding="utf-8"):
        line = line.strip()
        if not line or line[0] in "#;[":
            continue
        if "=" not in line:
            continue
        key, value = line.split("=", 1)
        entries[key.strip()] = value.strip()
    return entries


def main():
    if len(sys.argv) < 2:
        print(__doc__)
        return 2

    if sys.argv[1] == "skeleton":
        strings = source_strings()
        # The header names the language for the settings list; the player reads
        # it instead of guessing from the file name (see lang_display_name).
        name = sys.argv[2] if len(sys.argv) > 2 else "English"
        print("[Language: %s]" % name)
        last = None
        for text, where in strings:
            if where != last:
                print("\n; --- %s" % where)
                last = where
            print("%s = " % ini_escape(text))
        return 0

    if sys.argv[1] == "check":
        strings = source_strings()
        wanted = [t for t, _ in strings]
        have = read_ini(sys.argv[2])
        # Il riferimento per i formati: l'italiano. Controllando Italiano.ini
        # stesso, il riferimento e' il file sotto esame.
        ref_path = sys.argv[3] if len(sys.argv) > 3 else os.path.join(os.path.dirname(sys.argv[2]), "Italiano.ini")
        ref = read_ini(ref_path) if os.path.exists(ref_path) else have

        problems = 0
        for text in wanted:
            key = ini_escape(text)
            if key not in have:
                print("MANCA:      %s" % key)
                problems += 1
                continue
            got = have[key]
            want_spec = specifiers(ref.get(key, got))
            if want_spec != specifiers(got):
                print("FORMATI:    %s\n            attesi %s -> %s" % (key, want_spec, got))
                problems += 1
        wanted_keys = {ini_escape(t) for t in wanted}
        # Le chiavi del template web restano testi italiani e vivono nel file
        # anche quando web/index.html non sta in questo albero (il template e'
        # incorporato in webpage.h): si leggono da li'.
        web_keys = set()
        try:
            page = open("src/system/net/webpage.h", encoding="utf-8", errors="replace").read()
            body = "".join(re.findall(r'"((?:[^"\\]|\\.)*)"', page))
            body = body.replace('\\"', '"').replace("\\n", "\n").replace("\\\\", "\\")
            for m in re.finditer(r"\{\{(.*?)\}\}", body, re.S):
                k = m.group(1)
                web_keys.add(k[3:] if k.startswith("js|") else k)
        except OSError:
            pass
        for key in have:
            if key not in wanted_keys and key not in web_keys:
                # Le altre chiavi italiane della pagina web si riconoscono
                # perche' non hanno la forma di un tag.
                if re.fullmatch(r"[a-z0-9_]+", key):
                    print("IN PIU':    %s" % key)
                    problems += 1
        print("\n%d tag nel sorgente, %d voci nel file, %d problemi" % (len(wanted), len(have), problems))
        return 1 if problems else 0

    print(__doc__)
    return 2


if __name__ == "__main__":
    sys.exit(main())
