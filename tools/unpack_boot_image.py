#!/usr/bin/env python3
"""Inspect and unpack Android boot / vendor_boot images without external tools.

The porting record needs the exact geometry the TB365FC bootloader expects:
load addresses, page size, header version, cmdline and the DTB. This script
prints all of them and can extract every payload so the addresses can be
copied into Resources/Configs/tb365fc.toml of the firmware repository.

Supported:
  * boot images   header v0, v1, v2, v3, v4 ("ANDROID!")
  * vendor_boot   header v3, v4 ("VNDRBOOT")

Examples:
  python3 unpack_boot_image.py boot.img
  python3 unpack_boot_image.py boot.img --json
  python3 unpack_boot_image.py boot.img --extract out/stock
  python3 unpack_boot_image.py vendor_boot.img --extract out/stock

The script never writes into the source image and never needs root.
"""

import argparse
import json
import os
import struct
import sys

BOOT_MAGIC = b"ANDROID!"
VENDOR_BOOT_MAGIC = b"VNDRBOOT"
FDT_MAGIC = 0xD00DFEED
DTBO_MAGIC = 0xD7B7AB1E


def align(value, page):
    return (value + page - 1) // page * page


def read_cstring(raw):
    end = raw.find(b"\0")
    if end >= 0:
        raw = raw[:end]
    return raw.decode("utf-8", "replace").strip()


def read_u32(data, offset):
    return struct.unpack_from("<I", data, offset)[0]


def read_u64(data, offset):
    return struct.unpack_from("<Q", data, offset)[0]


def parse_boot(data):
    """Return a dict describing a boot image and the payload offsets."""
    if len(data) < 48:
        raise ValueError("boot image shorter than a header")
    header_version = read_u32(data, 40)
    info = {
        "kind": "boot",
        "header_version": header_version,
        "magic": read_cstring(data[:8]),
        "file_bytes": len(data),
    }

    if header_version in (0, 1, 2):
        (kernel_size, kernel_addr, ramdisk_size, ramdisk_addr, second_size, second_addr,
         tags_addr, page_size) = struct.unpack_from("<8I", data, 8)
        if page_size < 2048 or page_size > 65536 or page_size & (page_size - 1):
            raise ValueError(f"implausible page size {page_size}")
        info.update({
            "kernel_size": kernel_size,
            "kernel_addr": kernel_addr,
            "ramdisk_size": ramdisk_size,
            "ramdisk_addr": ramdisk_addr,
            "second_size": second_size,
            "second_addr": second_addr,
            "tags_addr": tags_addr,
            "page_size": page_size,
            "cmdline": read_cstring(data[64:64 + 512] + data[608:608 + 1024]),
            "name": read_cstring(data[48:64]),
            "os_version": read_u32(data, 44),
        })
        if header_version >= 1:
            info["recovery_dtbo_size"] = read_u32(data, 1632)
            info["recovery_dtbo_offset"] = read_u64(data, 1636)
            info["header_size"] = read_u32(data, 1644)
        if header_version >= 2:
            info["dtb_size"] = read_u32(data, 1648)
            info["dtb_addr"] = read_u64(data, 1652)

        page = page_size
        cursor = page
        payloads = {}

        def take(name, size):
            nonlocal cursor
            start = align(cursor, page)
            if size:
                if start + size > len(data):
                    raise ValueError(f"{name} runs past the end of the image")
                payloads[name] = {"offset": start, "size": size}
            cursor = start + size

        take("kernel", kernel_size)
        take("ramdisk", ramdisk_size)
        take("second", second_size)
        if header_version >= 1 and info.get("recovery_dtbo_size"):
            take("recovery_dtbo", info["recovery_dtbo_size"])
        if header_version >= 2 and info.get("dtb_size"):
            take("dtb", info["dtb_size"])
        info["payloads"] = payloads
        info["layout_note"] = (
            "kernel_addr, ramdisk_addr, second_addr and tags_addr are absolute; "
            "boot_image base = kernel_addr - kernel_offset. Keep 0x8000 unless the "
            "stock image proves otherwise."
        )
        return info

    if header_version in (3, 4):
        (kernel_size, ramdisk_size, os_version, header_size) = struct.unpack_from("<4I", data, 8)
        info.update({
            "kernel_size": kernel_size,
            "ramdisk_size": ramdisk_size,
            "os_version": os_version,
            "header_size": header_size,
            "page_size": 4096,
            "cmdline": read_cstring(data[44:44 + 1536]),
            "kernel_addr": None,
            "ramdisk_addr": None,
            "tags_addr": None,
        })
        if header_version == 4:
            info["signature_size"] = read_u32(data, 1580)
        cursor = 4096
        payloads = {}
        for name, size in (("kernel", kernel_size), ("ramdisk", ramdisk_size)):
            start = align(cursor, 4096)
            if start + size > len(data):
                raise ValueError(f"{name} runs past the end of the image")
            payloads[name] = {"offset": start, "size": size}
            cursor = start + size
        if header_version == 4 and info.get("signature_size"):
            start = align(cursor, 4096)
            payloads["signature"] = {"offset": start, "size": info["signature_size"]}
        info["payloads"] = payloads
        info["layout_note"] = (
            "v3/v4 images have no load addresses in the header; the bootloader "
            "picks them. The UEFI FD base cannot be read from this image."
        )
        return info

    raise ValueError(f"unsupported boot header version {header_version}")


def parse_vendor_boot(data):
    if len(data) < 2112:
        raise ValueError("vendor_boot image shorter than a v3 header")
    header_version = read_u32(data, 8)
    if header_version not in (3, 4):
        raise ValueError(f"unsupported vendor_boot header version {header_version}")
    page_size, kernel_addr, ramdisk_addr, vendor_ramdisk_size = struct.unpack_from("<4I", data, 12)
    if page_size < 2048 or page_size > 65536 or page_size & (page_size - 1):
        raise ValueError(f"implausible page size {page_size}")
    info = {
        "kind": "vendor_boot",
        "header_version": header_version,
        "magic": read_cstring(data[:8]),
        "file_bytes": len(data),
        "page_size": page_size,
        "kernel_addr": kernel_addr,
        "ramdisk_addr": ramdisk_addr,
        "vendor_ramdisk_size": vendor_ramdisk_size,
        "cmdline": read_cstring(data[28:28 + 2048]),
        "tags_addr": read_u32(data, 2076),
        "name": read_cstring(data[2080:2096]),
        "header_size": read_u32(data, 2096),
        "dtb_size": read_u32(data, 2100),
        "dtb_addr": read_u64(data, 2104),
    }
    if header_version == 4:
        if len(data) < 2128:
            raise ValueError("vendor_boot v4 image shorter than a v4 header")
        info["vendor_ramdisk_table_size"] = read_u32(data, 2112)
        info["vendor_ramdisk_table_entry_num"] = read_u32(data, 2116)
        info["vendor_ramdisk_table_entry_size"] = read_u32(data, 2120)
        info["bootconfig_size"] = read_u32(data, 2124)

    page = page_size
    cursor = page
    payloads = {}

    def take(name, size):
        nonlocal cursor
        start = align(cursor, page)
        if size:
            if start + size > len(data):
                raise ValueError(f"{name} runs past the end of the image")
            payloads[name] = {"offset": start, "size": size}
        cursor = start + size

    take("vendor_ramdisk", vendor_ramdisk_size)
    take("dtb", info["dtb_size"])
    if header_version == 4:
        take("vendor_ramdisk_table", info.get("vendor_ramdisk_table_size", 0))
        take("bootconfig", info.get("bootconfig_size", 0))
    info["payloads"] = payloads
    return info


def describe(info):
    lines = []
    for key, value in info.items():
        if key == "payloads":
            continue
        if isinstance(value, int) and key not in ("header_version", "page_size", "file_bytes",
                                                  "os_version", "dtb_size",
                                                  "vendor_ramdisk_size"):
            lines.append(f"{key:28s} 0x{value:x}")
        else:
            lines.append(f"{key:28s} {value}")
    for name, payload in info.get("payloads", {}).items():
        lines.append(f"payload {name:20s} offset 0x{payload['offset']:x} size 0x{payload['size']:x}")
    return "\n".join(lines)


def extract(info, data, outdir):
    os.makedirs(outdir, exist_ok=True)
    written = []
    for name, payload in info.get("payloads", {}).items():
        path = os.path.join(outdir, f"{name}.img")
        with open(path, "wb") as handle:
            handle.write(data[payload["offset"]:payload["offset"] + payload["size"]])
        written.append(path)
    meta = os.path.join(outdir, f"{info['kind']}-header.json")
    with open(meta, "w", encoding="utf-8") as handle:
        json.dump({k: v for k, v in info.items() if k != "payloads"}, handle, indent=2)
        handle.write("\n")
    written.append(meta)
    return written


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("image", help="boot.img or vendor_boot.img")
    parser.add_argument("--json", action="store_true", help="print the header as JSON")
    parser.add_argument("--extract", metavar="DIR", help="write every payload plus the header JSON into DIR")
    args = parser.parse_args()

    with open(args.image, "rb") as handle:
        data = handle.read()
    if len(data) < 8:
        parser.error(f"{args.image} is empty or truncated")
    if data[:8] == BOOT_MAGIC:
        info = parse_boot(data)
    elif data[:8] == VENDOR_BOOT_MAGIC:
        info = parse_vendor_boot(data)
    elif read_u32(data, 0) in (FDT_MAGIC, DTBO_MAGIC):
        parser.error(f"{args.image} is a DTB/DTBO, use extract_fdt.py")
    else:
        parser.error(f"{args.image} is not an Android boot or vendor_boot image")

    if args.json:
        print(json.dumps(info, indent=2))
    else:
        print(describe(info))
    if args.extract:
        for path in extract(info, data, args.extract):
            print(f"wrote {path}")


if __name__ == "__main__":
    try:
        main()
    except (OSError, ValueError, struct.error) as error:
        print(f"error: {error}", file=sys.stderr)
        sys.exit(1)
