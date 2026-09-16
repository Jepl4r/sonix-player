#!/bin/bash
# Fa girare tools/test_covers.c -- la catena delle copertine -- contro
# src/gui/nowplaying/cover.c vero, sotto AddressSanitizer e UndefinedBehaviorSanitizer.
#
# ASan e' il punto: quello che si cerca non e' un risultato sbagliato ma una
# scrittura di un byte oltre il bordo di un buffer, che su questo dispositivo
# non si vede finche' non si porta via qualcos'altro che stava li' vicino.
set -u

HERE=$(cd "$(dirname "$0")/.." && pwd)
WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT

echo "-- le immagini"
python3 "$HERE/tools/make_cover_zoo.py" "$WORK/zoo" || exit 1

SAN="-fsanitize=address,undefined -fno-omit-frame-pointer -fno-sanitize-recover=all"
CFLAGS="-O1 -g -std=gnu11 -D_FILE_OFFSET_BITS=64 -DHOST_BUILD=1 -DLV_CONF_INCLUDE_SIMPLE=1
        -I$HERE -I$HERE/lvgl"

# shellcheck disable=SC2086
gcc $CFLAGS $SAN -o "$WORK/test_covers" \
	"$HERE/tools/test_covers.c" \
	"$HERE/tools/cover_stubs.c" \
	"$HERE/src/gui/nowplaying/cover.c" \
	"$HERE/src/system/image/jpeg_scaled.c" \
	"$HERE/src/system/image/png_scaled.c" \
	"$HERE/src/system/image/stb_image_impl.c" \
	-lm -lpthread || exit 1

echo
# detect_stack_use_after_return costa, ma e' proprio la classe di guasto che
# spiegherebbe il secondo crash del dispositivo.
ASAN_OPTIONS=detect_stack_use_after_return=1:detect_leaks=0:abort_on_error=0 \
	UBSAN_OPTIONS=print_stacktrace=1 \
	"$WORK/test_covers" "$WORK/zoo"
