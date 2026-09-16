#!/usr/bin/env python3
"""Estrae da src/system/streaming/radio.c il riconoscimento del formato.

Il banco prova (tools/test_radiocodec.c) include il file generato e ci fa
girare sopra flussi veri, cosi' misura il codice del lettore e non una copia.
Qui una copia sarebbe particolarmente pericolosa: la prima versione di questo
riconoscimento sbagliava proprio sui casi che il banco prova adesso, e una
copia avrebbe continuato a passare.

Cosa viene preso: la costante della catena, radio_skip_id3(),
adts_frame_len(), adts_chain(), bytes_look_like_adts() e
codec_from_content_type() con la sua enum.

Uso: extract_radiocodec.py src/system/streaming/radio.c > /tmp/radiocodec_extracted.h
"""

import re
import sys

DEFINES = ["ADTS_CHAIN_PROOF"]
FUNCS = [
    "codec_from_content_type",
    "radio_skip_id3",
    "adts_frame_len",
    "adts_chain",
    "bytes_look_like_adts",
]


def take_define(src, name):
    m = re.search(r"^#define\s+%s\s+.*$" % re.escape(name), src, re.M)
    if not m:
        sys.exit("manca #define %s in radio.c" % name)
    return m.group(0)


def take_enum(src, name):
    m = re.search(r"^typedef\s+enum\s*\{[^{}]*\}\s*%s\s*;" % re.escape(name), src, re.S | re.M)
    if not m:
        sys.exit("manca l'enum %s in radio.c" % name)
    return m.group(0)


def take_func(src, name):
    m = re.search(r"^static [^\n;]*\b%s\(" % re.escape(name), src, re.M)
    if not m:
        sys.exit("manca %s() in radio.c" % name)
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
        sys.exit("uso: extract_radiocodec.py <radio.c>")
    src = open(sys.argv[1]).read()

    out = ["// Generato da tools/extract_radiocodec.py -- non si modifica a mano.", ""]
    # radio.c chiama i byte drmp3_uint8; qui non c'e' dr_mp3, e il tipo e' quello.
    out.append("typedef unsigned char drmp3_uint8;")
    out.append("")
    out.append(take_enum(src, "radio_codec_t"))
    out.append("")
    for name in DEFINES:
        out.append(take_define(src, name))
    out.append("")
    for name in FUNCS:
        out.append(take_func(src, name))
        out.append("")
    sys.stdout.write("\n".join(out))


if __name__ == "__main__":
    main()
