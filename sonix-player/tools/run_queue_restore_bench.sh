#!/bin/bash
# Fa girare tools/test_queue_restore.c contro il playlist.c e il library.c veri
# e uno SQLite vero, su un indice temporaneo.
#
# Serve `make` almeno una volta prima: linka build_host/system/playback/playlist.o e
# build_host/system/library/library.o, cosi' il banco prova il codice che gira sul
# lettore e non una copia.
#
# Uso: run_queue_restore_bench.sh
set -u

HERE=$(cd "$(dirname "$0")/.." && pwd)
cd "$HERE" || exit 1

for OBJ in build_host/system/playback/playlist.o build_host/system/library/library.o; do
	[ -f "$OBJ" ] || { echo "$OBJ non c'e': lancia prima make"; exit 1; }
done

W=$(mktemp -d)
trap 'rm -rf "$W"' EXIT

gcc -O1 -g -Wall -Wextra -I"$HERE" -o "$W/test_queue_restore" \
	"$HERE/tools/test_queue_restore.c" \
	build_host/system/playback/playlist.o build_host/system/library/library.o \
	-lsqlite3 -lpthread -lm || exit 1

"$W/test_queue_restore"
