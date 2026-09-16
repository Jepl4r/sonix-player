#!/bin/bash
# Fa girare tools/test_id3chap.c contro il src/system/library/id3chap.c vero, su MP3
# costruiti al momento da tools/make_id3_chapters.py.
#
# Serve `make` almeno una volta prima: linka l'oggetto gia' compilato, cosi' il
# banco prova il codice che gira sul lettore e non una copia.
set -u

HERE=$(cd "$(dirname "$0")/.." && pwd)
cd "$HERE" || exit 1

[ -f build_host/system/library/id3chap.o ] || { echo "build_host/system/library/id3chap.o non c'e': lancia prima make"; exit 1; }

W=$(mktemp -d)
trap 'rm -rf "$W"' EXIT

echo "gli mp3 di prova:"
python3 "$HERE/tools/make_id3_chapters.py" "$W" || exit 1

gcc -O1 -g -Wall -Wextra -I"$HERE" -o "$W/test_id3chap" \
	"$HERE/tools/test_id3chap.c" build_host/system/library/id3chap.o 2>"$W/link.txt"
if [ $? -ne 0 ]; then
	echo "il link non e' riuscito:"
	head -20 "$W/link.txt"
	exit 1
fi

"$W/test_id3chap" "$W"
