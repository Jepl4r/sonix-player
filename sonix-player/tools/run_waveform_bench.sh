#!/bin/bash
# Fa girare tools/test_waveform.c contro il src/system/audio/waveform.c vero e i
# decodificatori veri, su file audio costruiti al momento.
#
# Serve `make` almeno una volta prima: linka gli oggetti gia' compilati, cosi'
# il banco prova il codice che gira sul lettore e non una copia.
#
# Uso: run_waveform_bench.sh
set -u

HERE=$(cd "$(dirname "$0")/.." && pwd)
cd "$HERE" || exit 1

[ -f build_host/system/audio/waveform.o ] || { echo "build_host/system/audio/waveform.o non c'e': lancia prima make"; exit 1; }

W=$(mktemp -d)
trap 'rm -rf "$W"' EXIT

# Tutto quello che il decodificatore si porta dietro: e' meno lavoro elencare
# gli oggetti che scrivere finte per una dozzina di codec.
OBJS="build_host/system/audio/waveform.o build_host/system/core/utils.o build_host/system/library/cue.o \
	$(find build_host/system/decode build_host/system/image -name '*.o' 2>/dev/null | tr '\n' ' ') \
"

gcc -O1 -g -Wall -Wextra -I"$HERE" -o "$W/test_waveform" \
	"$HERE/tools/test_waveform.c" $OBJS \
	-lpthread -lm -lsndfile -lmpg123 -lopus -lopusfile -lFLAC -lwavpack 2>"$W/link.txt"
if [ $? -ne 0 ]; then
	echo "il link non e' riuscito; simboli mancanti:"
	grep -o "undefined reference to \`[^']*'" "$W/link.txt" | sort -u | head -20
	exit 1
fi

"$W/test_waveform" "$W"
