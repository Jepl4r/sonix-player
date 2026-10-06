#!/usr/bin/env bash
# Locate the official v1.upt in a downloaded file/folder and put the names the
# packer expects beside it. Archives from TempoTec are accepted as well as the
# Google Drive folder containing v1.upt directly.
set -euo pipefail

usage() {
	cat >&2 <<'EOF'
Usage: prepare_v1_stock.sh DOWNLOAD_PATH [PACKER_DIRECTORY]

DOWNLOAD_PATH may be an official v1.upt, a directory downloaded from the
TempoTec Google Drive folder, or the archive distributed for an older release.
EOF
	exit 2
}

[ "$#" -ge 1 ] && [ "$#" -le 2 ] || usage
command -v 7z >/dev/null 2>&1 || { echo "Error: 7z is required." >&2; exit 1; }

SOURCE="$1"
PACKER_DIR="${2:-$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)}"
[ -e "$SOURCE" ] || { echo "Error: $SOURCE does not exist." >&2; exit 1; }
mkdir -p "$PACKER_DIR"
PACKER_DIR="$(cd "$PACKER_DIR" && pwd)"

WORK="$(mktemp -d "${TMPDIR:-/tmp}/sonix-v1-stock.XXXXXX")"
cleanup() { rm -rf "$WORK"; }
trap cleanup EXIT

is_v1_upt() {
	local file="$1"
	[ -f "$file" ] || return 1
	# The update must be an ISO understood by 7-Zip and carry the OTA metadata,
	# not merely be any file somebody happened to call v1.upt.
	7z l "$file" 2>/dev/null | tr '\\' '/' | grep -E 'ota_v0/ota_update\.in$' >/dev/null
}

find_named_upt() {
	local root="$1"
	find "$root" -type f -iname 'v1.upt' -print -quit 2>/dev/null
}

CANDIDATE=""
if [ -f "$SOURCE" ] && [ "$(basename "$SOURCE" | tr '[:upper:]' '[:lower:]')" = "v1.upt" ]; then
	CANDIDATE="$SOURCE"
elif [ -d "$SOURCE" ]; then
	CANDIDATE="$(find_named_upt "$SOURCE")"
fi

# TempoTec V1.1 was distributed as an archive. Extract each top-level file to
# its own directory so equal names in unrelated files cannot overwrite one
# another. A raw UPT also opens in 7-Zip, but produces no nested v1.upt and is
# recognized by is_v1_upt below.
if [ -z "$CANDIDATE" ]; then
	index=0
	while IFS= read -r file; do
		if is_v1_upt "$file"; then
			CANDIDATE="$file"
			break
		fi
		out="$WORK/extracted-$index"
		mkdir -p "$out"
		if 7z x "$file" -o"$out" -y >/dev/null 2>&1; then
			CANDIDATE="$(find_named_upt "$out")"
			[ -z "$CANDIDATE" ] || break
		fi
		index=$((index + 1))
	done < <(if [ -d "$SOURCE" ]; then find "$SOURCE" -maxdepth 2 -type f -print; else printf '%s\n' "$SOURCE"; fi)
fi

[ -n "$CANDIDATE" ] || {
	echo "Error: no v1.upt was found in $SOURCE." >&2
	echo "Download the official TempoTec Variations V1 firmware, not a driver or instruction PDF." >&2
	exit 1
}
is_v1_upt "$CANDIDATE" || {
	echo "Error: $CANDIDATE has no ota_v0/ota_update.in and is not a supported V1 firmware image." >&2
	exit 1
}

cp -f "$CANDIDATE" "$PACKER_DIR/v1_original.upt"
printf 'Prepared %s (%s bytes)\n' "$PACKER_DIR/v1_original.upt" "$(wc -c < "$PACKER_DIR/v1_original.upt" | tr -d ' ')"

# Keep TempoTec's checksum-file wording when one accompanied the download. The
# packer substitutes the new image's MD5 while preserving everything else.
TEMPLATE=""
CANDIDATE_DIR="$(cd "$(dirname "$CANDIDATE")" && pwd)"
TEMPLATE="$(find "$CANDIDATE_DIR" -maxdepth 1 -type f -iname '*md5*.txt' -print -quit 2>/dev/null)"
if [ -z "$TEMPLATE" ] && [ -d "$SOURCE" ]; then
	TEMPLATE="$(find "$SOURCE" -type f -iname '*md5*.txt' -print -quit 2>/dev/null)"
fi
if [ -n "$TEMPLATE" ]; then
	cp -f "$TEMPLATE" "$PACKER_DIR/v1_md5_original.txt"
	printf 'Prepared checksum template %s\n' "$PACKER_DIR/v1_md5_original.txt"
else
	rm -f "$PACKER_DIR/v1_md5_original.txt"
	printf 'No checksum template found; the packer will write standard md5sum syntax.\n'
fi
