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
if sh "$repo/tests/run-remote-keys-test.sh"; then :; else failed=$((failed + 1)); fi
${CC:-cc} -O1 -g -I"$repo/sonix-player" "$repo/tests/bookmark_guard_test.c" -o "$build/bookmark-guard-test"
if "$build/bookmark-guard-test"; then :; else failed=$((failed + 1)); fi
${CC:-cc} -O1 -g -ffunction-sections -fdata-sections -I"$repo/sonix-player" "$repo/tests/bookmark_save_test.c" $gc_flags -o "$build/bookmark-save-test"
if "$build/bookmark-save-test"; then :; else failed=$((failed + 1)); fi
for test in cancel partial-cancel stat-error stale scan-error cyrillic live-save crash prune disc authors natural write-retry late-other-book publish-failure speed-column speed-kept duration-column duration-card duration-kept duration-scan; do
 mkdir "$build/$test"
 if ! "$build/test" "$test" "$build/$test"; then failed=$((failed + 1)); fi
done
${CC:-cc} -O1 -g -ffunction-sections -fdata-sections -I"$repo/tests/stubs" -I"$repo/sonix-player" "$repo/tests/playback_switch_test.c" $gc_flags -o "$build/switch-test"
if "$build/switch-test"; then printf "%s\n" "PASS playback-switch"; else failed=$((failed + 1)); fi
${CC:-cc} -O1 -g -ffunction-sections -fdata-sections -I"$repo/tests/stubs" -I"$repo/sonix-player" "$repo/tests/playback_part_step_test.c" $gc_flags -o "$build/part-step-test"
if "$build/part-step-test"; then printf "%s\n" "PASS playback-part-step"; else failed=$((failed + 1)); fi
${CC:-cc} -O1 -g -ffunction-sections -fdata-sections -I"$repo/tests/stubs" -I"$repo/sonix-player" "$repo/tests/audiobook_units_test.c" $gc_flags -lpthread -o "$build/units-test"
if "$build/units-test"; then printf "%s\n" "PASS audiobook-units"; else failed=$((failed + 1)); fi
for test in playback_floor sleep_checkpoint sleep_checkpoint_race; do
 ${CC:-cc} -O1 -g -ffunction-sections -fdata-sections -I"$repo/tests/stubs" -I"$repo/sonix-player" "$repo/tests/${test}_test.c" $gc_flags -lpthread -o "$build/$test-test"
 if "$build/$test-test"; then :; else failed=$((failed + 1)); fi
done
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
sanitizer_flags=""
case $(uname -s) in
  Linux) sanitizer_flags="-fsanitize=address,undefined -fno-omit-frame-pointer" ;;
esac
${CC:-cc} -O1 -g $sanitizer_flags -I"$repo/sonix-player" "$repo/tests/id3chap_test.c" -o "$build/id3chap-test"
if "$build/id3chap-test" "$build/id3chap.mp3"; then printf "%s\n" "PASS id3chap"; else failed=$((failed + 1)); fi
${CC:-cc} -O1 -g "$repo/tests/sleepfade_test.c" -o "$build/sleepfade-test"
if "$build/sleepfade-test"; then :; else failed=$((failed + 1)); fi
missing_pkg=""
for pkg in opusfile wavpack; do
 if ! pkg-config --exists "$pkg"; then missing_pkg="$pkg"; break; fi
done
if [ -n "$missing_pkg" ]; then
 printf '%s\n' "PASS durationprobe (skipped: missing $missing_pkg)"
else
# Build the real decoders for the duration fixtures; section GC drops unused playback paths.
${CC:-cc} -O1 -g -ffunction-sections -fdata-sections -DHOST_BUILD=1 -I"$repo/sonix-player" $(pkg-config --cflags opusfile wavpack) \
 "$repo/tests/durationprobe_test.c" "$repo"/sonix-player/src/system/decode/*.c \
 "$repo/sonix-player/src/system/decode/alac/alac.c" "$repo/sonix-player/src/system/decode/ape/apecore.c" \
 "$repo/sonix-player/src/system/library/cue.c" $gc_flags $(pkg-config --libs opusfile wavpack) -lm -ldl -lpthread -o "$build/durationprobe-test"
mkdir "$build/probe"
if "$build/durationprobe-test" "$build/probe"; then :; else failed=$((failed + 1)); fi
fi
exit "$failed"
