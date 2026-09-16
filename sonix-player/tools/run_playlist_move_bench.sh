#!/bin/bash
# Fa girare tools/test_playlist_move.c contro il src/system/library/library.c vero e uno
# SQLite vero, su un indice temporaneo.
#
# Serve `make` almeno una volta prima: linka build_host/system/library/library.o, cosi'
# il banco prova la funzione che gira sul lettore e non una copia.
#
# Uso: run_playlist_move_bench.sh
set -u

HERE=$(cd "$(dirname "$0")/.." && pwd)
cd "$HERE" || exit 1

OBJ=build_host/system/library/library.o
[ -f "$OBJ" ] || { echo "$OBJ non c'e': lancia prima make"; exit 1; }

W=$(mktemp -d)
trap 'rm -rf "$W"' EXIT

gcc -O1 -g -Wall -Wextra -I"$HERE" -o "$W/test_playlist_move" \
	"$HERE/tools/test_playlist_move.c" "$OBJ" -lsqlite3 -lpthread -lm || exit 1

"$W/test_playlist_move"
