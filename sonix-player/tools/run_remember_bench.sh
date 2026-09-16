#!/bin/bash
# Fa girare tools/test_remember.c contro la regola vera di
# src/system/playback/device_state.c.
#
# La decisione viene estratta dal sorgente del player (extract_remember.py), non
# riscritta: se cambia la regola nel player cambia anche quella misurata qui.
#
# Non serve `make`, e non serve niente del resto del player: la funzione e'
# pura.
#
# Uso: run_remember_bench.sh
set -u

HERE=$(cd "$(dirname "$0")/.." && pwd)
cd "$HERE" || exit 1

W=$(mktemp -d)
trap 'rm -rf "$W"' EXIT

python3 tools/extract_remember.py src/system/playback/device_state.c > "$W/remember_extracted.h" || exit 1

gcc -O2 -g -Wall -Wextra -I"$HERE" -I"$W" \
	-o "$W/test_remember" "$HERE/tools/test_remember.c" || exit 1

"$W/test_remember"
