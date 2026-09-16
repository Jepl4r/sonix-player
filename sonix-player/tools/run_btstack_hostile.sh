#!/bin/bash
# Gli scenari ostili di tools/test_btstack_hostile.c. Ognuno vuole un bus e un
# finto bluez suoi, perche' quasi tutti li rompono apposta.
set -u

HERE=$(cd "$(dirname "$0")/.." && pwd)
TOTAL=0
BAD=0

build() {
	gcc -O1 -g -Wall -Wextra -I"$HERE" -o "$1" \
		"$HERE/tools/test_btstack_hostile.c" "$HERE/src/system/bluetooth/btstack.c" "$HERE/src/system/bluetooth/dbuslite.c" \
		-lpthread
}

run_scenario() {
	SCENARIO=$1
	shift
	WORK=$(mktemp -d)

	cat > "$WORK/bus.conf" <<EOF
<!DOCTYPE busconfig PUBLIC "-//freedesktop//DTD D-BUS Bus Configuration 1.0//EN"
 "http://www.freedesktop.org/standards/dbus/1.0/busconfig.dtd">
<busconfig>
  <type>system</type>
  <listen>unix:path=$WORK/bus</listen>
  <auth>EXTERNAL</auth>
  <policy context="default">
    <allow user="*"/><allow own="*"/>
    <allow send_type="method_call"/><allow send_type="signal"/>
    <allow send_type="method_return"/><allow send_type="error"/>
    <allow receive_type="method_call"/><allow receive_type="signal"/>
    <allow receive_type="method_return"/><allow receive_type="error"/>
  </policy>
</busconfig>
EOF

	dbus-daemon --config-file="$WORK/bus.conf" --nofork --nopidfile &
	BUS_PID=$!
	export DBUS_SYSTEM_BUS_ADDRESS="unix:path=$WORK/bus"
	for i in $(seq 50); do [ -S "$WORK/bus" ] && break; sleep 0.1; done

	env "$@" python3 "$HERE/tools/fake_bluez.py" &
	FAKE_PID=$!
	sleep 1

	FAKE_BUS_PID=$BUS_PID timeout 90 "$BIN" "$SCENARIO"
	RC=$?
	TOTAL=$((TOTAL + 1))
	[ $RC -ne 0 ] && BAD=$((BAD + 1)) && echo "  >>> scenario $SCENARIO: rc=$RC"

	kill $FAKE_PID $BUS_PID 2>/dev/null
	wait $FAKE_PID $BUS_PID 2>/dev/null
	rm -rf "$WORK"
}

BIN=$(mktemp -u /tmp/test_hostile.XXXX)
build "$BIN" || exit 1

run_scenario no-agent  FAKE_BLUEZ_NO_AGENT=1
run_scenario pair-fail FAKE_BLUEZ_PAIR_FAIL=1
run_scenario no-pcm    FAKE_BLUEZ_NO_PCM=1
run_scenario many      FAKE_BLUEZ_MANY=200
run_scenario bus-dies  FAKE_BLUEZ_MANY=0

rm -f "$BIN"
echo
echo "$TOTAL scenari, $BAD falliti"
exit $([ $BAD -eq 0 ] && echo 0 || echo 1)
