#!/usr/bin/env python3
"""Print the CDHash of an ad-hoc signed Mach-O (ld64.lld -adhoc_codesign).

Parses LC_CODE_SIGNATURE, finds the CodeDirectory blob (0xfade0c02) and
hashes it with the algorithm its hashType names (2 = SHA-256, 3 = SHA-384).
AMFI and the trust cache key entries by this hash.
"""
import hashlib
import struct
import sys


def cdhash(path):
    data = open(path, 'rb').read()
    magic, _, _, _, ncmds, sizeofcmds, _ = struct.unpack_from('<IiiIIII', data, 0)
    assert magic == 0xfeedfacf, 'not a 64-bit Mach-O'
    off = 32
    sig = None
    for _ in range(ncmds):
        cmd, size = struct.unpack_from('<II', data, off)
        if cmd == 0x1d:  # LC_CODE_SIGNATURE
            sig = struct.unpack_from('<II', data, off + 8)
        off += size
    assert sig, 'no LC_CODE_SIGNATURE (link with -adhoc_codesign)'
    dataoff, datasize = sig
    sigdata = data[dataoff:dataoff + datasize]
    _, _, count = struct.unpack_from('>III', sigdata, 0)
    assert 0 < count < 64
    for i in range(count):
        _, at = struct.unpack_from('>II', sigdata, 12 + i * 8)
        blobmagic, bloblen = struct.unpack_from('>II', sigdata, at)
        if blobmagic != 0xfade0c02:
            continue
        cd = sigdata[at:at + bloblen]
        hash_type = cd[37]
        algo = {1: 'sha1', 2: 'sha256', 3: 'sha384'}[hash_type]
        digest = hashlib.new(algo, cd).digest()
        return digest[:20].hex()
    raise SystemExit('no CodeDirectory in the signature blobs')


if __name__ == '__main__':
    for path in sys.argv[1:]:
        print(cdhash(path), path)
