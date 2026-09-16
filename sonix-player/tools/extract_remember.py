#!/usr/bin/env python3
"""Estrae da src/system/playback/device_state.c la decisione di "Ricorda traccia".

Il banco prova (tools/test_remember.c) include il file generato, cosi' misura
la regola vera e non una copia riscritta a mano.

Cosa viene preso, e nient'altro: le tre costanti, il tipo remember_state_t e
remember_should_write(). E' pura -- non tocca il decoder, non tocca il
database, non guarda l'orologio: tutto quello che le serve sta negli argomenti.

Uso: extract_remember.py src/system/playback/device_state.c > /tmp/remember_extracted.h
"""

import re
import sys

DEFINES = ["REMEMBER_PERIOD_MS", "REMEMBER_MOVE_SECS", "REMEMBER_RESTORE_GRACE_MS"]


def take_define(src, name):
    m = re.search(r"^#define\s+%s\s+.*$" % re.escape(name), src, re.M)
    if not m:
        sys.exit("manca #define %s in device_state.c" % name)
    return m.group(0)


def take_block(src, start):
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


def main():
    if len(sys.argv) != 2:
        sys.exit(__doc__)
    src = open(sys.argv[1], encoding="utf-8").read()

    out = ["/* generato da tools/extract_remember.py -- non modificare */", ""]
    for d in DEFINES:
        out.append(take_define(src, d))
    out.append("")

    m = re.search(r"typedef struct \{[^}]*\} remember_state_t;", src, re.S)
    if not m:
        sys.exit("manca il typedef remember_state_t in device_state.c")
    out.append(m.group(0))
    out.append("")

    m = re.search(r"^static bool remember_should_write\(", src, re.M)
    if not m:
        sys.exit("manca remember_should_write() in device_state.c")
    out.append(take_block(src, m.start()))
    out.append("")

    sys.stdout.write("\n".join(out))


main()
