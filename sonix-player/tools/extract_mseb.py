#!/usr/bin/env python3
"""Estrae da src/system/audio/eq.c la banca MSEB e il suo pre-gain automatico.

Il banco prova (tools/test_mseb.c) include il file generato, cosi' misura la
tabella e l'algoritmo che girano sul lettore. Qui una copia sarebbe inutile due
volte: le frequenze e i Q della tabella sono il punto della replica, e il
pre-gain e' una ricerca a dieci giri con tre costanti lette dal modulo stock --
se una cambia, il banco deve fallire.

Cosa viene preso: mseb_def_t con MSEB_DEFS, le costanti della griglia e della
ricerca, il generatore di coefficienti biquad, la valutazione della risposta,
i pesi, la costruzione della banca e mseb_auto_preamp_db().

Uso: extract_mseb.py src/system/audio/eq.c > /tmp/mseb_extracted.h
"""

import re
import sys

DEFINES = [
    "MSEB_PROBE_POINTS",
    "MSEB_PROBE_DECADES",
    "MSEB_PROBE_START_HZ",
    "MSEB_PROBE_RISING",
    "MSEB_SEARCH_ROUNDS",
    "MSEB_SEARCH_RATIO",
    "MSEB_SEARCH_FLOOR",
    "MSEB_FILTERS",
    "EQ_MAX_CHANNELS",
]

FUNCS = [
    "filter_uses_gain",
    "biquad_set_coeffs",
    "biquad_power_ratio_at",
    "mseb_weights_build",
    "mseb_bank_build",
    "mseb_auto_preamp_db",
]


def take_define(src, name):
    m = re.search(r"^#define\s+%s\s+.*$" % re.escape(name), src, re.M)
    if not m:
        sys.exit("manca #define %s in eq.c" % name)
    return m.group(0)


def take_enum_line(src, name):
    m = re.search(r"^enum \{[^}]*\b%s\b[^}]*\};" % re.escape(name), src, re.M | re.S)
    if not m:
        sys.exit("manca l'enum con %s in eq.c" % name)
    return m.group(0)


def take_typedef_struct(src, name):
    m = re.search(r"^typedef struct \{[^{}]*\}\s*%s\s*;" % re.escape(name), src, re.M | re.S)
    if not m:
        sys.exit("manca la struct %s in eq.c" % name)
    return m.group(0)


def take_table(src, name):
    m = re.search(r"^static const mseb_def_t %s\[\]\s*=\s*\{.*?^\};" % re.escape(name), src, re.M | re.S)
    if not m:
        sys.exit("manca la tabella %s in eq.c" % name)
    return m.group(0)


def take_func(src, name):
    m = re.search(r"^static [^\n;]*\b%s\(" % re.escape(name), src, re.M)
    if not m:
        sys.exit("manca %s() in eq.c" % name)
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
        sys.exit("uso: extract_mseb.py <eq.c>")
    src = open(sys.argv[1]).read()

    out = ["// Generato da tools/extract_mseb.py -- non si modifica a mano.", ""]
    # Lo stato che le funzioni estratte leggono: il banco lo muove per
    # costruire i casi, quindi lo definisce lui prima di includere questo file.
    out.append("extern int mseb_enabled;")
    out.append("extern int mseb_value[];")
    out.append("")
    out.append(take_enum_line(src, "F_HIGHSHELF"))
    out.append("")
    for name in DEFINES:
        if name == "MSEB_FILTERS":
            continue
        out.append(take_define(src, name))
    out.append("")
    out.append(take_typedef_struct(src, "biquad_t"))
    out.append("")
    out.append(take_typedef_struct(src, "mseb_def_t"))
    out.append("")
    out.append(take_table(src, "MSEB_DEFS"))
    out.append(take_define(src, "MSEB_FILTERS"))
    out.append("")
    # I pesi: la tabella e la sua bandiera stanno accanto alla funzione che la
    # riempie, e senza di loro quella funzione non compila.
    out.append("static float mseb_weight[MSEB_PROBE_POINTS];")
    out.append("static bool mseb_weight_ready;")
    out.append("")
    for name in FUNCS:
        out.append(take_func(src, name))
        out.append("")
    sys.stdout.write("\n".join(out))


if __name__ == "__main__":
    main()
