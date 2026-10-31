#!/usr/bin/env python3
"""Extract flattened device trees from DTB, DTBO or vendor_boot images.

Inputs:
  * a single .dtb
  * a .dtbo partition image (DTBO table magic 0xd7b7ab1e)
  * a vendor_boot.img (the embedded DTB is extracted first; if it is a DTBO
    container its entries are extracted as well)
  * a raw partition with concatenated DTBs (--raw-scan)

Each extracted blob is written as <name>-NN.dtb. With dtc available (or
--dtc PATH) every blob is also decompiled to <name>-NN.dts, which is the file
to diff against the UniXNU device tree.

Examples:
  python3 extract_fdt.py dtbo.img --dtc dtc
  python3 extract_fdt.py vendor_boot.img -o out/fdt
  python3 extract_fdt.py dtb_partition.bin --raw-scan
"""

import argparse
import os
import shutil
import struct
import subprocess
import sys

FDT_MAGIC = 0xD00DFEED
DTBO_MAGIC = 0xD7B7AB1E
VENDOR_BOOT_MAGIC = b"VNDRBOOT"
DTBO_ENTRY_SIZE = 32


def read_be32(data, offset):
    return struct.unpack_from(">I", data, offset)[0]


def read_le32(data, offset):
    return struct.unpack_from("<I", data, offset)[0]


def fdt_size(data, offset):
    """Return the totalsize of the FDT at offset, or None if it is invalid."""
    if offset + 40 > len(data) or read_be32(data, offset) != FDT_MAGIC:
        return None
    total = read_be32(data, offset + 4)
    if total < 40 or offset + total > len(data):
        return None
    version = read_be32(data, offset + 20)
    if version > 17:
        return None
    return total


def dtbo_entries(data):
    if len(data) < 32 or read_be32(data, 0) != DTBO_MAGIC:
        return None
    total_size = read_be32(data, 4)
    header_size = read_be32(data, 8)
    entry_size = read_be32(data, 12)
    entry_count = read_be32(data, 16)
    table_offset = read_be32(data, 20)
    if entry_size < DTBO_ENTRY_SIZE or header_size < 32 or header_size > len(data):
        raise ValueError("invalid DTBO table header")
    if table_offset + entry_count * entry_size > min(total_size, len(data)):
        raise ValueError("DTBO entry table runs past the image")
    entries = []
    for index in range(entry_count):
        base = table_offset + index * entry_size
        size = read_be32(data, base)
        offset = read_be32(data, base + 4)
        if offset < header_size or offset + size > min(total_size, len(data)):
            raise ValueError(f"DTBO entry {index} is outside the image")
        entries.append((index, offset, size))
    return entries


def vendor_boot_dtb(data):
    if len(data) < 2112 or data[:8] != VENDOR_BOOT_MAGIC:
        return None
    header_version = read_le32(data, 8)
    if header_version not in (3, 4):
        raise ValueError(f"unsupported vendor_boot header version {header_version}")
    page_size = read_le32(data, 12)
    ramdisk_size = read_le32(data, 24)
    size = read_le32(data, 2100)
    if not size:
        return b""
    if page_size < 2048 or page_size & (page_size - 1):
        raise ValueError("invalid vendor_boot page size")
    offset = (page_size + ramdisk_size + page_size - 1) // page_size * page_size
    if offset + size > len(data):
        raise ValueError("vendor_boot DTB runs past the image")
    return data[offset:offset + size]


def raw_scan(data):
    blobs = []
    offset = 0
    while offset + 40 <= len(data):
        size = fdt_size(data, offset)
        if size is None:
            offset += 4
            continue
        blobs.append((offset, data[offset:offset + size]))
        offset += (size + 3) & ~3
    return blobs


def unique_name(directory, stem, index, suffix):
    return os.path.join(directory, f"{stem}-{index:02d}{suffix}")


def emit(directory, stem, blobs, dtc):
    written = []
    for index, blob in enumerate(blobs):
        path = unique_name(directory, stem, index, ".dtb")
        with open(path, "wb") as handle:
            handle.write(blob)
        written.append(path)
        if dtc:
            dts = unique_name(directory, stem, index, ".dts")
            result = subprocess.run([dtc, "-I", "dtb", "-O", "dts", "-o", dts, path],
                                    capture_output=True, text=True)
            if result.returncode != 0:
                print(f"dtc failed for {path}: {result.stderr.strip()}", file=sys.stderr)
            else:
                written.append(dts)
    return written


def split(path):
    with open(path, "rb") as handle:
        data = handle.read()
    stem = os.path.splitext(os.path.basename(path))[0]

    dtb = vendor_boot_dtb(data)
    if dtb is not None:
        if not dtb:
            raise ValueError("vendor_boot image has no DTB")
        entries = dtbo_entries(dtb)
        if entries is not None:
            blobs = [dtb[offset:offset + size] for _, offset, size in entries]
            return stem, blobs
        return stem, [dtb]

    entries = dtbo_entries(data)
    if entries is not None:
        return stem, [data[offset:offset + size] for _, offset, size in entries]

    if fdt_size(data, 0) is not None:
        return stem, [data]

    raise ValueError("no DTB/DTBO found; pass --raw-scan for a raw partition")


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("image", help="DTB, DTBO, vendor_boot.img or raw partition")
    parser.add_argument("-o", "--output", default=".", help="output directory (default: .)")
    parser.add_argument("--dtc", nargs="?", const="dtc", metavar="PATH",
                        help="decompile with dtc; PATH defaults to dtc in PATH")
    parser.add_argument("--raw-scan", action="store_true",
                        help="scan the file for concatenated FDT magic instead of one image")
    args = parser.parse_args()

    with open(args.image, "rb") as handle:
        data = handle.read()
    stem = os.path.splitext(os.path.basename(args.image))[0]

    if args.raw_scan:
        blobs = [blob for _, blob in raw_scan(data)]
        if not blobs:
            parser.error("no FDT magic found; is this a raw DTB partition?")
    else:
        try:
            stem, blobs = split(args.image)
        except ValueError as error:
            parser.error(str(error))

    dtc = None
    if args.dtc:
        dtc = shutil.which(args.dtc)
        if dtc is None:
            parser.error(f"dtc not found: {args.dtc}; install device-tree-compiler or drop --dtc")

    os.makedirs(args.output, exist_ok=True)
    for path in emit(args.output, stem, blobs, dtc):
        print(f"wrote {path}")


if __name__ == "__main__":
    try:
        main()
    except (OSError, ValueError) as error:
        print(f"error: {error}", file=sys.stderr)
        sys.exit(1)
