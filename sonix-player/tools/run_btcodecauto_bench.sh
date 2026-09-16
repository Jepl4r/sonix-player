#!/bin/bash
# Estrae da src/system/bluetooth/bluetooth.c la scelta automatica del codec e ci fa girare
# sopra tools/test_btcodecauto.c.
set -eu

HERE=$(cd "$(dirname "$0")/.." && pwd)
W=$(mktemp -d)
trap 'rm -rf "$W"' EXIT

python3 "$HERE/tools/extract_codec_list.py" "$HERE/src/system/bluetooth/bluetooth.c" "$W/codec_auto_extracted.h" \
	CODEC_RANKS codec_normalise codec_rank do_auto_codec

gcc -O1 -g -Wall -Wextra -I"$W" -o "$W/test_btcodecauto" "$HERE/tools/test_btcodecauto.c"
"$W/test_btcodecauto"
