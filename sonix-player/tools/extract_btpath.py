#!/usr/bin/env python3
"""Estrae da src/system/audio/audio.c il pezzo di percorso audio che riguarda il
Bluetooth: come si chiude un PCM alla fine di una traccia e come ci si scrive.

Il banco prova (tools/test_bttrack.c) include il file generato e lo fa girare
contro un server bluealsa vero, cosi' misura esattamente il codice che gira sul
lettore e non una copia riscritta a mano.

Cosa viene preso, e nient'altro: le tre costanti dei tempi, pcm_is_bluetooth(),
pcm_drain_bounded() e pcm_write_bluetooth(). Le uniche cose che chiedono
all'esterno sono log_ms() e playback_reader_should_abort(), che il banco
fornisce.

Uso: extract_btpath.py src/system/audio/audio.c > /tmp/btpath_extracted.h
"""

import re
import sys

DEFINES = ["DRAIN_TIMEOUT_MS", "DRAIN_STALL_MS", "BT_WRITE_STALL_MS"]

FUNCS = ["pcm_is_bluetooth", "pcm_drain_bounded", "pcm_write_bluetooth"]


def take_define(src, name):
    m = re.search(r"^#define\s+%s\s+.*$" % re.escape(name), src, re.M)
    if not m:
        sys.exit("manca #define %s in audio.c" % name)
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
        sys.exit("manca la funzione %s in audio.c" % name)
    return take_block(src, m.start())


def main():
    if len(sys.argv) != 2:
        sys.exit(__doc__)
    src = open(sys.argv[1], encoding="utf-8").read()

    out = ["/* generato da tools/extract_btpath.py -- non modificare */", ""]
    for d in DEFINES:
        out.append(take_define(src, d))
    out.append("")

    for f in FUNCS:
        out.append(take_func(src, f))
        out.append("")

    sys.stdout.write("\n".join(out))


main()
