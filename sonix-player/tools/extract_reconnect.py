#!/usr/bin/env python3
"""Estrae dal src/system/bluetooth/bluetooth.c VERO le due funzioni del rientro automatico:
quella che decide di mollarlo e quella che dimentica l'ultimo dispositivo.

Stessa idea di extract_write_path.py: nel banco non finisce una copia scritta a
mano ma le righe che girano sul dispositivo. Se bluetooth.c cambia e il banco
non compila piu', ha ragione il banco.

Uso: extract_reconnect.py <bluetooth.c> <reconnect.inc>
"""

import re
import sys

WANTED = ["reconnect_should_stop", "forget_last_device"]


def find_function(text, name):
    """Il corpo della funzione, dalla riga della firma alla graffa che la chiude."""
    pattern = re.compile(
        r"^(?:static\s+)?[A-Za-z_][A-Za-z0-9_ \t*]*\b" + re.escape(name) + r"\s*\([^;]*?\)\s*\{",
        re.M | re.S,
    )
    m = pattern.search(text)
    if not m:
        raise SystemExit("non trovo %s in bluetooth.c" % name)
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


def main():
    if len(sys.argv) != 3:
        raise SystemExit(__doc__)
    text = open(sys.argv[1]).read()

    out = [
        "// Generato da tools/extract_reconnect.py: NON modificare a mano.",
        "// E' la funzione vera di src/system/bluetooth/bluetooth.c.",
        "",
    ]
    for name in WANTED:
        out.append(find_function(text, name))
        out.append("")

    open(sys.argv[2], "w").write("\n".join(out))
    print("estratte %d funzioni" % len(WANTED))


if __name__ == "__main__":
    main()
