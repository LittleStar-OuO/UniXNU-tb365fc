#!/usr/bin/env python3
"""Sign the root policy of a tb365fc userdata image with the owner key (VerifiedRoot envelopes, A04RPOL1 region).

iPadOS mounts the root only when it is authenticated. On tb365fc the kernel accepts the OS volume through the UniXNU
owner root chain (XNU_OWNER_ROOT_AUDIT, XNU_OWNER_ROOT_TRUST): NativeUfs verifies every 4096-byte block of the OS and
SystemOS windows against an envelope signed with the owner key, and answers the root authorization query for windows
whose signed volume UUID matches. The kernel carries only the public anchor (package/root-key/owner-public.pem,
scripts/build-tb365fc-root-key.py); the envelopes (about 73 MiB) live in the userdata partition right after the
SHNGROOT image and are written once with the installation. They change only with the IPSW or the key.

Envelope: kits/VerifiedRoot/tools/manifest.py of UniXNU (VROOT01: 160-byte header, RSA-3072 PKCS#1 v1.5 SHA-256
signature, SHA-256 of every 4096-byte block). Region (little endian), read by NativeUfs:
    0      "A04RPOL1", u32 version = 1, u32 count (<= 16), u64 total_bytes (header included)
    24     count entries {u64 offset (from the region start, 4096 aligned), u64 bytes}
    4064   SHA-256 of the 4096-byte header with these 32 bytes zero
    4096.. the envelopes
The region starts at the SHNGROOT image_bytes offset of userdata (the image end, 1 MiB aligned).

usage: make-tb365fc-root-policy.py --userdata userdata-root.img --private-key owner.pem --out DIR [--unixnu ../UniXNU]
       make-tb365fc-root-policy.py --selftest --private-key owner.pem   (rewrites package/root-key/selftest.json)
The private key stays with its owner; another builder generates its own key pair and replaces package/root-key/owner-public.pem.
"""
import argparse
import hashlib
import importlib.util
import io
import json
from pathlib import Path
import struct
import tempfile

HERE = Path(__file__).resolve().parent
DEVICE = HERE.parent
CONFIG = json.loads((HERE / 'tb365fc-root-policy.json').read_text())
MAGIC = b'A04RPOL1'
HEADER = 4096


def package_root():
    # The device package is package/ in the device repository and kits/tb365fc in the assembled tree.
    for candidate in (DEVICE / 'package/root-key', DEVICE / 'kits/tb365fc/root-key'):
        if (candidate / 'owner-public.pem').is_file():
            return candidate
    raise SystemExit('package/root-key/owner-public.pem not found')


def load_manifest(unixnu):
    for candidate in (DEVICE / 'kits/VerifiedRoot/tools/manifest.py',
                      Path(unixnu) / 'kits/VerifiedRoot/tools/manifest.py',
                      DEVICE.parent / 'UniXNU/kits/VerifiedRoot/tools/manifest.py'):
        if candidate.is_file():
            spec = importlib.util.spec_from_file_location('vr_manifest', candidate)
            module = importlib.util.module_from_spec(spec)
            spec.loader.exec_module(module)
            return module
    raise SystemExit('kits/VerifiedRoot/tools/manifest.py of UniXNU not found (--unixnu)')


class Window(io.RawIOBase):
    """Read exactly `size` bytes of `stream` from `offset`, hashing them for the SHNGROOT check."""
    def __init__(self, stream, offset, size):
        self.stream, self.left, self.digest = stream, size, hashlib.sha256()
        stream.seek(offset)

    def read(self, n=-1):
        if self.left == 0:
            return b''
        n = self.left if n is None or n < 0 else min(n, self.left)
        data = self.stream.read(n)
        if len(data) != n:
            raise SystemExit('short userdata image')
        self.left -= n
        self.digest.update(data)
        return data


def shngroot(stream):
    header = stream.read(4096)
    if header[:8] != b'SHNGROOT' or hashlib.sha256(header[:4064]).digest() != header[4064:]:
        raise SystemExit('not an SHNGROOT userdata image (magic or manifest SHA-256)')
    version, count, image_bytes = struct.unpack_from('<IIQ', header, 8)
    if version != 1 or not 0 < count <= 31 or image_bytes % (1 << 20):
        raise SystemExit('unexpected SHNGROOT manifest')
    windows = {}
    for index in range(count):
        name, offset, size, _, _, digest, _ = struct.unpack_from('<32sQQII32s40s', header, 24 + 128 * index)
        windows[name.rstrip(b'\0').decode()] = (offset, size, digest)
    return image_bytes, windows


def region(envelopes):
    if not 0 < len(envelopes) <= 16:
        raise SystemExit('1..16 envelopes')
    entries, cursor = [], HEADER
    for envelope in envelopes:
        entries.append((cursor, len(envelope)))
        cursor += (len(envelope) + 4095) // 4096 * 4096
    header = bytearray(HEADER)
    header[0:8] = MAGIC
    struct.pack_into('<IIQ', header, 8, 1, len(envelopes), cursor)
    for index, entry in enumerate(entries):
        struct.pack_into('<QQ', header, 24 + 16 * index, *entry)
    header[4064:] = hashlib.sha256(bytes(header[:4064]) + bytes(32)).digest()
    body = bytearray(cursor)
    body[:HEADER] = header
    for (offset, length), envelope in zip(entries, envelopes):
        body[offset:offset + length] = envelope
    return bytes(body)


def sign(manifest, stream, size, key, uuid, generation, work):
    public = package_root() / 'owner-public.pem'
    record = manifest.make(stream, size, key, public, uuid, generation, work)
    return (work / 'manifest.bin').read_bytes(), record


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--userdata', type=Path)
    parser.add_argument('--private-key', type=Path, required=True)
    parser.add_argument('--out', type=Path)
    parser.add_argument('--unixnu', default=str(DEVICE.parent / 'UniXNU'))
    parser.add_argument('--selftest', action='store_true')
    args = parser.parse_args()
    manifest = load_manifest(args.unixnu)
    if args.selftest:
        # Two blocks, the second short like the last block of the OS image: the kernel probe opens this envelope and
        # checks the blocks, then that a changed signature, header, hash table, block, key and generation are refused.
        data = bytes((i * 7 + 3) & 0xff for i in range(4096 + 1536))
        with tempfile.TemporaryDirectory() as work:
            envelope, _ = sign(manifest, io.BytesIO(data), len(data), args.private_key, CONFIG['selftest_uuid'], 1, Path(work))
        (package_root() / 'selftest.json').write_text(json.dumps(
            {'data': data.hex(), 'envelope': envelope.hex(), 'volume_uuid': CONFIG['selftest_uuid']}, indent=1) + '\n')
        print('package/root-key/selftest.json written')
        return
    if not args.userdata or not args.out:
        parser.error('--userdata and --out are required')
    args.out.mkdir(parents=True, exist_ok=True)
    envelopes, records = [], []
    with args.userdata.open('rb') as stream:
        image_bytes, windows = shngroot(stream)
        # NativeUfs places the region at the end of the farthest window, rounded up to 1 MiB; it must be image_bytes.
        farthest = max((offset + size + (1 << 20) - 1) >> 20 << 20 for offset, size, _ in windows.values())
        if farthest != image_bytes:
            raise SystemExit(f'the farthest window ends at {farthest:#x}, not at image_bytes {image_bytes:#x}')
        for name, uuid in CONFIG['windows'].items():
            if name not in windows:
                raise SystemExit(f'window {name} missing from the SHNGROOT manifest')
            offset, size, digest = windows[name]
            window = Window(stream, offset, size)
            with tempfile.TemporaryDirectory() as work:
                envelope, record = sign(manifest, window, size, args.private_key, uuid, CONFIG['generation'], Path(work))
            if window.digest.digest() != digest:
                raise SystemExit(f'window {name} does not match its SHNGROOT SHA-256')
            envelopes.append(envelope)
            records.append({'window': name, **record})
            print(f'{name}: {size} bytes, {record["block_count"]} blocks, volume {uuid}')
    data = region(envelopes)
    (args.out / 'rootpolicy.img').write_bytes(data)
    report = {'userdata_offset': image_bytes, 'bytes': len(data), 'sha256': hashlib.sha256(data).hexdigest(),
              'trusted_key_sha256': records[0]['trusted_key_sha256'], 'policies': records}
    (args.out / 'rootpolicy.json').write_text(json.dumps(report, indent=2) + '\n')
    print(f'rootpolicy.img: {len(data)} bytes at userdata offset {image_bytes:#x}')


if __name__ == '__main__':
    main()
