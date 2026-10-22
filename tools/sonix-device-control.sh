#!/usr/bin/env bash
# Local ADB helper for the FIFO protocol from sonix-audiobooks; see OFFLINE_ADDITIONS.md.
set -euo pipefail
command=${1:-status}
shift "$(( $# > 0 ? 1 : 0 ))"
case "$command:$#" in
  tap:2|tap:3|swipe:4|swipe:5)
    for value in "$@"; do
      [[ "$value" =~ ^-?[0-9]+$ ]] || { echo 'Coordinates/duration must be integers' >&2; exit 2; }
    done ;;
  wake:0|status:0|shot:0) ;;
  *) echo 'Usage: sonix-device-control.sh tap x y [ms] | swipe x1 y1 x2 y2 [ms] | wake | status | shot' >&2; exit 2 ;;
esac
adb shell 'test -p /tmp/sonix-control' || { echo 'No Sonix control FIFO on the connected device' >&2; exit 1; }
line="$command${*:+ $*}"
# Arguments are restricted to fixed commands and decimal integers above.
adb shell "printf '%s\n' '$line' > /tmp/sonix-control"
sleep 0.2
result=$(adb shell cat /tmp/sonix-control.status)
printf '%s\n' "$result"
[[ "$result" == ok* ]]
