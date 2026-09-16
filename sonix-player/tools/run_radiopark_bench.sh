#!/bin/bash
# Fa girare tools/test_radiopark.c contro il codice vero di
# src/system/device/power.c: quando le due radio si spengono da sole con lo
# schermo scuro.
#
# La parte estratta e' aritmetica su un orologio, quindi non serve ne' `make`
# ne' LVGL ne' un dispositivo.
#
# Uso: run_radiopark_bench.sh
set -eu

HERE=$(cd "$(dirname "$0")/.." && pwd)
W=$(mktemp -d)
trap 'rm -rf "$W"' EXIT

python3 "$HERE/tools/extract_radiopark.py" "$HERE/src/system/device/power.c" > "$W/radiopark_extracted.h"

gcc -O1 -g -Wall -Wextra -I"$W" -o "$W/test_radiopark" "$HERE/tools/test_radiopark.c"
"$W/test_radiopark"
