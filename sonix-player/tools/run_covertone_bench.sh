#!/bin/bash
# Fa girare tools/test_covertone.c -- il colore del bagliore di Cover Flow --
# contro la cover_dominant_tone() vera, estratta da src/gui/nowplaying/cover.c.
set -u

HERE=$(cd "$(dirname "$0")/.." && pwd)
WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT

python3 "$HERE/tools/extract_covertone.py" "$HERE/src/gui/nowplaying/cover.c" > "$WORK/covertone_extracted.h" || exit 1
gcc -O1 -g -Wall -Wextra -std=gnu11 -I"$WORK" -o "$WORK/test_covertone" "$HERE/tools/test_covertone.c" || exit 1

"$WORK/test_covertone"
