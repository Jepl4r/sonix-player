#!/bin/bash
# Fa girare tools/test_cuefolder.c contro il src/system/playback/playlist.c vero e il
# src/system/library/cue.c vero, su una cartella costruita al momento.
#
# Serve `make` almeno una volta prima: linka gli oggetti gia' compilati, cosi'
# il banco prova il codice che gira sul lettore e non una copia.
#
# Uso: run_cuefolder_bench.sh
set -u

HERE=$(cd "$(dirname "$0")/.." && pwd)
cd "$HERE" || exit 1

OBJS="build_host/system/playback/playlist.o build_host/system/library/cue.o build_host/system/core/utils.o"

for o in $OBJS; do
	[ -f "$o" ] || { echo "$o non c'e': lancia prima make"; exit 1; }
done

W=$(mktemp -d)
trap 'rm -rf "$W"' EXIT

gcc -O1 -g -Wall -Wextra -I"$HERE" -o "$W/test_cuefolder" \
	"$HERE/tools/test_cuefolder.c" $OBJS \
	-lpthread -lm || exit 1

mkdir -p "$W/album"
"$W/test_cuefolder" "$W/album"
