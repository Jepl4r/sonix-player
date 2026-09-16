#!/bin/bash
# Fa girare tools/test_btpcm.c contro un server bluealsa vero (il bluealsa-mock
# del suo albero, che pubblica un A2DP finto senza hardware Bluetooth) e
# attraverso il plugin ALSA vero.
#
# Uso: run_btpcm_bench.sh <cartella-build-di-bluez-alsa> [secondi-prima-della-seek]
#
# NOTA sul mock: di suo usa l'adattatore hci11, mentre il player costruisce i
# percorsi su hci0. Per far combaciare le due cose, nell'albero di bluez-alsa:
#     test/mock/mock.h:  #define MOCK_ADAPTER_ID 0
#                        #define MOCK_BLUEZ_ADAPTER_PATH "/org/bluez/hci0"
# e poi `make -C test/mock bluealsa-mock`.
#
# La cartella build e' quella di un `configure && make` di bluez-alsa: serve
# src/asound/.libs per il plugin e, per la 4.3.1, test/mock/bluealsa-mock.
set -u

BUILD=${1:?serve la cartella di build di bluez-alsa}
SEEK_AT=${2:-3}
HERE=$(cd "$(dirname "$0")/.." && pwd)
MOCK=${MOCK:-$BUILD/test/mock/bluealsa-mock}
DEV="12:34:56:78:9A:BC"

[ -x "$MOCK" ] || { echo "bluealsa-mock non trovato in $MOCK"; exit 1; }
PLUGIN_DIR="$BUILD/src/asound/.libs"
[ -f "$PLUGIN_DIR/libasound_module_pcm_bluealsa.so" ] || { echo "plugin non trovato in $PLUGIN_DIR"; exit 1; }

W=$(mktemp -d)
trap 'kill $MOCK_PID 2>/dev/null; rm -rf "$W"' EXIT

# Il mock si tira su un bus tutto suo e ne stampa l'indirizzo su stderr: e'
# quello che il plugin deve usare.
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
echo "bus del mock: $ADDR"

# La configurazione ALSA: quella di sistema piu' il 20-bluealsa.conf di questa
# stessa build, cosi' pcm.bluealsa e' definito esattamente come sul dispositivo.
CONF=$(ls "$BUILD"/src/asound/20-bluealsa.conf 2>/dev/null || true)
[ -f "$CONF" ] || { echo "20-bluealsa.conf non trovato"; exit 1; }
cat > "$W/alsa.conf" <<EOF
</usr/share/alsa/alsa.conf>
<$CONF>
EOF
export ALSA_CONFIG_PATH="$W/alsa.conf"
export ALSA_PLUGIN_DIR="$PLUGIN_DIR"

gcc -O1 -g -Wall -o "$W/test_btpcm" "$HERE/tools/test_btpcm.c" -lasound -lm || exit 1

# E il percorso di scrittura vero del player, estratto da audio.c.
python3 "$HERE/tools/extract_write_path.py" "$HERE/src/system/audio/audio.c" "$W/write_path.inc" || exit 1
gcc -O1 -g -Wall -I"$W" -o "$W/test_btwrite" "$HERE/tools/test_btwrite.c" -lasound -lm || exit 1

echo
echo "=== riferimento: nessuna seek ==="
"$W/test_btpcm" "bluealsa:DEV=$DEV,PROFILE=a2dp" --secondi 6

echo
echo "=== con la seek a ${SEEK_AT}s (drop + prepare, come audio.c) ==="
"$W/test_btpcm" "bluealsa:DEV=$DEV,PROFILE=a2dp" --seek-at "$SEEK_AT" --secondi 8

echo
echo "=== la stessa seek, ma con 1,5 s in cui il decoder non da' frame ==="
"$W/test_btpcm" "bluealsa:DEV=$DEV,PROFILE=a2dp" --seek-at "$SEEK_AT" --secondi 10 --stallo-seek 1500

echo
echo "=== la catena vera del dispositivo: 96 kHz / 32 bit convertiti da plug, con la seek ==="
"$W/test_btpcm" "bluealsa:DEV=$DEV,PROFILE=a2dp" --seek-at "$SEEK_AT" --secondi 9 --rate 96000 --s32

echo
echo "=== il percorso di scrittura vero, scrittore al passo del tempo reale ==="
echo "--- riferimento: nessuna interruzione"
"$W/test_btwrite" "bluealsa:DEV=$DEV,PROFILE=a2dp" --secondi 10 --pacer
echo "--- con la seek a 4s"
"$W/test_btwrite" "bluealsa:DEV=$DEV,PROFILE=a2dp" --seek-at 4 --secondi 14 --pacer
echo "--- con la pausa/ripresa a 4s, per confronto"
"$W/test_btwrite" "bluealsa:DEV=$DEV,PROFILE=a2dp" --pausa-at 4 --secondi 14 --pacer

echo
echo "--- ultime righe del server ---"
tail -5 "$W/mock.log"
