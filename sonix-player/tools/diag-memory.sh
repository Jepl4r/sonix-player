#!/bin/sh
# Where the memory goes, and who is on the bus.
#
# Copy onto the device and run it. Everything it prints comes from /proc, so it
# costs nothing and changes nothing; it exists because the two questions worth
# asking about memory on this player -- "which process is holding it" and "which
# part of the player is holding it" -- both need numbers that only the device
# has.
#
#     sh /mnt/sdcard/diag-memory.sh

echo "=== free ==="
free

echo
echo "=== i quindici processi piu' grossi (VmRSS, kB) ==="
for d in /proc/[0-9]*; do
	[ -r "$d/status" ] || continue
	rss=$(awk '/^VmRSS/{print $2}' "$d/status" 2>/dev/null)
	[ -n "$rss" ] || continue
	name=$(awk '/^Name:/{print $2}' "$d/status" 2>/dev/null)
	echo "$rss ${d#/proc/} $name"
done | sort -rn | head -15

echo
echo "=== ogni dbus-daemon: chi lo ha avviato e con quali argomenti ==="
for d in /proc/[0-9]*; do
	[ -r "$d/status" ] || continue
	name=$(awk '/^Name:/{print $2}' "$d/status" 2>/dev/null)
	[ "$name" = "dbus-daemon" ] || continue
	pid=${d#/proc/}
	ppid=$(awk '/^PPid/{print $2}' "$d/status")
	rss=$(awk '/^VmRSS/{print $2}' "$d/status")
	pname=$(awk '/^Name:/{print $2}' "/proc/$ppid/status" 2>/dev/null)
	echo "--- pid $pid  padre $ppid ($pname)  VmRSS ${rss} kB"
	printf '    '
	tr '\0' ' ' <"$d/cmdline"
	echo
done

echo
echo "=== gli altri pezzi della catena Bluetooth ==="
for n in bluetoothd bt-agent bluealsa brcm_patchram_plus; do
	for d in /proc/[0-9]*; do
		[ -r "$d/status" ] || continue
		name=$(awk '/^Name:/{print $2}' "$d/status" 2>/dev/null)
		[ "$name" = "$n" ] || continue
		echo "$n pid ${d#/proc/} VmRSS $(awk '/^VmRSS/{print $2}' "$d/status") kB"
	done
done

echo
echo "=== il player ==="
for d in /proc/[0-9]*; do
	[ -r "$d/status" ] || continue
	name=$(awk '/^Name:/{print $2}' "$d/status" 2>/dev/null)
	# Il nome in /proc e' troncato a 15 caratteri, e lo script di avvio si
	# chiama quasi come il programma: si tiene solo quello con piu' di un thread.
	case "$name" in
	sonix_player*) ;;
	*) continue ;;
	esac
	[ "$(awk '/^Threads/{print $2}' "$d/status")" -gt 1 ] || continue
	# VmLck e' la riga che decide: se e' vicina a VmExe+VmLib e' il blocco
	# delle sole pagine di codice, se e' molto piu' grande e' ancora
	# mlockall che tiene in RAM anche il .bss mai scritto.
	grep -E '^(Name|VmSize|VmRSS|VmLck|VmData|VmStk|VmExe|VmLib|Threads)' "$d/status"
	echo
	echo "--- residente: quanto viene dai file e quanto no ---"
	# statm c'e' su ogni kernel; smaps no, e su questo non risponde. I campi
	# sono pagine da 4 kB: dimensione, residente, condivise (cioe' pagine di
	# file: eseguibile, librerie, icone), testo, librerie, dati.
	set -- $(cat "$d/statm" 2>/dev/null)
	if [ -n "$2" ]; then
		echo "   residente totale : $(( $2 * 4 )) kB"
		echo "   da file (icone, codice, librerie): $(( $3 * 4 )) kB   <- il kernel se le puo' riprendere"
		echo "   anonima (heap, stack, array)     : $(( ($2 - $3) * 4 )) kB   <- questa no"
	fi

	# I file di /proc dichiarano tutti dimensione zero, quindi un test "-s" li
	# scarta tutti -- ed e' il motivo per cui questa parte non ha mai stampato
	# niente. Si legge e si guarda se ne e' uscito qualcosa.
	awk '
		/^[0-9a-f]+-[0-9a-f]+ / { what = ($6 == "" ? "[anonimo]" : $6); next }
		/^Rss:/ { if ($2 > 64) printf "%8d  %s\n", $2, what }
	' "$d/smaps" 2>/dev/null | sort -rn | head -20 >/tmp/diag_maps.txt

	if [ -s /tmp/diag_maps.txt ]; then
		echo
		echo "--- le mappe residenti piu' grosse (kB) ---"
		cat /tmp/diag_maps.txt

		echo
		echo "--- sommate per tipo ---"
		awk '
			/^[0-9a-f]+-[0-9a-f]+ / { what = $6; next }
			/^Size:/ { sz = $2 }
			/^Rss:/ {
				if (what == "[heap]") { heap += $2 }
				else if (what == "[stack]") { mainstack += $2 }
				else if (what == "") {
					# Uno stack di thread e una mappa anonima grande quanto
					# il limite di stack, quindi si contano a parte.
					if (sz >= 1024) { big += $2; nbig++ } else { small += $2 }
				}
				else { files += $2 }
			}
			END {
				printf "   heap                                      : %6d kB\n", heap
				printf "   stack del thread principale               : %6d kB\n", mainstack
				printf "   anonime grandi (stack dei thread)         : %6d kB in %d mappe\n", big, nbig
				printf "   anonime piccole (.bss, mmap vari)         : %6d kB\n", small
				printf "   di file (codice, icone, librerie)         : %6d kB\n", files
			}
		' "$d/smaps" 2>/dev/null
	else
		echo "   (smaps non risponde: bastano i numeri sopra)"
	fi
	rm -f /tmp/diag_maps.txt
done
