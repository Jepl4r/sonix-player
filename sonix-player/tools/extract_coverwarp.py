#!/usr/bin/env python3
"""Estrae da src/gui/nowplaying/coverflow.c la parte che calcola la prospettiva.

Il banco prova (tools/test_coverwarp.c) include il file generato, cosi' misura
esattamente il codice che gira sul lettore e non una copia riscritta a mano che
puo' divergere alla prima modifica.

Cosa viene preso, e nient'altro: le costanti della geometria,
height_to_far_pct() e warp_square(). Sono autonome -- non toccano LVGL, non
toccano il database, non toccano nessuno stato della pagina.

Uso: extract_coverwarp.py src/gui/nowplaying/coverflow.c > /tmp/coverwarp_extracted.h
"""

import re
import sys

DEFINES = [
    "CF_MID_W",
    "CF_LEAN_H",
    "CF_FAR_PCT",
    "CF_FAR_SHADE",
]

FUNCS = ["isqrt16", "height_to_far", "warp_square"]


def take_define(src, name):
    m = re.search(r"^#define\s+%s\s+.*$" % re.escape(name), src, re.M)
    if not m:
        sys.exit("manca #define %s in coverflow.c" % name)
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
    m = re.search(r"^static [^\n;]*\b%s\(" % re.escape(name), src, re.M)
    if not m:
        sys.exit("manca la funzione %s in coverflow.c" % name)
    return take_block(src, m.start())


def main():
    if len(sys.argv) != 2:
        sys.exit(__doc__)
    src = open(sys.argv[1], encoding="utf-8").read()

    out = ["/* generato da tools/extract_coverwarp.py -- non modificare */", ""]
    for d in DEFINES:
        out.append(take_define(src, d))
    out.append("")

    for f in FUNCS:
        out.append(take_func(src, f))
        out.append("")

    sys.stdout.write("\n".join(out))


main()
