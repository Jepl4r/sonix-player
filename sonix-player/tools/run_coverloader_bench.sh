#!/bin/bash
# Fa girare tools/test_coverloader.c -- il passaggio di consegne fra il worker
# delle copertine e l'interfaccia -- sotto ThreadSanitizer.
#
# L'altra meta' del bench delle copertine: run_covers_bench.sh guarda
# l'aritmetica dei buffer con ASan, questo guarda le corse fra i due thread, che
# ASan non vede. Nei crash del dispositivo il thread che muore e' quello
# dell'interfaccia e l'ultima riga la scrive il worker: erano attivi insieme.
set -u

HERE=$(cd "$(dirname "$0")/.." && pwd)
WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT

ROUNDS=${1:-400}

echo "-- le immagini"
python3 "$HERE/tools/make_cover_zoo.py" "$WORK/zoo" || exit 1

CFLAGS="-O1 -g -std=gnu11 -D_FILE_OFFSET_BITS=64 -DHOST_BUILD=1 -DLV_CONF_INCLUDE_SIMPLE=1
        -I$HERE -I$HERE/lvgl"

# shellcheck disable=SC2086
gcc $CFLAGS -fsanitize=thread -fno-omit-frame-pointer -o "$WORK/test_coverloader" \
	"$HERE/tools/test_coverloader.c" \
	"$HERE/tools/cover_stubs.c" \
	"$HERE/src/gui/nowplaying/coverloader.c" \
	"$HERE/src/gui/nowplaying/cover.c" \
	"$HERE/src/system/image/jpeg_scaled.c" \
	"$HERE/src/system/image/png_scaled.c" \
	"$HERE/src/system/image/stb_image_impl.c" \
	-lm -lpthread || exit 1

echo
# halt_on_error: una corsa e' una risposta, non serve raccoglierne mille.
TSAN_OPTIONS=halt_on_error=0:second_deadlock_stack=1 \
	"$WORK/test_coverloader" "$ROUNDS" "$WORK"/zoo/b_0400x0400.jpg "$WORK"/zoo/p_0300x0293.png \
	"$WORK"/zoo/prog_1200x1200.jpg "$WORK"/zoo/b_0401x0399.jpg "$WORK"/zoo/troncato_meta.jpg \
	"$WORK"/zoo/b_3000x3000.jpg 2>&1 | grep -v "^cover: decoded"
