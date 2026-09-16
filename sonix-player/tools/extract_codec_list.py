#!/usr/bin/env python3
# Tira fuori da src/system/bluetooth/bluetooth.c i pezzi che i bench dei codec fanno
# girare, e li scrive in un header che il bench compila.
#
# Si estrae invece di copiare perche' una copia invecchia in silenzio: se
# domani qualcuno cambia do_auto_codec e il bench continua a passare su una
# vecchia versione, il bench non serve piu' a niente.
#
# Uso: extract_codec_list.py <sorgente> <header> <nome> [<nome> ...]
import re
import sys


def grab_function(text, name):
    # La firma sta su una riga sola in questo file; da li' si contano le graffe.
    # Piu' di un riscontro quando c'e' anche la dichiarazione in cima al file:
    # quella finisce con ";" prima di qualsiasi graffa e va saltata, altrimenti
    # si estrae il corpo della funzione successiva.
    for match in re.finditer(r"^static [^\n;]*\b%s\(" % re.escape(name), text, re.M):
        brace = text.find("{", match.end() - 1)
        if brace < 0 or ";" in text[match.end() - 1:brace]:
            continue
        start = match.start()
        depth = 0
        i = brace
        while True:
            if text[i] == "{":
                depth += 1
            elif text[i] == "}":
                depth -= 1
                if depth == 0:
                    return text[start:i + 1]
            i += 1
    return None


def grab_typedef(text, name):
    # typedef enum { ... } nome_t;
    # Il corpo di una enum non ha graffe dentro, quindi [^{}]* invece di .*?:
    # con .*? il riscontro partirebbe dalla prima enum del file e si
    # porterebbe dietro tutto quello che sta in mezzo.
    match = re.search(r"^typedef\s+enum\s*\{[^{}]*\}\s*%s\s*;" % re.escape(name), text, re.S | re.M)
    return match.group(0) if match else None


def grab_table(text, name):
    # Una tabella statica con la struct anonima davanti, fino al "};" a inizio riga.
    match = re.search(r"^static\s+const\s+struct\s*\{[^{}]*\}\s*%s\[\]\s*=\s*\{.*?^\};" % re.escape(name),
                      text, re.S | re.M)
    return match.group(0) if match else None


def main():
    source, out, names = sys.argv[1], sys.argv[2], sys.argv[3:]
    if not names:
        sys.exit("serve almeno un nome da estrarre")
    text = open(source).read()
    parts = ["// Generato da tools/extract_codec_list.py -- non si modifica a mano."]
    for name in names:
        piece = grab_function(text, name) or grab_typedef(text, name) or grab_table(text, name)
        if piece is None:
            sys.exit("%s: non trovata in %s" % (name, source))
        parts.append(piece)
    open(out, "w").write("\n\n".join(parts) + "\n")


if __name__ == "__main__":
    main()
