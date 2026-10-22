#!/bin/sh
set -eu
repo=$(CDPATH= cd -- "$(dirname "$0")/.." && pwd)
build=$(mktemp -d "${TMPDIR:-/tmp}/sonix-gui-book.XXXXXX")
trap 'rm -rf "$build"' EXIT HUP INT TERM
case $(uname -s) in
  Darwin) gc_flags="-Wl,-dead_strip" ;;
  *) gc_flags="-Wl,--gc-sections" ;;
esac
# Whole production functions, like run-remote-keys-test.sh; avoid pulling the
# entire player's renderer and SDL into the navigation boundary harness.
awk '/^static (double current_chapter_end|bool audiobook_auto_stop|void audiobook_seek_by|bool chapter_load_part|void more_btn_event_cb|void book_back_before_jump_action)\(/ || /^void player_step_chapter\(/ {emit=1} emit {print} /^}/ {emit=0}' \
 "$repo/sonix-player/src/gui/nowplaying/player.c" > "$build/gui-player-navigation.h"
${CC:-cc} -O1 -g -ffunction-sections -fdata-sections -I"$build" -I"$repo/tests/stubs" -I"$repo/sonix-player" \
 "$repo/tests/gui_book_navigation_test.c" $gc_flags -lpthread -o "$build/test"
failed=0
for scenario in ${@:-failed-bookmark outgoing-floor failed-chapter failed-part picked-part library-resume skip-origin auto-stop stopped-menu stopped-back paused-back playing-back}; do
 if "$build/test" "$scenario"; then :; else failed=$((failed + 1)); fi
done
printf 'GUI book scenarios: %s failed\n' "$failed"
exit "$failed"
