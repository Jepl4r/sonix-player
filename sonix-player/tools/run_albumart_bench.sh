#!/bin/bash
# Fa girare tools/test_albumart.c -- il lettore delle copertine dentro i tag --
# contro src/system/library/albumart.c vero, sotto AddressSanitizer e
# UndefinedBehaviorSanitizer.
#
# E' il pezzo che il bench delle copertine lascia fuori: li' albumart e'
# sostituito da un finto che consegna i byte gia' pronti, cosi' l'aritmetica dei
# buffer si prova da sola. Questo prova invece il camminatore di byte che legge
# il file audio, che e' l'unica cosa del percorso a non avere mai avuto un banco.
set -u

HERE=$(cd "$(dirname "$0")/.." && pwd)
WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT

echo "-- i file"
python3 "$HERE/tools/make_art_zoo.py" "$WORK/zoo" || exit 1

SAN="-fsanitize=address,undefined -fno-omit-frame-pointer -fno-sanitize-recover=all"
CFLAGS="-O1 -g -std=gnu11 -D_FILE_OFFSET_BITS=64 -DHOST_BUILD=1 -I$HERE"

# shellcheck disable=SC2086
gcc $CFLAGS $SAN $(pkg-config --cflags opusfile 2>/dev/null) -o "$WORK/test_albumart" \
	"$HERE/tools/test_albumart.c" \
	"$HERE/src/system/library/albumart.c" \
	"$HERE/src/system/library/cue.c" \
	"$HERE/src/system/decode/decode.c" \
	"$HERE/src/system/decode/aacdec.c" \
	"$HERE/src/system/decode/alacdec.c" \
	"$HERE/src/system/decode/alac/alac.c" \
	"$HERE/src/system/decode/dsd.c" \
	"$HERE/src/system/decode/growfile.c" \
	"$HERE/src/system/decode/opusdec.c" \
	"$HERE/src/system/decode/sndfile.c" \
	"$HERE/src/system/core/utils.c" \
	"$HERE/src/system/decode/mp4.c" \
	"$HERE/src/system/decode/stb_vorbis.c" \
	"$HERE/src/system/decode/wavpackdec.c" \
	$(pkg-config --libs opusfile wavpack 2>/dev/null) -lm -lpthread || exit 1

echo
ASAN_OPTIONS=detect_stack_use_after_return=1:detect_leaks=1:abort_on_error=0 \
	UBSAN_OPTIONS=print_stacktrace=1 \
	"$WORK/test_albumart" "$WORK/zoo"
