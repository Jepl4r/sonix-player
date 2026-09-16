#!/bin/bash
# Fa girare tools/test_ebook.c contro il src/system/ebook/ vero, su EPUB costruiti al
# momento da tools/make_test_epub.py.
#
# Serve `make` almeno una volta prima: linka gli oggetti gia' compilati, cosi'
# il banco prova il codice che gira sul lettore e non una copia.
#
# Uso: run_ebook_bench.sh
set -u

HERE=$(cd "$(dirname "$0")/.." && pwd)
cd "$HERE" || exit 1

[ -f build_host/system/ebook/ebook.o ] || { echo "build_host/system/ebook/ebook.o non c'e': lancia prima make"; exit 1; }

W=$(mktemp -d)
trap 'rm -rf "$W"' EXIT

echo "i libri di prova:"
python3 "$HERE/tools/make_test_epub.py" "$W" || exit 1

# png_scaled.o porta l'inflate che epub_zip usa (vedi il commento in epub_zip.c).
OBJS="build_host/system/ebook/ebook.o build_host/system/ebook/epub_arena.o build_host/system/ebook/epub_zip.o \
	build_host/system/ebook/epub_xml.o build_host/system/ebook/epub_opf.o build_host/system/ebook/epub_xhtml.o \
	build_host/system/ebook/epub_css.o \
	build_host/system/image/png_scaled.o"

gcc -O1 -g -Wall -Wextra -D_GNU_SOURCE -I"$HERE" -o "$W/test_ebook" \
	"$HERE/tools/test_ebook.c" $OBJS -lm 2>"$W/link.txt"
if [ $? -ne 0 ]; then
	echo "il link non e' riuscito:"
	head -20 "$W/link.txt"
	exit 1
fi

"$W/test_ebook" "$W"
