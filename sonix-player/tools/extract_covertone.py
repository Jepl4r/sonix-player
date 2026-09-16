#!/usr/bin/env python3
"""Estrae da src/gui/nowplaying/cover.c la funzione che decide il colore del bagliore.

Il banco (tools/test_covertone.c) include il file generato, cosi' misura la
funzione che gira davvero sul lettore e non una copia riscritta a mano: il
difetto che questo banco esiste per sorvegliare -- le copertine in bianco e nero
che accendevano un bagliore verde -- stava in due righe di espansione RGB565,
esattamente il genere di cosa che una copia a mano sistema per sbaglio.

Cosa viene preso, e nient'altro: le due costanti della soglia e dominant_tone().
La struttura cover_image_t la fornisce il banco.

Uso: extract_covertone.py src/gui/nowplaying/cover.c > /tmp/covertone_extracted.h
"""

import re
import sys

DEFINES = ["TONE_CHROMA_FLOOR", "TONE_MONO"]

FUNCS = ["cover_dominant_tone"]


def take_define(src, name):
    m = re.search(r"^#define\s+%s\s+.*$" % re.escape(name), src, re.M)
    if not m:
        sys.exit("manca #define %s in cover.c" % name)
    return m.group(0)


def take_block(src, start):
    """Dal punto dato fino alla graffa che chiude, contandole."""
    depth = 0
    j = src.index("{", start)
    while True:
        if src[j] == "{":
            depth += 1
        elif src[j] == "}":
            depth -= 1
            if depth == 0:
                return src[start:j + 1]
        j += 1


def take_func(src, name):
    m = re.search(r"^(?:static )?[^\n;]*\b%s\(" % re.escape(name), src, re.M)
    if not m:
        sys.exit("manca la funzione %s in cover.c" % name)
    return take_block(src, m.start())


def main():
    if len(sys.argv) != 2:
        sys.exit(__doc__)
    src = open(sys.argv[1], encoding="utf-8").read()

    out = ["/* generato da tools/extract_covertone.py -- non modificare */", ""]
    for d in DEFINES:
        out.append(take_define(src, d))
    out.append("")

    for f in FUNCS:
        out.append(take_func(src, f))
        out.append("")

    sys.stdout.write("\n".join(out))


main()
