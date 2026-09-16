#!/usr/bin/env python3
"""Estrae da src/system/audio/alsa-controls.c la compensazione di guadagno DSD.

Il banco prova (tools/test_dsdgain.c) include il file generato, cosi' misura il
codice che gira davvero sul lettore. Una copia scritta a mano nel banco
invecchierebbe in silenzio: il giorno che qualcuno cambia il segno della somma
o toglie il taglio a zero, il banco deve fallire, non continuare a passare su
una vecchia versione.

Cosa viene preso: le due curve di volume, la tabella DSDGain con il suo
#define, e with_dsd_gain(). Le tre variabili su cui quella funzione si appoggia
(high_gain, current_dop, dsd_gain_index) le definisce il banco prima di
includere questo file: muoverle e' il modo in cui costruisce i casi.

Uso: extract_dsdgain.py src/system/audio/alsa-controls.c > /tmp/dsdgain_extracted.h
"""

import re
import sys

DEFINES = ["DSD_GAIN_STEPS"]
TABLES = ["HIBY_HW_MDB", "HIBY_HW_HDB", "HIBY_DSD_GAIN_RAW"]
FUNCS = ["with_dsd_gain"]


def take_define(src, name):
    m = re.search(r"^#define\s+%s\s+.*$" % re.escape(name), src, re.M)
    if not m:
        sys.exit("manca #define %s in alsa-controls.c" % name)
    return m.group(0)


def take_table(src, name):
    # Nessuna graffa dentro una tabella di interi, quindi il primo "};" chiude.
    m = re.search(r"^static const int %s\[[^\]]*\]\s*=\s*\{[^{}]*\};" % re.escape(name), src, re.S | re.M)
    if not m:
        sys.exit("manca la tabella %s in alsa-controls.c" % name)
    return m.group(0)


def take_func(src, name):
    m = re.search(r"^static [^\n;]*\b%s\(" % re.escape(name), src, re.M)
    if not m:
        sys.exit("manca %s() in alsa-controls.c" % name)
    start = m.start()
    depth = 0
    j = src.index("{", m.end() - 1)
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
        sys.exit("uso: extract_dsdgain.py <alsa-controls.c>")
    src = open(sys.argv[1]).read()

    out = ["// Generato da tools/extract_dsdgain.py -- non si modifica a mano.", ""]
    for name in DEFINES:
        out.append(take_define(src, name))
    out.append("")
    for name in TABLES:
        out.append(take_table(src, name))
        out.append("")
    for name in FUNCS:
        out.append(take_func(src, name))
        out.append("")
    sys.stdout.write("\n".join(out))


if __name__ == "__main__":
    main()
