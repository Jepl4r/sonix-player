#!/usr/bin/env python3
"""Estrae da src/system/device/power.c la regola che spegne le radio da sole.

Il banco prova (tools/test_radiopark.c) include il file generato e ci fa girare
sopra un orologio finto. E' una regola di tempo, e le regole di tempo sbagliano
in silenzio: il minuto partiva da quando si spegneva lo schermo invece che da
quando la radio smetteva di servire, quindi una pausa a schermo gia' spento
spegneva il Wi-Fi nel quarto di secondo dopo. Con una copia a mano il banco
continuerebbe a passare il giorno che qualcuno rimette l'orologio sbagliato.

Cosa viene preso: since(), wifi_wanted(), park_radios_if_idle() e le due
marche di tempo che quella funzione tiene.

Uso: extract_radiopark.py src/system/device/power.c > /tmp/radiopark_extracted.h
"""

import re
import sys

FUNCS = ["since", "wifi_wanted", "park_radios_if_idle"]
MARKS = ["g_wifi_idle_since", "g_bt_idle_since"]


def take_func(src, name):
    m = re.search(r"^static [^\n;]*\b%s\(" % re.escape(name), src, re.M)
    if not m:
        sys.exit("manca %s() in power.c" % name)
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


def take_mark(src, name):
    m = re.search(r"^static uint32_t %s\s*;" % re.escape(name), src, re.M)
    if not m:
        sys.exit("manca %s in power.c" % name)
    return m.group(0)


def main():
    if len(sys.argv) != 2:
        sys.exit("uso: extract_radiopark.py <power.c>")
    src = open(sys.argv[1]).read()

    out = ["// Generato da tools/extract_radiopark.py -- non si modifica a mano.", ""]
    for name in MARKS:
        out.append(take_mark(src, name))
    out.append("")
    for name in FUNCS:
        out.append(take_func(src, name))
        out.append("")
    sys.stdout.write("\n".join(out))


if __name__ == "__main__":
    main()
