#!/usr/bin/env python3
"""Estrae dal src/system/audio/audio.c VERO le funzioni del percorso di scrittura.

Non una copia scritta a mano: le righe che finiscono nel banco sono quelle che
girano sul dispositivo. Se audio.c cambia e il banco non compila piu', e' il
banco ad avere ragione.

Uso: extract_write_path.py <audio.c> <write_path.inc>
"""

import re
import sys

WANTED = [
    "pcm_is_bluetooth",
    "pcm_write_bluetooth",
    "pcm_write_recover",
]

DEFINES = [
    "BT_WRITE_STALL_MS",
]


def find_function(text, name):
    """Il corpo della funzione, dalla riga della firma alla graffa che la chiude."""
    pattern = re.compile(
        r"^(?:static\s+)?[A-Za-z_][A-Za-z0-9_ \t*]*\b" + re.escape(name) + r"\s*\([^;]*?\)\s*\{",
        re.M | re.S,
    )
    m = pattern.search(text)
    if not m:
        raise SystemExit("non trovo %s in audio.c" % name)
    start = m.start()
    depth = 0
    i = m.end() - 1
    while i < len(text):
        if text[i] == "{":
            depth += 1
        elif text[i] == "}":
            depth -= 1
            if depth == 0:
                return text[start : i + 1]
        i += 1
    raise SystemExit("la funzione %s non si chiude" % name)


def find_define(text, name):
    m = re.search(r"^#define\s+" + re.escape(name) + r"\s+.*$", text, re.M)
    if not m:
        raise SystemExit("non trovo #define %s" % name)
    return m.group(0)


def main():
    if len(sys.argv) != 3:
        raise SystemExit(__doc__)
    text = open(sys.argv[1]).read()

    out = ["// Generato da tools/extract_write_path.py: NON modificare a mano.",
           "// Sono le funzioni vere di src/system/audio/audio.c.", ""]
    for name in DEFINES:
        out.append(find_define(text, name))
    out.append("")
    for name in WANTED:
        out.append(find_function(text, name))
        out.append("")

    open(sys.argv[2], "w").write("\n".join(out))
    print("estratte %d funzioni e %d define" % (len(WANTED), len(DEFINES)))


if __name__ == "__main__":
    main()
