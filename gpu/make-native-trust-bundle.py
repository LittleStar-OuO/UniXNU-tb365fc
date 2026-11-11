#!/usr/bin/env python3
"""Build the owner trust-cache bundle for the NativeMali userspace pieces.

Layout (same as Sheng's make-owner-trust-bundle.py and the loader's
ValidTrustCaches): one owner module (version 2, sorted SHA-256 CDHashes of
our ad-hoc signed binaries) followed by the two unchanged original Apple
modules, so platform binaries stay trusted.

  make-native-trust-bundle.py ORIGINAL.bundle OUTPUT.bundle PROBE DYLD [MORE...]
"""
import hashlib
import json
import struct
import sys
import uuid
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent / 'tools'))
from adhoc_cdhash import cdhash  # noqa: E402


def main():
    if len(sys.argv) < 5:
        sys.exit(__doc__)
    original = Path(sys.argv[1]).read_bytes()
    output = Path(sys.argv[2])
    binaries = sys.argv[3:]
    count = struct.unpack_from('<I', original)[0]
    if not 0 < count <= 128:
        sys.exit('original bundle count is not plausible')
    starts = [struct.unpack_from('<I', original, 4 + 4 * i)[0] for i in range(count)]
    originals = [original[starts[i]:(starts[i + 1] if i + 1 < count else len(original))]
                 for i in range(count)]
    for module in originals:
        if struct.unpack_from('<I', module)[0] != 2:
            sys.exit('original bundle is not made of version-2 modules')
    hashes = sorted({bytes.fromhex(cdhash(path)) for path in binaries})
    identifier = uuid.uuid5(uuid.NAMESPACE_URL,
                            'org.unixnu.native-mali:' + ''.join(h.hex() for h in hashes))
    own = struct.pack('<I16sI', 2, identifier.bytes, len(hashes))
    own += b''.join(h + bytes([2, 0, 0, 0]) for h in hashes)
    modules = [own, *originals]
    cursor, offsets = 4 + 4 * len(modules), []
    for module in modules:
        offsets.append(cursor)
        cursor += len(module)
    bundle = struct.pack(f'<{1 + len(modules)}I', len(modules), *offsets) + b''.join(modules)

    # Same structural checks as DarwinBoot ValidTrustCaches.
    n = struct.unpack_from('<I', bundle)[0]
    assert 0 < n <= 128
    for i in range(n):
        start = struct.unpack_from('<I', bundle, 4 + 4 * i)[0]
        end = struct.unpack_from('<I', bundle, 8 + 4 * i)[0] if i + 1 < n else len(bundle)
        assert start >= 4 + 4 * n and not start & 3 and end <= len(bundle) and end - start >= 24
        assert struct.unpack_from('<I', bundle, start)[0] == 2
        entries = struct.unpack_from('<I', bundle, start + 20)[0]
        assert end - start == 24 + entries * 24
        for e in range(entries):
            at = start + 24 + e * 24
            assert 1 <= bundle[at + 20] <= 4
            assert not e or bundle[at - 24:at - 4] < bundle[at:at + 20]

    output.write_bytes(bundle)
    report = {'bundle_sha256': hashlib.sha256(bundle).hexdigest(),
              'owner_cdhashes': [h.hex() for h in hashes],
              'original_modules': count,
              'binaries': [str(p) for p in binaries]}
    output.with_suffix('.json').write_text(json.dumps(report, indent=2) + '\n')
    print(json.dumps(report, indent=2))


if __name__ == '__main__':
    main()
