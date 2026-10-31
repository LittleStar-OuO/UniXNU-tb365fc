#!/usr/bin/env python3
# Compile the board PMGR nub with the actual source-kernel ABI and archive it for -force_load.
# Adapted from 57UU/UniXNU-sirius scripts/build-sirius-pmgr.py for the tb365fc build tree.
import argparse
import json
from pathlib import Path
import struct
import subprocess

DEFAULT_TREE = '/mnt/e/workspace/Repository/UniXNU/out/tb365fc/ipados-17.7.11-21H461/tb365fc/tree'

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--tree', type=Path, default=Path(DEFAULT_TREE), help='assembled UniXNU build tree')
args = parser.parse_args()

ROOT = args.tree.resolve()
source = ROOT / 'kits/tb365fc/SiriusPmgr.cpp'
OUT = ROOT / 'build/sheng-platform-power'
OUT.mkdir(parents=True, exist_ok=True)
obj = OUT / 'SiriusPmgr.o'
record = json.loads((ROOT / 'build/xnu/ios-obj/DEVELOPMENT_ARM64_BCM2837/iokit/DEVELOPMENT/IOPlatformExpert.cpo.json').read_text().strip().rstrip(','))
command, index = [], 0
arguments = record['arguments']
while index < len(arguments):
    value = arguments[index]
    if value == record['file']:
        command.append(str(source))
    elif value == '-o':
        command.extend(['-o', str(obj)])
        index += 1
    elif value == '-Xclang' and arguments[index + 1].startswith('-W'):
        index += 1
    elif not value.startswith('-W'):
        command.append(value)
    index += 1
command.extend(['-Wall', '-Wextra', '-Werror', '-Wno-unused-parameter',
                '-Wno-c99-designator', '-Wno-reorder-init-list', '-Wno-deprecated-enum-enum-conversion',
                '-Wno-unnecessary-virtual-specifier', '-Wno-unknown-warning-option',
                '-Wno-missing-designated-field-initializers', '-I', str(ROOT / '.cache/xnu-work/osfmk')])
if not source.is_file():
    raise SystemExit(f'missing PMGR source: {source}')
result = subprocess.run(command, cwd=record['directory'], stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
(OUT / 'compile-command.json').write_text(json.dumps(command, indent=2) + '\n')
(OUT / 'compile.log').write_text(result.stdout)
if result.returncode:
    raise SystemExit(result.stdout[-3500:])
data = obj.read_bytes()
magic, cpu, subtype, kind, count, size = struct.unpack_from('<6I', data)
if (magic, cpu, subtype, kind) != (0xfeedfacf, 0x100000c, 0, 1):
    raise SystemExit('Unexpected platform object architecture')
offset = 32
deployment = None
for _ in range(count):
    cmd, length = struct.unpack_from('<II', data, offset)
    if cmd == 0x32:
        deployment = struct.unpack_from('<II', data, offset + 8)
    offset += length
if deployment != (2, 0x110000):
    raise SystemExit(f'Unexpected platform deployment {deployment}')
archive = OUT / 'libShengPlatformPower.a'
archive.unlink(missing_ok=True)
# ld64 needs the BSD __.SYMDEF table, so use cctools ar from the UniXNU toolchain.
ar = Path('/opt/unixnu-linux/CommandLineTools/usr/bin/ar')
subprocess.run([str(ar) if ar.is_file() else 'ar', 'rcs', str(archive), str(obj)], check=True)
print('tb365fc PMGR nub: -Werror build and ARM64/iOS 17 object audit passed')
