#!/usr/bin/env python3
"""Pack the NativeMali userspace pieces into an NGPUADD2 boot-payload addon.

Same container as Sheng's make-gpu-addon.py (read by the kernel's
native_gpu_addon_install into /private/var/ngpu): "NGPUADD2", u32 count,
u32 0, u64 total bytes, then per file u32 name bytes (with NUL), u32 mode,
u64 size, u64 zlib size, padded name, padded stream.

  make-native-gpu-addon.py OUTPUT
Reads gpu/out/mali-vk-probe, gpu/out/libvulkan_panfrost.dylib and the launchd
job below.
"""
import struct
import sys
import zlib
from pathlib import Path

ROOT = Path(__file__).resolve().parent
OUT = ROOT / 'out'
SCRIPT = 'mkdir /private/var/ngpu/.probe.lock 2>/dev/null || exit 0; ' \
         'echo d183 probe-start > /dev/console; ' \
         '/private/var/ngpu/bin/mali-vk-probe; ' \
         'echo d183 probe-rc=$? > /dev/console'


def job_plist(label, script, system_session=False):
    session = '    <key>LimitLoadToSessionType</key><string>System</string>\n' if system_session else ''
    return ('<?xml version="1.0" encoding="UTF-8"?>\n'
            '<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" '
            '"http://www.apple.com/DTDs/PropertyList-1.0.dtd">\n'
            '<plist version="1.0">\n<dict>\n'
            f'    <key>Label</key><string>{label}</string>\n'
            '    <key>ProgramArguments</key>\n'
            f'    <array><string>/bin/sh</string><string>-c</string><string>{script}</string></array>\n'
            '    <key>RunAtLoad</key><true/>\n'
            '    <key>KeepAlive</key><false/>\n'
            f'{session}'
            '    <key>ProcessType</key><string>Interactive</string>\n'
            '    <key>StandardOutPath</key><string>/dev/console</string>\n'
            '    <key>StandardErrorPath</key><string>/dev/console</string>\n'
            '</dict>\n</plist>\n')


JOB = job_plist('com.unixnu.mali-vk-probe', SCRIPT)
# launchd may enumerate the LaunchDaemons directory without the overlay's new
# names, so the same job is offered under existing RunAtLoad names; the shell
# lock above keeps only the first launch running the probe. bluetoothd and the
# FDR loader did not exec on this board; keybagd/cfprefsd/aggregated/logd are
# the daemons the tb365fc exec record shows running on every boot.
JOBS_REPLACED = {label: job_plist(label, SCRIPT, system_session=(label == 'com.apple.iomfb_fdr_loader'))
                 for label in ('com.apple.iomfb_fdr_loader', 'com.apple.bluetoothd',
                               'com.apple.keybagd', 'com.apple.cfprefsd.xpc.daemon',
                               'com.apple.aggregated', 'com.apple.logd')}


def pad(data):
    return data + bytes(-len(data) % 8)


def main():
    if len(sys.argv) != 2:
        sys.exit(__doc__)
    output = Path(sys.argv[1]).resolve()
    probe = OUT / 'mali-vk-probe'
    dylib = OUT / 'libvulkan_panfrost.dylib'
    for path in (probe, dylib):
        if not path.is_file():
            sys.exit(f'missing {path}')
    files = [
        ('bin/mali-vk-probe', 0o755, probe.read_bytes()),
        ('bin/libvulkan_panfrost.dylib', 0o644, dylib.read_bytes()),
        ('daemons/com.unixnu.mali-vk-probe.plist', 0o644, JOB.encode()),
    ]
    for label, plist in sorted(JOBS_REPLACED.items()):
        files.append(('daemons/' + label + '.plist', 0o644, plist.encode()))
    body = b''
    for name, mode, data in files:
        encoded = name.encode() + b'\0'
        if len(encoded) > 160 or '..' in name.split('/') or name.startswith('/'):
            sys.exit('bad addon path ' + name)
        packed = zlib.compress(data, 9)
        body += struct.pack('<IIQQ', len(encoded), mode, len(data), len(packed))
        body += pad(encoded) + pad(packed)
        print(f'{mode:o} {len(data):>10} {len(packed):>9} {name}')
    blob = b'NGPUADD2' + struct.pack('<IIQ', len(files), 0, 24 + len(body)) + body
    output.write_bytes(blob)
    print(f'{output}: {len(files)} files, {len(blob)} bytes')


if __name__ == '__main__':
    main()
