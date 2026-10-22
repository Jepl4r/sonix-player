#!/usr/bin/env bash
set -euo pipefail
ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT
cd "$ROOT/sonix-player"
FLAGS=(-O1 -g -DHOST_BUILD=1 -D_FILE_OFFSET_BITS=64 -DLV_CONF_INCLUDE_SIMPLE=1
       -I. -Ilvgl -ffunction-sections -fdata-sections)
case $(uname -s) in
  Darwin) GC=(-Wl,-dead_strip) ;;
  *) GC=(-Wl,--gc-sections) ;;
esac
cat >"$WORK/tinfl.c" <<'SRC'
#include "miniz.h"
#include "miniz_tinfl.h"
#include "tinfl_impl.inc"
SRC
${CC:-cc} "${FLAGS[@]}" -Isrc/system/image/miniz -c "$WORK/tinfl.c" -o "$WORK/tinfl.o"
${CC:-cc} "${FLAGS[@]}" "$ROOT/tests/metadata_sidecar_test.c" \
 src/system/library/{metadata,oggtags,cue}.c src/system/core/{utils,config}.c \
 src/system/decode/{decode,mp4,wavpackdec,apedec,sndfile,stb_vorbis}.c \
 "$WORK/tinfl.o" "${GC[@]}" $(pkg-config --cflags --libs opusfile wavpack) \
 -lm -ldl -lpthread -o "$WORK/test"
"$WORK/test" "$WORK"
