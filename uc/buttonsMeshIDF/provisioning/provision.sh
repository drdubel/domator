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
PROVISION_PYTHON="${PROVISION_PYTHON:-python3}"

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

# Prefer the installed module; newer ESP-IDF scripts only wrap this module.
# Older ESP-IDF installations may still provide a standalone generator.
GEN=("$PROVISION_PYTHON" -m esp_idf_nvs_partition_gen)
if ! "${GEN[@]}" --help >/dev/null 2>&1; then
    GEN=()
    for candidate in \
        "${IDF_PATH:-}/components/nvs_flash/nvs_partition_generator/nvs_partition_gen.py" \
        "$HOME/.platformio/packages/framework-espidf/components/nvs_flash/nvs_partition_generator/nvs_partition_gen.py"
    do
        if [ -f "$candidate" ] && "$PROVISION_PYTHON" "$candidate" --help >/dev/null 2>&1; then
            GEN=("$PROVISION_PYTHON" "$candidate")
            break
        fi
    done
fi

if [ "${#GEN[@]}" -eq 0 ]; then
    echo "ERROR: NVS generator is unavailable in Python: $PROVISION_PYTHON" >&2
    echo "Activate a provisioning virtual environment and install its tools:" >&2
    echo "  python -m pip install -r \"$HERE/requirements.txt\"" >&2
    echo "See $HERE/README.md (Setup)." >&2
    exit 1
fi

ESPTOOL=("$PROVISION_PYTHON" -m esptool)
if ! "${ESPTOOL[@]}" --help >/dev/null 2>&1; then
    ESPTOOL_SCRIPT="$(command -v esptool.py || true)"
    if [ -n "$ESPTOOL_SCRIPT" ] && "$PROVISION_PYTHON" "$ESPTOOL_SCRIPT" --help >/dev/null 2>&1; then
        ESPTOOL=("$PROVISION_PYTHON" "$ESPTOOL_SCRIPT")
    else
        echo "ERROR: esptool is unavailable in Python: $PROVISION_PYTHON" >&2
        echo "Install provisioning/requirements.txt in your active virtual environment." >&2
        exit 1
    fi
fi

# An explicit template works with both GNU (Linux) and BSD (macOS) mktemp.
# Keep the .bin inside a private directory: appending it to a temporary file
# name would leave the original file behind and create a new, unprotected file.
CREDS_TMP_DIR="$(mktemp -d "${TMPDIR:-/tmp}/domator-creds.XXXXXX")"
IMAGE="$CREDS_TMP_DIR/credentials.bin"
trap 'rm -f "$IMAGE"; rmdir "$CREDS_TMP_DIR"' EXIT

echo "Building NVS image from $(basename "$CSV")..."
"${GEN[@]}" generate "$CSV" "$IMAGE" "$SIZE" >/dev/null

ARGS=(--chip "$CHIP")
[ -n "$PORT" ] && ARGS+=(--port "$PORT")

echo "Flashing to $OFFSET..."
"${ESPTOOL[@]}" "${ARGS[@]}" write_flash "$OFFSET" "$IMAGE"

echo
echo "Done. Only the credentials partition was written -- the application was"
echo "left untouched. Reset the device; it should now log its SSID and broker"
echo "on startup instead of refusing to start."
