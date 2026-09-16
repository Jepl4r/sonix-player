#!/bin/bash
# Estrae da src/system/bluetooth/bluetooth.c la decisione di mollare il rientro
# automatico e la fa girare contro tools/test_btreconnect.c.
#
# Uso: run_btreconnect_bench.sh
set -u

HERE=$(cd "$(dirname "$0")/.." && pwd)
W=$(mktemp -d)
trap 'rm -rf "$W"' EXIT

python3 "$HERE/tools/extract_reconnect.py" "$HERE/src/system/bluetooth/bluetooth.c" "$W/reconnect.inc" || exit 1

gcc -O1 -g -Wall -Wextra -I"$W" -o "$W/test_btreconnect" "$HERE/tools/test_btreconnect.c" -lpthread || exit 1

"$W/test_btreconnect"
