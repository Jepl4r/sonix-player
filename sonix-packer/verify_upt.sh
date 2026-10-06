#!/usr/bin/env bash
# Structural validation for a HiByOS/TempoTec UPT. This cannot prove hardware
# compatibility, but it catches corrupt ISO output, broken chunk/hash chains,
# changed kernels, and a missing Sonix install before an artifact is published.
set -euo pipefail

usage() {
	echo "Usage: verify_upt.sh IMAGE.upt [--sonix-v1] [--kernel-sha256 HASH]" >&2
	exit 2
}

[ "$#" -ge 1 ] || usage
UPT="$1"
shift
EXPECT_SONIX=false
EXPECTED_KERNEL=""
while [ "$#" -gt 0 ]; do
	case "$1" in
		--sonix-v1) EXPECT_SONIX=true ;;
		--kernel-sha256)
			shift
			[ "$#" -gt 0 ] || usage
			EXPECTED_KERNEL="$1"
			;;
		*) usage ;;
	esac
	shift
done

for tool in 7z md5sum sha256sum awk sed grep find sort unsquashfs; do
	command -v "$tool" >/dev/null 2>&1 || { echo "Error: $tool is required." >&2; exit 1; }
done
[ -f "$UPT" ] || { echo "Error: $UPT does not exist." >&2; exit 1; }
UPT="$(cd "$(dirname "$UPT")" && pwd)/$(basename "$UPT")"

WORK="$(mktemp -d "${TMPDIR:-/tmp}/sonix-verify-upt.XXXXXX")"
cleanup() { rm -rf "$WORK"; }
trap cleanup EXIT

7z t "$UPT" >/dev/null
7z x "$UPT" -o"$WORK/iso" -y >/dev/null
OTA="$WORK/iso/ota_v0"
[ -f "$OTA/ota_update.in" ] || { echo "Error: image has no ota_v0/ota_update.in." >&2; exit 1; }

metadata_for() {
	local wanted="$1" key="$2"
	awk -F= -v wanted="$wanted" -v key="$key" '
		{sub(/\r$/, "", $0)}
		$1 == "img_name" { active = ($2 == wanted) }
		active && $1 == key { print $2; exit }
	' "$OTA/ota_update.in"
}

verify_payload() {
	local stem="$1" label="$2"
	local declared_size declared_md5 full actual_size actual_md5 manifest
	declared_size="$(metadata_for "$stem" img_size)"
	declared_md5="$(metadata_for "$stem" img_md5 | tr '[:upper:]' '[:lower:]')"
	[ -n "$declared_size" ] && [[ "$declared_size" =~ ^[0-9]+$ ]] || {
		echo "Error: invalid $label size in ota_update.in." >&2; exit 1;
	}
	[[ "$declared_md5" =~ ^[0-9a-f]{32}$ ]] || {
		echo "Error: invalid $label MD5 in ota_update.in." >&2; exit 1;
	}

	mapfile -t chunks < <(find "$OTA" -maxdepth 1 -type f -regextype posix-extended \
		-regex ".*/${stem}\.[0-9]{4}\.[0-9a-fA-F]{32}" -print | sort)
	[ "${#chunks[@]}" -gt 0 ] || { echo "Error: no $label chunks found." >&2; exit 1; }

	full="$WORK/$stem"
	: > "$full"
	for chunk in "${chunks[@]}"; do cat "$chunk" >> "$full"; done
	actual_size="$(wc -c < "$full" | tr -d ' ')"
	actual_md5="$(md5sum "$full" | awk '{print $1}')"
	[ "$actual_size" = "$declared_size" ] || {
		echo "Error: $label is $actual_size bytes; metadata says $declared_size." >&2; exit 1;
	}
	[ "$actual_md5" = "$declared_md5" ] || {
		echo "Error: $label MD5 is $actual_md5; metadata says $declared_md5." >&2; exit 1;
	}

	manifest="$OTA/ota_md5_${stem}.${declared_md5}"
	[ -f "$manifest" ] || { echo "Error: $label MD5-chain file is missing." >&2; exit 1; }
	mapfile -t chain < <(tr -d '\r ' < "$manifest" | sed '/^$/d' | tr '[:upper:]' '[:lower:]')
	[ "${#chain[@]}" = "${#chunks[@]}" ] || {
		echo "Error: $label has ${#chunks[@]} chunks but ${#chain[@]} chain entries." >&2; exit 1;
	}

	local previous="$declared_md5" index=0 basename suffix chunk_md5
	for chunk in "${chunks[@]}"; do
		basename="$(basename "$chunk")"
		suffix="${basename##*.}"
		suffix="$(printf '%s' "$suffix" | tr '[:upper:]' '[:lower:]')"
		[ "$suffix" = "$previous" ] || {
			echo "Error: $basename names previous MD5 $suffix; expected $previous." >&2; exit 1;
		}
		chunk_md5="$(md5sum "$chunk" | awk '{print $1}')"
		[ "${chain[$index]}" = "$chunk_md5" ] || {
			echo "Error: chain entry $index for $label is ${chain[$index]}; chunk is $chunk_md5." >&2; exit 1;
		}
		previous="$chunk_md5"
		index=$((index + 1))
	done

	printf 'Verified %-7s %10s bytes  md5 %s  (%s chunks)\n' "$label" "$actual_size" "$actual_md5" "${#chunks[@]}"
}

verify_payload xImage kernel
verify_payload rootfs.squashfs rootfs

KERNEL_SHA256="$(sha256sum "$WORK/xImage" | awk '{print $1}')"
if [ -n "$EXPECTED_KERNEL" ] && [ "$KERNEL_SHA256" != "$EXPECTED_KERNEL" ]; then
	echo "Error: output kernel SHA-256 $KERNEL_SHA256 differs from stock $EXPECTED_KERNEL." >&2
	exit 1
fi
printf 'Kernel SHA-256: %s\n' "$KERNEL_SHA256"

if $EXPECT_SONIX; then
	LISTING="$WORK/rootfs.list"
	unsquashfs -ll "$WORK/rootfs.squashfs" > "$LISTING"
	grep -qE 'squashfs-root/usr/bin/sonix_player$' "$LISTING" || {
		echo "Error: rootfs has no usr/bin/sonix_player." >&2; exit 1;
	}
	if grep -qE 'squashfs-root/usr/bin/hiby_player$' "$LISTING"; then
		echo "Error: stock usr/bin/hiby_player remains in the rootfs." >&2
		exit 1
	fi
	INFO="$WORK/system-info.json"
	unsquashfs -cat "$WORK/rootfs.squashfs" usr/resource/sonix/components/system-info.json > "$INFO"
	grep -qE '"device-name"[[:space:]]*:[[:space:]]*"TempoTec V1"' "$INFO" || {
		echo "Error: system-info.json does not identify TempoTec V1." >&2; exit 1;
	}
	printf 'Verified Sonix player and TempoTec V1 identity in rootfs.\n'
fi

printf 'UPT validation passed: %s\n' "$UPT"
