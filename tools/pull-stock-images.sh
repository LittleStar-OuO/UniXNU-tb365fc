#!/usr/bin/env bash
# Pull the stock Android images needed for the boot geometry and DTB.
#
# Requires root (Magisk/KernelSU) on the tablet. Only boot-critical,
# non-identifying partitions are pulled; the script refuses known sensitive
# partitions (persist, modemst*, nvram, ...) and user data.
#
# Usage:
#   ./pull-stock-images.sh                    # boot vendor_boot dtbo
#   ./pull-stock-images.sh -o out/stock boot dtbo
#   ./pull-stock-images.sh --list             # list /dev/block/by-name
#   ./pull-stock-images.sh -s SERIAL boot
set -eu

ADB=(adb)
OUT="out/stock"
LIST_ONLY=0
SENSITIVE='persist|nvram|nvdata|nvcfg|modem.*|fsg|fsc|radio|metadata|userdata|super'

usage() {
  sed -n '2,13p' "$0" | sed 's/^# \{0,1\}//'
}

while [ $# -gt 0 ]; do
  case "$1" in
    -s|--serial) ADB+=(-s "$2"); shift 2 ;;
    -o|--output) OUT="$2"; shift 2 ;;
    --list) LIST_ONLY=1; shift ;;
    -h|--help) usage; exit 0 ;;
    --*) echo "unknown option: $1" >&2; usage; exit 2 ;;
    *) break ;;
  esac
done

PARTITIONS=("$@")
if [ ${#PARTITIONS[@]} -eq 0 ]; then
  PARTITIONS=(boot vendor_boot dtbo)
fi

command -v adb >/dev/null 2>&1 || { echo "adb not found in PATH" >&2; exit 1; }
"${ADB[@]}" get-state >/dev/null 2>&1 || { echo "no adb device" >&2; exit 1; }

shell() { "${ADB[@]}" shell "$@" | tr -d '\r'; }

if ! shell "su -c 'id'" 2>/dev/null | grep -q 'uid=0'; then
  echo "root is required to read block devices" >&2
  exit 1
fi

if [ "$LIST_ONLY" -eq 1 ]; then
  shell "su -c 'ls -l /dev/block/by-name'"
  exit 0
fi

SLOT=$(shell getprop ro.boot.slot_suffix 2>/dev/null || true)
mkdir -p "$OUT"

for requested in "${PARTITIONS[@]}"; do
  base=${requested%_a}
  base=${base%_b}
  if printf '%s' "$base" | grep -qiE "^($SENSITIVE)$"; then
    echo "refusing sensitive partition: $requested (keep it out of the repository)" >&2
    continue
  fi

  partition=""
  for candidate in "${requested}${SLOT}" "$requested"; do
    if shell "su -c 'test -e /dev/block/by-name/$candidate && echo yes'" 2>/dev/null | grep -q yes; then
      partition=$candidate
      break
    fi
  done
  if [ -z "$partition" ]; then
    echo "skip: $requested (no such by-name entry)" >&2
    continue
  fi

  target="$OUT/$partition.img"
  echo "pulling $partition -> $target"
  if ! "${ADB[@]}" exec-out "su -c 'cat /dev/block/by-name/$partition'" > "$target"; then
    rm -f "$target"
    echo "failed: $partition" >&2
    continue
  fi
  if [ ! -s "$target" ]; then
    rm -f "$target"
    echo "failed: $partition produced an empty file" >&2
    continue
  fi

  if command -v sha256sum >/dev/null 2>&1; then
    sha256sum "$target"
  elif command -v shasum >/dev/null 2>&1; then
    shasum -a 256 "$target"
  fi
done

cat > "$OUT/SOURCE.txt" <<EOF
Pulled with pull-stock-images.sh on $(date -u +%Y-%m-%dT%H:%M:%SZ)
slot suffix: ${SLOT:-none}

These are complete stock partition images. Do not commit them: they contain
vendor code and may contain device-specific data. Record only hashes,
partition names, sizes and the extracted geometry.
EOF

echo "done: $OUT"
echo "next: python3 unpack_boot_image.py $OUT/boot*.img --extract $OUT/unpacked"
echo "      python3 extract_fdt.py $OUT/dtbo*.img -o $OUT/fdt --dtc"
