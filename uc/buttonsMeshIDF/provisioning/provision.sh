#!/usr/bin/env bash
#
# Write per-installation credentials into a device's "creds" NVS partition.
#
# This is the wired step: the firmware image itself contains no secrets, so a
# freshly flashed device does nothing until this has run against it.
#
#   ./provision.sh                      # autodetect port
#   ./provision.sh -p /dev/ttyUSB0      # explicit port
#   ./provision.sh -c other.csv -C esp32c3
#
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
CSV="$HERE/credentials.csv"
CHIP="auto"
PORT=""

usage() { sed -n '3,12p' "${BASH_SOURCE[0]}" | sed 's/^# \{0,1\}//'; exit "${1:-0}"; }

while getopts "c:p:C:h" opt; do
    case "$opt" in
        c) CSV="$OPTARG" ;;
        p) PORT="$OPTARG" ;;
        C) CHIP="$OPTARG" ;;
        h) usage 0 ;;
        *) usage 1 ;;
    esac
done

if [ ! -f "$CSV" ]; then
    echo "ERROR: $CSV not found." >&2
    echo "Copy credentials.csv.example to credentials.csv and fill it in," >&2
    echo "or migrate an existing build with ./from_sdkconfig.py." >&2
    exit 1
fi

# Offset and size come from partitions.csv, so the two can never drift apart.
PARTITIONS="$HERE/../partitions.csv"
read -r OFFSET SIZE < <(
    awk -F',' '/^creds[[:space:]]*,/ {gsub(/ /,"",$4); gsub(/ /,"",$5); print $4, $5}' "$PARTITIONS"
)

if [ -z "${OFFSET:-}" ]; then
    echo "ERROR: no 'creds' partition in $PARTITIONS" >&2
    exit 1
fi

echo "Partition 'creds' at $OFFSET, size $SIZE (from partitions.csv)"

# nvs_partition_gen.py ships with ESP-IDF; PlatformIO keeps its own copy.
GEN=""
for candidate in \
    "${IDF_PATH:-}/components/nvs_flash/nvs_partition_generator/nvs_partition_gen.py" \
    "$HOME/.platformio/packages/framework-espidf/components/nvs_flash/nvs_partition_generator/nvs_partition_gen.py"
do
    if [ -n "$candidate" ] && [ -f "$candidate" ]; then GEN="$candidate"; break; fi
done

if [ -z "$GEN" ]; then
    echo "ERROR: nvs_partition_gen.py not found." >&2
    echo "Source ESP-IDF's export.sh, or: pip install esp-idf-nvs-partition-gen" >&2
    exit 1
fi

# An explicit template works with both GNU (Linux) and BSD (macOS) mktemp.
# Keep the .bin inside a private directory: appending it to a temporary file
# name would leave the original file behind and create a new, unprotected file.
CREDS_TMP_DIR="$(mktemp -d "${TMPDIR:-/tmp}/domator-creds.XXXXXX")"
IMAGE="$CREDS_TMP_DIR/credentials.bin"
trap 'rm -f "$IMAGE"; rmdir "$CREDS_TMP_DIR"' EXIT

echo "Building NVS image from $(basename "$CSV")..."
python3 "$GEN" generate "$CSV" "$IMAGE" "$SIZE" >/dev/null

ESPTOOL=(python3 -m esptool)
command -v esptool.py >/dev/null 2>&1 && ESPTOOL=(esptool.py)

ARGS=(--chip "$CHIP")
[ -n "$PORT" ] && ARGS+=(--port "$PORT")

echo "Flashing to $OFFSET..."
"${ESPTOOL[@]}" "${ARGS[@]}" write_flash "$OFFSET" "$IMAGE"

echo
echo "Done. Only the credentials partition was written -- the application was"
echo "left untouched. Reset the device; it should now log its SSID and broker"
echo "on startup instead of refusing to start."
