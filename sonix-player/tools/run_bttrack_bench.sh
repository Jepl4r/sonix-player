#!/bin/bash
# Fa girare tools/test_bttrack.c -- il confine fra due tracce sulle cuffie --
# contro un server bluealsa vero (il bluealsa-mock del suo albero, che pubblica
# un A2DP finto senza nessun hardware Bluetooth) e attraverso il plugin ALSA
# vero.
#
# Uso: run_bttrack_bench.sh <cartella-build-di-bluez-alsa>
#
# NOTA sul mock: di suo usa l'adattatore hci11, mentre il player costruisce i
# percorsi su hci0. Per far combaciare le due cose, nell'albero di bluez-alsa:
#     test/mock/mock.h:  #define MOCK_ADAPTER_ID 0
#                        #define MOCK_BLUEZ_ADAPTER_PATH "/org/bluez/hci0"
# e poi `make -C test/mock bluealsa-mock`.
set -u

BUILD=${1:?serve la cartella di build di bluez-alsa}
HERE=$(cd "$(dirname "$0")/.." && pwd)
MOCK=${MOCK:-$BUILD/test/mock/bluealsa-mock}
DEV="12:34:56:78:9A:BC"

[ -x "$MOCK" ] || { echo "bluealsa-mock non trovato in $MOCK"; exit 1; }
PLUGIN_DIR="$BUILD/src/asound/.libs"
[ -f "$PLUGIN_DIR/libasound_module_pcm_bluealsa.so" ] || { echo "plugin non trovato in $PLUGIN_DIR"; exit 1; }

W=$(mktemp -d)
trap 'kill $MOCK_PID 2>/dev/null; rm -rf "$W"' EXIT

"$MOCK" -p a2dp-source -c sbc --device-name="$DEV:Cuffie finte" -t 120000 >"$W/mock.log" 2>&1 &
MOCK_PID=$!

for i in $(seq 100); do
	ADDR=$(sed -n 's/^DBUS_SYSTEM_BUS_ADDRESS=//p' "$W/mock.log" | head -1)
	READY=$(grep -c "^BLUEALSA_READY=A2DP:$DEV" "$W/mock.log")
	[ -n "$ADDR" ] && [ "$READY" != "0" ] && break
	sleep 0.1
done
[ -n "${ADDR:-}" ] || { echo "il mock non ha detto su quale bus sta"; cat "$W/mock.log"; exit 1; }
export DBUS_SYSTEM_BUS_ADDRESS="$ADDR"

CONF=$(ls "$BUILD"/src/asound/20-bluealsa.conf 2>/dev/null || true)
[ -f "$CONF" ] || { echo "20-bluealsa.conf non trovato"; exit 1; }
cat > "$W/alsa.conf" <<EOF
</usr/share/alsa/alsa.conf>
<$CONF>
EOF
export ALSA_CONFIG_PATH="$W/alsa.conf"
export ALSA_PLUGIN_DIR="$PLUGIN_DIR"

# Il percorso Bluetooth vero, estratto da audio.c.
python3 "$HERE/tools/extract_btpath.py" "$HERE/src/system/audio/audio.c" > "$W/btpath_extracted.h" || exit 1
gcc -O1 -g -Wall -I"$W" -o "$W/test_bttrack" "$HERE/tools/test_bttrack.c" -lasound -lm || exit 1

RC=0
# Il tempo fra una traccia e l'altra e' la variabile che il dispositivo ha e un
# banco non ha: li' fra la chiusura e l'apertura dopo ci stanno la ricerca del
# file, l'apertura del decoder e il primo buffer letto dalla microSD.
for GAP in 0 300 1000 3000; do
	echo
	echo "=== $GAP ms fra una traccia e l'altra ==="
	"$W/test_bttrack" "bluealsa:DEV=$DEV,PROFILE=a2dp" --pausa "$GAP" || RC=1
done

# E poi la parte che riguarda il lettore vero: UN core.
#
# Qui di core ce ne sono tanti, e con un core libero un thread di fondo non
# affama nessuno -- e' il limite che il round 232 si era gia' scritto. Con
# taskset il lettore, il server bluealsa e il thread di fondo finiscono tutti
# sullo stesso core, che e' la condizione del dispositivo.
CORE=${CORE:-0}
echo
echo "########## un core solo (CPU $CORE): lettore, bluealsa e il thread di fondo insieme"
taskset -cp "$CORE" $MOCK_PID >/dev/null 2>&1 || echo "(non sono riuscito a pinnare il mock)"
for NOISE in nessuno idle nice10; do
	echo
	echo "=== thread di fondo: $NOISE ==="
	if [ "$NOISE" = nessuno ]; then
		taskset -c "$CORE" "$W/test_bttrack" "bluealsa:DEV=$DEV,PROFILE=a2dp" --pausa 300 || RC=1
	else
		taskset -c "$CORE" "$W/test_bttrack" "bluealsa:DEV=$DEV,PROFILE=a2dp" --pausa 300 --rumore "$NOISE" || RC=1
	fi
done

echo
echo "--- ultime righe del server ---"
tail -4 "$W/mock.log"
exit $RC
