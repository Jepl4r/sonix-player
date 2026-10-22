#!/bin/sh
set -eu
repo=$(CDPATH= cd -- "$(dirname "$0")/.." && pwd)
build=$(mktemp -d "${TMPDIR:-/tmp}/sonix-gui-book.XXXXXX")
trap 'rm -rf "$build"' EXIT HUP INT TERM
# Whole production functions, like run-remote-keys-test.sh; avoid pulling the
# entire player's renderer and SDL into the navigation boundary harness.
awk '/^static (double current_chapter_end|bool audiobook_auto_stop|void audiobook_seek_by|bool chapter_load_part)\(/ || /^void player_step_chapter\(/ {emit=1} emit {print} /^}/ {emit=0}' \
 "$repo/sonix-player/src/gui/nowplaying/player.c" > "$build/gui-player-navigation.h"
${CC:-cc} -O1 -g -ffunction-sections -fdata-sections -I"$build" -I"$repo/tests/stubs" -I"$repo/sonix-player" \
 "$repo/tests/gui_book_navigation_test.c" -Wl,--gc-sections -lpthread -o "$build/test"
for scenario in ${@:-failed-bookmark outgoing-floor failed-chapter library-resume}; do
 "$build/test" "$scenario"
done
