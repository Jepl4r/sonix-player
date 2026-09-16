#!/bin/bash
# Estrae da src/system/bluetooth/bluetooth.c la lettura dei codec dall'help di bluealsa e
# la scelta di quali passare con -c, e ci fa girare sopra tools/test_btcodeclist.c.
set -eu

HERE=$(cd "$(dirname "$0")/.." && pwd)
W=$(mktemp -d)
trap 'rm -rf "$W"' EXIT

python3 "$HERE/tools/extract_codec_list.py" "$HERE/src/system/bluetooth/bluetooth.c" "$W/codec_list_extracted.h" \
	parse_codec_line read_local_codecs codecs_to_ask_for

gcc -O1 -g -Wall -Wextra -I"$W" -o "$W/test_btcodeclist" "$HERE/tools/test_btcodeclist.c"
"$W/test_btcodeclist"
