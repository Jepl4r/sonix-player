#!/bin/bash
# Monta un dbus-daemon vero, ci mette sopra tools/fake_bluez.py e ci fa girare
# contro tools/test_btstack.c, che compila i sorgenti veri del player.
set -u

HERE=$(cd "$(dirname "$0")/.." && pwd)
WORK=$(mktemp -d)
trap 'kill $FAKE_PID $BUS_PID 2>/dev/null; rm -rf "$WORK"' EXIT

cat > "$WORK/bus.conf" <<EOF
<!DOCTYPE busconfig PUBLIC "-//freedesktop//DTD D-BUS Bus Configuration 1.0//EN"
 "http://www.freedesktop.org/standards/dbus/1.0/busconfig.dtd">
<busconfig>
  <type>system</type>
  <listen>unix:path=$WORK/bus</listen>
  <auth>EXTERNAL</auth>
  <policy context="default">
    <allow user="*"/>
    <allow own="*"/>
    <allow send_type="method_call"/>
    <allow send_type="signal"/>
    <allow send_type="method_return"/>
    <allow send_type="error"/>
    <allow receive_type="method_call"/>
    <allow receive_type="signal"/>
    <allow receive_type="method_return"/>
    <allow receive_type="error"/>
  </policy>
</busconfig>
EOF

dbus-daemon --config-file="$WORK/bus.conf" --nofork --nopidfile &
BUS_PID=$!
export DBUS_SYSTEM_BUS_ADDRESS="unix:path=$WORK/bus"

for i in $(seq 50); do [ -S "$WORK/bus" ] && break; sleep 0.1; done
[ -S "$WORK/bus" ] || { echo "il bus non e' partito"; exit 1; }

python3 "$HERE/tools/fake_bluez.py" &
FAKE_PID=$!
sleep 1

gcc -O1 -g -Wall -Wextra -I"$HERE" -o "$WORK/test_btstack" \
    "$HERE/tools/test_btstack.c" "$HERE/src/system/bluetooth/btstack.c" "$HERE/src/system/bluetooth/dbuslite.c" \
    -lpthread || exit 1

"$WORK/test_btstack" 2>"$WORK/err.txt"
RESULT=$?
cat "$WORK/err.txt"

# Il cambio di stato del flusso A2DP arriva come segnale, non come risposta a
# una domanda: la riga deve comparire da sola, senza che nessuno chieda niente.
echo
echo "-- i cambi di stato del flusso arrivano dai segnali"
for want in pending active idle; do
	if grep -q "il flusso A2DP fd0 -> $want" "$WORK/err.txt"; then
		echo "  ok       il passaggio a $want e' finito nel log"
	else
		echo "  FALLITO  il passaggio a $want non e' stato visto"
		RESULT=1
	fi
done

# E una seconda volta con due transport sullo stesso dispositivo: quello vivo e
# l'orfano che bluez si tiene quando l'encoder rifiuta una configurazione. E'
# la forma che ha il log quando l'audio non esce.
echo
echo "########## con un transport orfano accanto a quello vivo"
kill $FAKE_PID 2>/dev/null
wait $FAKE_PID 2>/dev/null
FAKE_BLUEZ_TRANSPORTS="fd5=idle,fd6=active" python3 "$HERE/tools/fake_bluez.py" &
FAKE_PID=$!
sleep 1
EXPECT_STREAMS="fd5=idle fd6=active" "$WORK/test_btstack" || RESULT=1

exit $RESULT
