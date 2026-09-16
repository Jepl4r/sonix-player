#!/bin/bash
# Fa girare tools/test_dsdgain.c contro il codice vero di
# src/system/audio/alsa-controls.c: la tabella DSDGain del player stock, la
# somma sull'attenuazione grezza e il taglio a zero.
#
# La parte estratta e' aritmetica su interi, quindi non serve ne' `make` ne'
# ALSA ne' LVGL.
#
# Uso: run_dsdgain_bench.sh
set -eu

HERE=$(cd "$(dirname "$0")/.." && pwd)
W=$(mktemp -d)
trap 'rm -rf "$W"' EXIT

python3 "$HERE/tools/extract_dsdgain.py" "$HERE/src/system/audio/alsa-controls.c" > "$W/dsdgain_extracted.h"

gcc -O1 -g -Wall -Wextra -I"$W" -o "$W/test_dsdgain" "$HERE/tools/test_dsdgain.c"
"$W/test_dsdgain"
