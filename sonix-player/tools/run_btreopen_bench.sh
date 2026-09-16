#!/bin/bash
# Estrae da src/system/bluetooth/btreceiver.c la politica di riapertura della modalita'
# ricevitore e ci fa girare sopra tools/test_btreopen.c.
set -eu

HERE=$(cd "$(dirname "$0")/.." && pwd)
W=$(mktemp -d)
trap 'rm -rf "$W"' EXIT

python3 "$HERE/tools/extract_codec_list.py" "$HERE/src/system/bluetooth/btreceiver.c" "$W/reopen_extracted.h" \
	reopen_t reopen_decision still_there

gcc -O1 -g -Wall -Wextra -I"$W" -o "$W/test_btreopen" "$HERE/tools/test_btreopen.c"
"$W/test_btreopen"
