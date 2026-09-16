#!/bin/bash
# Fa girare tools/test_radiocodec.c contro il codice vero di
# src/system/streaming/radio.c: come si capisce se una stazione manda MP3 o AAC.
#
# I flussi li genera ffmpeg qui sotto -- AAC LC, AAC 5.1 e MP3 -- perche' su
# dati inventati a mano il riconoscimento passerebbe qualunque cosa. Senza
# ffmpeg il banco non gira e lo dice invece di fingere.
#
# Uso: run_radiocodec_bench.sh
set -eu

HERE=$(cd "$(dirname "$0")/.." && pwd)

if ! command -v ffmpeg >/dev/null 2>&1; then
	echo "serve ffmpeg per generare i flussi di prova"
	exit 1
fi

W=$(mktemp -d)
trap 'rm -rf "$W"' EXIT

ffmpeg -hide_banner -loglevel error -f lavfi -i "sine=frequency=440:duration=30:sample_rate=44100" \
	-ac 2 -c:a aac -b:a 128k -f adts "$W/lc.aac"
ffmpeg -hide_banner -loglevel error -f lavfi -i "sine=frequency=440:duration=30:sample_rate=44100" \
	-ac 6 -c:a aac -b:a 256k -f adts "$W/surround.aac"
ffmpeg -hide_banner -loglevel error -f lavfi -i "sine=frequency=440:duration=30:sample_rate=44100" \
	-ac 2 -c:a libmp3lame -b:a 128k -f mp3 "$W/test.mp3"

python3 "$HERE/tools/extract_radiocodec.py" "$HERE/src/system/streaming/radio.c" > "$W/radiocodec_extracted.h"

gcc -O1 -g -Wall -Wextra -D_GNU_SOURCE -I"$W" -o "$W/test_radiocodec" "$HERE/tools/test_radiocodec.c"
"$W/test_radiocodec" "$W"
