#!/usr/bin/env bash
# Restore the A slot after the USB log reader ran from it.
set -euo pipefail
FB=/e/workspace/msys64/ucrt64/bin/fastboot.exe
BOOT=/e/workspace/Repository/UniXNU/out/tb365fc/test-bootloop-20261029/diag131-boot.img
INIT=/e/workspace/device-data/tb365fc-root-20261028-144622/stock/init_boot_a.img
VENDOR=/e/workspace/Repository/UniXNU/out/tb365fc/test-bootloop-20261029/diag154-vendor_boot.img
"$FB" devices | grep -q fastboot || { echo "device not in fastboot"; exit 1; }
"$FB" flash boot_a "$BOOT"
"$FB" flash init_boot_a "$INIT"
"$FB" flash vendor_boot_a "$VENDOR"
"$FB" reboot
echo "restored slot a"
