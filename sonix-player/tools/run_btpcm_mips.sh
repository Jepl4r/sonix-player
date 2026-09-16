#!/bin/bash
# Fa girare il plugin bluealsa VERO, compilato per MIPS, dentro il rootfs VERO
# del dispositivo, contro un server bluealsa vero.
#
# Perche' esiste. run_btpcm_bench.sh prova il plugin compilato per x86 contro le
# librerie di questa macchina: dice se il codice del plugin si comporta bene, non
# se il file che finisce nel firmware funziona. Questo invece usa il .so MIPS che
# spediamo, la libasound 1.2.1.2 del dispositivo e la sua configurazione ALSA --
# tutto quello che sta fra il player e il demone, esattamente come sul lettore.
#
# Come. qemu-mipsel-static esegue il MIPS; il chroot serve perche' la cartella
# dei plugin e' compilata dentro libasound come /usr/lib/alsa-lib e nessuna
# variabile d'ambiente la sposta -- l'unico modo di far caricare il nostro .so e'
# che quel percorso sia il nostro. Il demone resta x86 e parla dal di fuori: il
# bus e' un socket unix, l'architettura non c'entra.
#
# Uso:
#   run_btpcm_mips.sh <rootfs> <plugin.so> <bluealsa-mock> [argomenti di test_btpcm]
#
# Esempio (confronto fra il plugin di serie e il nostro):
#   run_btpcm_mips.sh ~/rootfs ~/rootfs/usr/lib/alsa-lib/libasound_module_pcm_bluealsa.so \
#       ~/bt/natbuild411m/test/mock/bluealsa-mock --seek-at 3 --secondi 9 --stallo-seek 1500
#   run_btpcm_mips.sh ~/rootfs ~/bt/out/libasound_module_pcm_bluealsa.so \
#       ~/bt/natbuild/test/mock/bluealsa-mock --seek-at 3 --secondi 9 --stallo-seek 1500
#
# Serve root (il chroot e i bind mount) e qemu-mipsel-static.
set -u

ROOTFS=${1:?serve il rootfs estratto del dispositivo}
PLUGIN=${2:?serve il plugin .so MIPS da provare}
# Niente apostrofi nel messaggio: dentro ${var:?...} bash fa quote processing e
# un apostrofo apre una stringa che si mangia il resto dello script.
MOCK=${3:?serve il bluealsa-mock di bluez-alsa}
shift 3

HERE=$(cd "$(dirname "$0")/.." && pwd)
BT=${BT:-/home/claude/work/bt}
SR=$BT/sysroot
DEV="12:34:56:78:9A:BC"
QEMU=$(command -v qemu-mipsel-static) || { echo "manca qemu-mipsel-static"; exit 1; }

[ -d "$ROOTFS/usr/lib/alsa-lib" ] || { echo "$ROOTFS non sembra il rootfs del dispositivo"; exit 1; }
[ -x "$MOCK" ] || { echo "bluealsa-mock non trovato in $MOCK"; exit 1; }

W=$(mktemp -d)
cleanup() {
	kill ${MOCK_PID:-0} 2>/dev/null
	umount "$W/fs/proc" "$W/fs/dev" "$W/fs/tmp" 2>/dev/null
	rm -rf "$W"
}
trap cleanup EXIT

# test_btpcm per MIPS, con gli stessi flag del plugin: la ISA e l'ABI in virgola
# mobile del binario stock, gli header di glibc 2.39 riportati al 2.22 e il
# tempo a 32 bit. -lc prima di ld.so.1, come sempre.
echo "== compilo test_btpcm per MIPS"
mipsel-linux-gnu-gcc-12 -EL -mips32r2 -mabi=32 -mfp32 -mnan=legacy -O1 -std=gnu11 \
	-include "$BT/glibc22-compat.h" -U_TIME_BITS -D_TIME_BITS=32 \
	-I"$SR/usr/include" -L"$SR/lib" -Wl,-rpath-link,"$SR/lib" \
	-o "$W/test_btpcm" "$HERE/tools/test_btpcm.c" -lasound -lm -lc -l:ld.so.1 || exit 1

echo "== preparo il chroot"
cp -a "$ROOTFS" "$W/fs"
cp "$QEMU" "$W/fs/qemu-mipsel-static"
cp "$W/test_btpcm" "$W/fs/test_btpcm"
cp "$PLUGIN" "$W/fs/usr/lib/alsa-lib/libasound_module_pcm_bluealsa.so"
mkdir -p "$W/fs/tmp" "$W/fs/dev" "$W/fs/proc"
# /tmp serve perche' il socket del bus del mock sta li'; /dev e /proc perche'
# alsa-lib e la libc li guardano all'avvio.
mount --bind /tmp "$W/fs/tmp" || exit 1
mount --bind /dev "$W/fs/dev" || exit 1
mount -t proc proc "$W/fs/proc" || exit 1

echo "== avvio il server ($(basename "$(dirname "$(dirname "$(dirname "$MOCK")")")"))"
"$MOCK" -p a2dp-source -c sbc --device-name="$DEV:Cuffie finte" -t 120000 >"$W/mock.log" 2>&1 &
MOCK_PID=$!
for i in $(seq 100); do
	ADDR=$(sed -n 's/^DBUS_SYSTEM_BUS_ADDRESS=//p' "$W/mock.log" | head -1)
	[ -n "$ADDR" ] && [ "$(grep -c "READY=A2DP:$DEV" "$W/mock.log")" != "0" ] && break
	sleep 0.1
done
[ -n "${ADDR:-}" ] || { echo "il mock non ha detto su quale bus sta"; cat "$W/mock.log"; exit 1; }

echo "== plugin: $(stat -c %s "$PLUGIN") byte  ($PLUGIN)"
DBUS_SYSTEM_BUS_ADDRESS="$ADDR" chroot "$W/fs" /qemu-mipsel-static /test_btpcm \
	"bluealsa:DEV=$DEV,PROFILE=a2dp" "$@"
