#!/bin/bash
# Fa girare tools/test_btvolume.c -- che compila btvolume.c e dbuslite.c veri --
# contro il bluealsa-mock dell'albero di bluez-alsa.
#
# Uso: run_btvolume_bench.sh <cartella-build-di-bluez-alsa>
#
# NOTA sul mock: di suo usa l'adattatore hci11, mentre il player costruisce i
# percorsi su hci0. Per far combaciare le due cose, nell'albero di bluez-alsa:
#     test/mock/mock.h:  #define MOCK_ADAPTER_ID 0
#                        #define MOCK_BLUEZ_ADAPTER_PATH "/org/bluez/hci0"
# e poi `make -C test/mock bluealsa-mock`.
set -u

BUILD=${1:?serve la cartella di build di bluez-alsa}
HERE=$(cd "$(dirname "$0")/.." && pwd)
MOCK=$BUILD/test/mock/bluealsa-mock
DEV="12:34:56:78:9A:BC"

[ -x "$MOCK" ] || { echo "bluealsa-mock non trovato in $MOCK"; exit 1; }

W=$(mktemp -d)
trap 'kill $MOCK_PID 2>/dev/null; rm -rf "$W"' EXIT

"$MOCK" -p a2dp-source -c sbc --device-name="$DEV:Cuffie finte" -t 120000 >"$W/mock.log" 2>&1 &
MOCK_PID=$!

for i in $(seq 100); do
	ADDR=$(sed -n 's/^DBUS_SYSTEM_BUS_ADDRESS=//p' "$W/mock.log" | head -1)
	READY=$(grep -c "READY=A2DP:$DEV" "$W/mock.log")
	[ -n "$ADDR" ] && [ "$READY" != "0" ] && break
	sleep 0.1
done
[ -n "${ADDR:-}" ] || { echo "il mock non ha detto su quale bus sta"; cat "$W/mock.log"; exit 1; }
export DBUS_SYSTEM_BUS_ADDRESS="$ADDR"
echo "bus del mock: $ADDR"

gcc -O1 -g -Wall -Wextra -I"$HERE" -o "$W/test_btvolume" \
	"$HERE/tools/test_btvolume.c" "$HERE/src/system/bluetooth/btvolume.c" "$HERE/src/system/bluetooth/dbuslite.c" \
	-lpthread || exit 1

"$W/test_btvolume" "$DEV"
