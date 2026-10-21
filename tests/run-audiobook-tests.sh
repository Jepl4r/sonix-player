#!/bin/sh
set -eu
repo=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
build=$(mktemp -d "${TMPDIR:-/tmp}/sonix-audiobook-tests.XXXXXX")
trap 'rm -rf "$build"' EXIT HUP INT TERM
case $(uname -s) in
  Darwin) gc_flags="-Wl,-dead_strip" ;;
  *) gc_flags="-Wl,--gc-sections" ;;
esac
${CC:-cc} -O1 -g -ffunction-sections -fdata-sections -I"$repo/sonix-player" -c "$repo/sonix-player/src/system/library/library.c" -o "$build/library.o"
${CC:-cc} -O1 -g -ffunction-sections -fdata-sections -I"$repo/sonix-player" "$repo/tests/audiobookdb_test.c" "$build/library.o" $gc_flags -lsqlite3 -lpthread -o "$build/test"
failed=0
for test in cancel partial-cancel stat-error stale scan-error cyrillic live-save crash prune disc authors natural write-retry late-other-book publish-failure; do
 mkdir "$build/$test"
 if ! "$build/test" "$test" "$build/$test"; then failed=$((failed + 1)); fi
done
${CC:-cc} -O1 -g -ffunction-sections -fdata-sections -I"$repo/tests/stubs" -I"$repo/sonix-player" "$repo/tests/playback_switch_test.c" $gc_flags -o "$build/switch-test"
if "$build/switch-test"; then printf "%s\n" "PASS playback-switch"; else failed=$((failed + 1)); fi
${CC:-cc} -O1 -g -ffunction-sections -fdata-sections -I"$repo/tests/stubs" -I"$repo/sonix-player" "$repo/tests/playback_part_step_test.c" $gc_flags -o "$build/part-step-test"
if "$build/part-step-test"; then printf "%s\n" "PASS playback-part-step"; else failed=$((failed + 1)); fi
${CC:-cc} -O1 -g -ffunction-sections -fdata-sections -I"$repo/tests/stubs" -I"$repo/sonix-player" "$repo/tests/audiobook_units_test.c" $gc_flags -lpthread -o "$build/units-test"
if "$build/units-test"; then printf "%s\n" "PASS audiobook-units"; else failed=$((failed + 1)); fi
${CC:-cc} -O1 -g -ffunction-sections -fdata-sections -I"$repo/tests/stubs" -I"$repo/sonix-player" "$repo/tests/keymap_actions_test.c" $gc_flags -o "$build/keymap-test"
if "$build/keymap-test"; then printf "%s\n" "PASS keymap-actions"; else failed=$((failed + 1)); fi
${CC:-cc} -O1 -g -ffunction-sections -fdata-sections -I"$repo/tests/stubs" -I"$repo/sonix-player" "$repo/tests/sleeptimer_test.c" $gc_flags -o "$build/sleeptimer-test"
if "$build/sleeptimer-test"; then printf "%s\n" "PASS sleeptimer"; else failed=$((failed + 1)); fi
${CC:-cc} -O1 -g -ffunction-sections -fdata-sections -I"$repo/tests/stubs" -I"$repo/sonix-player" "$repo/tests/eq_override_test.c" $gc_flags -lm -o "$build/eq-override-test"
mkdir "$build/eq-override"
if "$build/eq-override-test" "$build/eq-override"; then printf "%s\n" "PASS eq-override"; else failed=$((failed + 1)); fi
for guard in ota firmware; do
 ${CC:-cc} -O1 -g -ffunction-sections -fdata-sections -I"$repo/tests/stubs" -I"$repo/sonix-player" "$repo/tests/${guard}_guard_test.c" $gc_flags -o "$build/$guard-test"
 mkdir "$build/$guard"
 if "$build/$guard-test" "$build/$guard"; then printf "%s\n" "PASS $guard-guard"; else failed=$((failed + 1)); fi
done
exit "$failed"
