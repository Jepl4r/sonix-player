#!/bin/bash
# Fa girare tools/test_mseb.c contro il codice vero di src/system/audio/eq.c:
# la banca MSEB e il pre-gain automatico del modulo stock.
#
# La parte estratta e' aritmetica su biquad, quindi non serve ne' `make` ne'
# ALSA ne' LVGL.
#
# Uso: run_mseb_bench.sh
set -eu

HERE=$(cd "$(dirname "$0")/.." && pwd)
W=$(mktemp -d)
trap 'rm -rf "$W"' EXIT

python3 "$HERE/tools/extract_mseb.py" "$HERE/src/system/audio/eq.c" > "$W/mseb_extracted.h"

gcc -O1 -g -Wall -Wextra -I"$W" -o "$W/test_mseb" "$HERE/tools/test_mseb.c" -lm
"$W/test_mseb"
