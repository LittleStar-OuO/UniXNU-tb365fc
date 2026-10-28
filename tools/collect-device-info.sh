#!/usr/bin/env bash
# Collect the TB365FC facts the porting record needs, without pulling user data.
#
# Reads build props, kernel command line, CPU/memory info, the partition map and
# (with root) /proc/iomem and the live FDT. getprop, cmdline, bootconfig, cpuinfo
# and dmesg outputs are filtered for serials, IMEI/MEID, MACs, Android ID and FRP;
# the full unfiltered getprop output is never written to disk.
#
# Usage:
#   ./collect-device-info.sh                 # single attached device
#   ./collect-device-info.sh -s SERIAL       # pick a device
#   ./collect-device-info.sh -o out/inv      # output directory
#   ./collect-device-info.sh --dmesg         # also collect a filtered dmesg
set -eu

ADB=(adb)
OUT=""
WANT_DMESG=0
HAS_ROOT=0

usage() {
  sed -n '2,13p' "$0" | sed 's/^# \{0,1\}//'
}

while [ $# -gt 0 ]; do
  case "$1" in
    -s|--serial) ADB+=(-s "$2"); shift 2 ;;
    -o|--output) OUT="$2"; shift 2 ;;
    --dmesg) WANT_DMESG=1; shift ;;
    -h|--help) usage; exit 0 ;;
    *) echo "unknown option: $1" >&2; usage; exit 2 ;;
  esac
done

if ! command -v adb >/dev/null 2>&1; then
  echo "adb not found in PATH" >&2
  exit 1
fi

"${ADB[@]}" get-state >/dev/null 2>&1 || { echo "no adb device (check USB debugging)" >&2; exit 1; }

STAMP=$(date +%Y%m%d-%H%M%S)
OUT=${OUT:-out/device-info-$STAMP}
mkdir -p "$OUT"

shell() { "${ADB[@]}" shell "$@" | tr -d '\r'; }
root_shell() { "${ADB[@]}" shell "su -c '$1'" | tr -d '\r'; }

# Redact key=value and key = "value" pairs whose key names a device identifier.
redact_ids() {
  sed -E 's/([A-Za-z0-9_.-]*(serial|macaddr|mac_addr|btmacaddr|wifimacaddr|imei|meid|android_id|frp)[A-Za-z0-9_.-]*[[:space:]]*=[[:space:]]*)("[^"]*"|[^[:space:]]+)/\1<redacted>/Ig'
}

if root_shell id 2>/dev/null | grep -q 'uid=0'; then
  HAS_ROOT=1
fi

echo "collecting into $OUT (root: $([ "$HAS_ROOT" -eq 1 ] && echo yes || echo no))"

# Build identity: no serial, no unique ids.
{
  for prop in ro.product.device ro.product.name ro.product.model ro.product.manufacturer \
              ro.board.platform ro.boot.hardware ro.boot.hardware.sku ro.boot.hardware.subtype \
              ro.boot.dtb_idx ro.boot.dtbo_idx ro.boot.slot_suffix ro.boot.verifiedbootstate \
              ro.boot.flash.locked ro.build.version.release ro.build.version.sdk \
              ro.build.fingerprint ro.system.build.fingerprint; do
    value=$(shell getprop "$prop" || true)
    printf '%s=%s\n' "$prop" "$value"
  done
} > "$OUT/build-identity.txt"

shell getprop \
  | grep -viE 'serial|imei|meid|macaddr|mac_addr|bt_mac|wifi_mac|android_id|frp|gsm\.sim|persist\.radio\.imei' \
  > "$OUT/props-filtered.txt" || true

shell cat /proc/cmdline | redact_ids > "$OUT/cmdline.txt" 2>/dev/null || true
shell cat /proc/cpuinfo \
  | grep -viE '^[[:space:]]*serial[[:space:]]*:' \
  > "$OUT/cpuinfo.txt" 2>/dev/null || true
shell cat /proc/meminfo > "$OUT/meminfo.txt" 2>/dev/null || true
shell cat /proc/partitions > "$OUT/partitions.txt" 2>/dev/null || true
shell cat /proc/bootconfig | redact_ids > "$OUT/bootconfig.txt" 2>/dev/null || true
shell ls -l /dev/block/by-name > "$OUT/block-by-name.txt" 2>/dev/null || true
shell 'for d in /sys/block/*; do printf "%s size=%s lbs=%s\n" "$d" "$(cat "$d/size" 2>/dev/null)" "$(cat "$d/queue/logical_block_size" 2>/dev/null)"; done' \
  > "$OUT/block-queue.txt" 2>/dev/null || true
shell getprop ro.boot.boot_devices > "$OUT/boot-devices.txt" 2>/dev/null || true
shell wm size > "$OUT/wm-size.txt" 2>/dev/null || true
shell wm density > "$OUT/wm-density.txt" 2>/dev/null || true

if [ "$HAS_ROOT" -eq 1 ]; then
  root_shell 'cat /proc/iomem' > "$OUT/iomem.txt" 2>/dev/null || true
  root_shell 'ls /sys/firmware/devicetree/base' > "$OUT/devicetree-nodes.txt" 2>/dev/null || true
  "${ADB[@]}" exec-out "su -c 'cat /sys/firmware/fdt'" > "$OUT/fdt.dtb" 2>/dev/null || true
  if [ -s "$OUT/fdt.dtb" ] && [ "$(od -An -tx1 -N4 "$OUT/fdt.dtb" | tr -d ' \n')" = "d00dfeed" ]; then
    echo "live FDT: $OUT/fdt.dtb ($(wc -c < "$OUT/fdt.dtb") bytes)"
  else
    rm -f "$OUT/fdt.dtb"
    echo "warning: /sys/firmware/fdt is not a readable FDT; use the dtbo partition instead" >&2
  fi

  if [ "$WANT_DMESG" -eq 1 ]; then
    root_shell 'dmesg' \
      | grep -viE 'serial|imei|meid|macaddr|mac_addr|bt_mac|wifi_mac|android_id|frp' \
      > "$OUT/dmesg-filtered.txt" 2>/dev/null || true
  fi
fi

cat > "$OUT/NOTES.txt" <<EOF
Collected $(date -u +%Y-%m-%dT%H:%M:%SZ)
Root available: $([ "$HAS_ROOT" -eq 1 ] && echo yes || echo no)

Files:
  build-identity.txt   selected build props (safe to share)
  props-filtered.txt   getprop with serial/IMEI/MAC/android_id/FRP lines removed
  cmdline.txt          kernel command line (identifier values redacted)
  cpuinfo.txt          /proc/cpuinfo with the Serial line removed
  meminfo.txt          /proc/meminfo
  partitions.txt       /proc/partitions (size in 1K blocks)
  bootconfig.txt       /proc/bootconfig when present (identifier values redacted)
  block-by-name.txt    symlink -> major:minor map
  block-queue.txt      /sys/block size and logical_block_size per device
  boot-devices.txt     ro.boot.boot_devices (UFS LUN vs mmcblk)
  wm-size.txt          display logical size
  wm-density.txt       display density
  iomem.txt            /proc/iomem (root)
  fdt.dtb              live FDT from /sys/firmware/fdt (root); may embed
                       mac-address/serial properties, keep it local
  devicetree-nodes.txt top-level DT nodes (root)
  dmesg-filtered.txt   dmesg with identifier-bearing lines removed (--dmesg)

Review every file before committing it to a public repository. If any file
contains a device identifier, keep it out of the commit and record only the
derived hardware values in porting.toml.
EOF

echo "done: $OUT"
echo "next: ./pull-stock-images.sh -o $OUT/stock   (boot, vendor_boot, dtbo)"
