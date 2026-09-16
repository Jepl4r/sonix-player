#!/bin/bash
# Fa girare tools/test_coverwarp.c contro il codice vero di src/gui/nowplaying/coverflow.c.
#
# La proiezione viene estratta dal sorgente del player (extract_coverwarp.py), non
# riscritta: se cambia la geometria nel player cambia anche quella misurata qui.
#
# Non serve `make`, e non serve nemmeno LVGL: la parte estratta e' aritmetica
# pura su un buffer di pixel.
#
# Uso: run_coverwarp_bench.sh
set -u

HERE=$(cd "$(dirname "$0")/.." && pwd)
cd "$HERE" || exit 1

W=$(mktemp -d)
trap 'rm -rf "$W"' EXIT

python3 tools/extract_coverwarp.py src/gui/nowplaying/coverflow.c > "$W/coverwarp_extracted.h" || exit 1

# -O2 come il build vero: la misura del costo per fotogramma non vuol dire
# niente su codice non ottimizzato.
gcc -O2 -g -Wall -Wextra -I"$HERE" -I"$W" \
	-o "$W/test_coverwarp" "$HERE/tools/test_coverwarp.c" -lm || exit 1

"$W/test_coverwarp"
