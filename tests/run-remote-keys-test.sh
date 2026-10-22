#!/bin/sh
set -eu
repo=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
build=$(mktemp -d "${TMPDIR:-/tmp}/sonix-remote-test.XXXXXX")
trap 'rm -rf "$build"' EXIT HUP INT TERM
# Select whole production functions, without rewriting their statements. This
# keeps device discovery/DBus worker dependencies out of a command-routing test.
awk '/^static (bool media_keys_swapped|keymap_button_t keymap_button_for_code|void handle_key_press|void run_hook_gesture|void handle_bt_remote_key|void run_remote_clicks)\(/ {emit=1} emit {print} /^}/ {emit=0}' \
 "$repo/sonix-player/src/system/device/system.c" > "$build/remote-handlers.h"
awk '/^static void run_command\(/ {emit=1} emit {print} /^}/ {emit=0}' \
 "$repo/sonix-player/src/system/bluetooth/btplayer.c" >> "$build/remote-handlers.h"
awk '/^static bool chapter_key_steps\(/ {print; next} /^void player_key_(next|prev)_chapter\(/ {emit=1} emit {print} /^}/ {emit=0}' \
 "$repo/sonix-player/src/gui/nowplaying/player.c" >> "$build/remote-handlers.h"
${CC:-cc} -O1 -g -I"$build" -I"$repo/tests/stubs" -I"$repo/sonix-player" \
 "$repo/tests/remote_keys_test.c" -o "$build/test"
"$build/test"
