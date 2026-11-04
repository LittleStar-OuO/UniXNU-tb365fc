#!/usr/bin/env python3
"""Build the TB365FC legacy display late archive.

Uses the frozen display ABI reference (build/xnu-prelink candidate and the
QEMU gdb observation) and UniXNU's audit, then compiles the original
NativeLegacyDisplay/NativeGOPPresenter classes, the official
LegacyDisplayProbe and temporary key/HID probe stubs into the late archive
the kernel force-loads when CONFIG_XNU_NATIVE_LEGACY_DISPLAY=1.
"""
import hashlib
import json
from pathlib import Path
import struct
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / 'build/native-legacy-display'
OUT.mkdir(parents=True, exist_ok=True)
sys.path.insert(0, str(ROOT / 'kits/XnuPrelink'))
from native_prelink import macho, backed_offset, exports


def run(cmd, **kw):
    return subprocess.check_output([str(x) for x in cmd], timeout=180, **kw)


def require(v, why):
    if not v:
        raise SystemExit(why)


def sha(p):
    return hashlib.sha256(Path(p).read_bytes()).hexdigest()


print(run(['python3', ROOT / 'scripts/audit-mobile-graphics-legacy.py'], text=True), end='')
audit = json.loads((ROOT / 'build/display-analysis/mobile-graphics-legacy-abi.json').read_text())
pure = audit['abstract_slots']
methods = audit['methods']
header = '''// SPDX-License-Identifier: BSD-2-Clause-Patent
// Opaque ABI storage for the exact original class, not source-level access
// to its private fields. The actual original metadata supplies its full
// Framebuffer/Service/IOService ancestry. Only qualified base prototypes may
// be called from source; opaque entries are original dispatch targets only.
#pragma once
#include <IOKit/IOService.h>
#include <libkern/libkern.h>
struct OriginalLegacySwapRequest;
class IOMobileFramebufferLegacy : public IOService {
 OSDeclareAbstractStructors(IOMobileFramebufferLegacy)
 unsigned char originalPrivate[0x18a0-sizeof(IOService)];
public:
 void installBootDimensions(uint32_t width,uint32_t height) {
  *reinterpret_cast<uint32_t *>(reinterpret_cast<unsigned char *>(this)+5808)=width;
  *reinterpret_cast<uint32_t *>(reinterpret_cast<unsigned char *>(this)+5812)=height;
 }
 const OriginalLegacySwapRequest *pendingSwapRequest() const {
  const OriginalLegacySwapRequest *request=nullptr;
  memcpy(&request,reinterpret_cast<const unsigned char *>(this)+5480,sizeof(request));return request;
 }
 bool activateOnlyPendingSwap(const OriginalLegacySwapRequest *request) {
  auto self=reinterpret_cast<unsigned char *>(this);
  auto bytes=reinterpret_cast<const unsigned char *>(request);
  const void *current=nullptr,*head=nullptr,*tail=nullptr,*next=nullptr,*previous=nullptr;
  memcpy(&current,self+5472,8);memcpy(&head,self+5480,8);memcpy(&tail,self+5488,8);
  if(!request || current || head!=request)return false;
  memcpy(&next,bytes+2960,8);memcpy(&previous,bytes+2968,8);
  if(previous!=self+5480)return false;
  if(next) {
   const void *link=self+5480;
   memcpy(const_cast<unsigned char *>(reinterpret_cast<const unsigned char *>(next))+2968,&link,8);
  } else if(tail!=bytes+2960)return false;
  memcpy(self+5472,&request,8);
  if(next) {memcpy(self+5480,&next,8);}
  else {const void *empty=nullptr,*emptyTail=self+5480;memcpy(self+5480,&empty,8);memcpy(self+5488,&emptyTail,8);}
  return true;
 }
 void free() override;
 bool init(OSDictionary *properties=nullptr) override;
 IOReturn setProperties(OSObject *) override;
 void systemWillShutdown(IOOptionBits) override;
 IOService *probe(IOService *,SInt32 *) override;
 bool start(IOService *) override;
 void stop(IOService *) override;
 IOWorkLoop *getWorkLoop() const override;
 IOReturn newUserClient(task_t,void *,UInt32,IOUserClient **) override;
 IOReturn setPowerState(unsigned long,IOService *) override;
'''
for i in range(168, 358):
    if i == 184:
        header += ' virtual bool isMainDisplay();\n'
    elif i == 188:
        header += ' virtual IOReturn getDotPitch(uint32_t *);\n'
    elif i == 213:
        header += ' virtual IOReturn supportedFrameInfo(uint32_t,uint32_t *,char *,uint32_t *)=0;\n'
    elif i == 249:
        header += ' virtual void getDisplayDimensions(uint32_t *,uint32_t *);\n'
    elif i == 290:
        header += ' virtual void kickSwapQueue();\n'
    elif i == 296:
        header += ' virtual bool validateSwap(const OriginalLegacySwapRequest *);\n'
    elif i == 295:
        header += ' virtual void completeCurrentSwap();\n'
    elif i == 337:
        header += ' virtual IOReturn collectCompletionReports(uint32_t,void *,uint32_t *,uint32_t)=0;\n'
    elif i == 288:
        header += ' virtual bool startHardware();\n'
    elif i == 301:
        header += ' virtual bool getRefreshAttributes(uint64_t *,bool);\n'
    elif i in pure:
        header += f' virtual __attribute__((noreturn)) void opaque{i}()=0;\n'
    else:
        header += f' virtual __attribute__((deprecated("Unclassified ABI: never call from source"))) void opaque{i}();\n'
header += '};\nstatic_assert(sizeof(IOMobileFramebufferLegacy)==0x18a0,"Actual original instance extent");\n'
(OUT / 'OriginalLegacyFramebufferABI.h').write_text(header)
print(run(['python3', ROOT / 'scripts/build-arm-driver.py', 'NativeLegacyDisplay'], text=True), end='')
obj = OUT / 'NativeLegacyDisplay.o'
raw = obj.read_bytes()
image = macho(raw)
st = next(p for c, p, _ in image['commands'] if c == 2)
sy, count, strings, _ = struct.unpack_from('<4I', raw, st + 8)
symbols = []
for i in range(count):
    ix, kind, section, _, va = struct.unpack_from('<IBBHQ', raw, sy + i * 16)
    symbols.append((raw[strings + ix:raw.index(b'\0', strings + ix)].decode(), va, kind))
vtable = next(va for name, va, _ in symbols if name == '__ZTV19NativeLegacyDisplay')
relocations = {}
for segment in image['segments']:
    for sec in segment['sections']:
        if sec['va'] <= vtable < sec['va'] + sec['size']:
            ro, rc = struct.unpack_from('<2I', raw, sec['command_offset'] + 56)
            for i in range(rc):
                offset, bits = struct.unpack_from('<2I', raw, ro + i * 8)
                if bits >> 27 & 1 and bits >> 28 == 0 and bits >> 25 & 3 == 3:
                    relocations[sec['va'] + offset] = symbols[bits & 0xffffff][0]
compiled = [relocations.get(vtable + 16 + i * 8) for i in range(358)]
require(all(compiled) and vtable + 16 + 358 * 8 not in relocations, 'Exact owned 358-entry vtable')
candidate = ROOT / 'build/xnu-prelink/native-owner-mobile-graphics-candidate.macho'
require(candidate.exists(), 'Missing the frozen display reference candidate')
old = candidate.read_bytes()
old_image = macho(old)
names = {n: v for v, group in exports(old, old_image).items() for n in group}


def read(va, n):
    at = backed_offset(old_image['segments'], va, n, len(old))
    return old[at:at + n]


bindings = {'__ZN25IOMobileFramebufferLegacyC2EPK11OSMetaClass': 0xfffffff0057865cc,
            '__ZN25IOMobileFramebufferLegacyD2Ev': 0xfffffff005786618,
            '__ZN25IOMobileFramebufferLegacy10gMetaClassE': 0xfffffff006b35940,
            '__ZTV25IOMobileFramebufferLegacy': int(audit['legacy_vptr'], 16) - 16}
owned = []
native = []
for i, name in enumerate(compiled):
    expected = int(methods[i]['address'], 16)
    if name.startswith(('__ZN19NativeLegacyDisplay', '__ZNK19NativeLegacyDisplay')):
        owned.append(i)
        require(i in (0, 1, 7, 14, 84, 86, 87, 141, 155, 184, 188, 288, 290, 296, 301) or i in pure,
                'Only qualified own methods and guarded leaves override')
    elif '25IOMobileFramebufferLegacy' in name:
        require(i not in pure, 'No unresolved original abstract backend target')
        bindings[name] = expected
    else:
        require(i < 168 and names.get(name) == expected, 'Actual inherited native base symbol ' + str(i))
        native.append(i)
require(owned == sorted([0, 1, 7, 14, 84, 86, 87, 141, 155, 184, 188, 288, 290, 296, 301] + pure),
        'Exact owned method set')
undefined = {n for n, _, k in symbols if k == 1 and 'IOMobileFramebufferLegacy' in n}
require(undefined == set(bindings), 'All and only actual original-class imports qualified')
for name, address, field, wide in [
        ('descriptor', 0xfffffff004b9ead0, 48, True), ('width', 0xfffffff004b9ebbc, 88, True),
        ('height', 0xfffffff004b9ebc4, 96, True), ('stride', 0xfffffff004b9ebd4, 104, True),
        ('allocation', 0xfffffff004b9ebfc, 144, True), ('format', 0xfffffff004b9ebac, 128, False),
        ('planes', 0xfffffff004b9ebf4, 152, False)]:
    expected = (0xf9400000 if wide else 0xb9400000) | ((field // (8 if wide else 4)) << 10)
    require(read(address, 8) == struct.pack('<2I', expected, 0xd65f03c0), 'Exact original surface getter ' + name)
    bindings['_native_legacy_surface_' + name] = address


def branch(pc):
    word = struct.unpack('<I', read(pc, 4))[0]
    require(word & 0xfc000000 == 0x94000000, 'Actual protected base call')
    value = word & 0x3ffffff
    return pc + 4 * (value - (1 << 26) if value & (1 << 25) else value)


require(branch(0xfffffff0057865fc) == 0xfffffff00577b510 and
        branch(0xfffffff005786630) == 0xfffffff00577b55c,
        'Original protected Legacy C2/D2 really call their original Service bases')
require(read(0xfffffff005787ef8, 16) == bytes.fromhex('080040f9088144f900013fd6e0000037'),
        'Original bool start_hardware() call and result check')
getter = int(methods[249]['address'], 16)
for offset, field in [(0x30, 5808), (0x58, 5812)]:
    require(struct.unpack('<I', read(getter + offset, 4))[0] == 0xb9400000 | ((field // 4) << 10) | (8 << 5) | 8,
            'Actual original width/height getter fields')
require(int(methods[295]['address'], 16) == 0xfffffff005795b88 and
        read(0xfffffff005795b98, 16) == bytes.fromhex('e00740f9280080520101001272d3ff97'),
        'Actual original slot-295 passes true to the normal current completion body')
require(read(0xfffffff00578aa7c, 24) == bytes.fromhex('080040f9084545f9030080d2e20303aa0400805200013fd6'),
        'Original completion calls optional report slot 337 with null outputs and zero limit')
require(read(0xfffffff005777e9c, 16) == bytes.fromhex('080040f908e142f900013fd6c0000036'),
        'Original isMainDisplay dispatch uses only self and tests boolean bit zero')
asm = OUT / 'original-bindings.S'
asm.write_text(''.join(f'.globl {name}\n.set {name}, {va:#x}\n' for name, va in sorted(bindings.items())))
recipe = ROOT / 'build/xnu/ios-obj/DEVELOPMENT_ARM64_BCM2837/iokit/DEVELOPMENT/IOPlatformExpert.cpo.json'
record = json.loads(recipe.read_text().strip().rstrip(','))
clang = record['arguments'][0]
run([clang, '-target', 'arm64-apple-ios17.0', '-c', str(asm), '-o', str(OUT / 'original-bindings.o')])
llvm_ar = '/usr/bin/llvm-ar'


def late_stage(src, dst, target):
    data = bytearray(Path(src).read_bytes())
    magic, cputype, cpusubtype, filetype, ncmds, sizeofcmds, flags = struct.unpack_from('<IiiIIII', data, 0)
    assert magic == 0xfeedfacf
    off = 32
    for _ in range(ncmds):
        cmd, size = struct.unpack_from('<II', data, off)
        if cmd == 0x19:
            nsects = struct.unpack_from('<I', data, off + 64)[0]
            for i in range(nsects):
                s = off + 72 + i * 80
                name = bytes(data[s:s + 16]).split(b'\0')[0]
                if name == b'__mod_init_func':
                    data[s:s + 16] = b'__tb365fc_noop'.ljust(16, b'\0')
        elif cmd == 0x2:
            symoff, nsyms, stroff, strsize = struct.unpack_from('<4I', data, off + 8)
            for i in range(nsyms):
                n = symoff + i * 16
                strx = struct.unpack_from('<I', data, n)[0]
                if strx == 0:
                    continue
                end = data.find(b'\0', stroff + strx)
                if bytes(data[stroff + strx:end]) == target.encode():
                    data[n + 4] |= 1
        off += size
    Path(dst).write_bytes(bytes(data))
late = OUT / 'NativeLegacyDisplay.late.o'
late_stage(obj, late, '__GLOBAL__sub_I_NativeLegacyDisplay.cpp')


def code(data, info):
    sec = next(s for g in info['segments'] for s in g['sections'] if s['name'] == '__text')
    return data[sec['fileoff']:sec['fileoff'] + sec['size']]


require(code(raw, image) == code(late.read_bytes(), macho(late.read_bytes())),
        'Only owned initializer section registration changes')
probe_source = ROOT / 'kits/tb365fc/LegacyDisplayProbe.cpp'
require(probe_source.exists(), 'Missing kits/tb365fc/LegacyDisplayProbe.cpp')
cmd = []
args = record['arguments']
i = 0
while i < len(args):
    value = args[i]
    if value == record['file']:
        cmd.append(str(probe_source))
    elif value == '-o':
        cmd += ['-o', str(OUT / 'LegacyDisplayProbe.o')]
        i += 1
    elif value == '-Xclang' and args[i + 1].startswith('-W'):
        i += 1
    elif not value.startswith('-W'):
        cmd.append(value)
    i += 1
cmd += ['-Wall', '-Wextra', '-Wno-unused-parameter', '-Wno-missing-designated-field-initializers', '-Wno-unnecessary-virtual-specifier',
        '-Wno-c99-designator', '-Wno-reorder-init-list', '-Wno-deprecated-enum-enum-conversion',
        '-I', str(ROOT / '.cache/xnu-work/osfmk')]
run(cmd, cwd=record['directory'])
stub = OUT / 'tb365fc-display-stubs.c'
stub.write_text('''/* Providers that the QEMU board links and TB365FC does not have: the
   QEMU display probes, a panel vsync source, the MTK DPU scanout hooks, and
   the BootProgress framebuffer claim. They are real definitions so the kernel
   links; an unclaimed framebuffer and DPU state zero keep NativeLegacyDisplay
   on the CPU/GOP present path. */
void native_keystore_probe(void) {}
void native_hid_input_probe(void) {}
void native_virtio_input_probe(void) {}
void native_shared_window_probe(void) {}
void native_qemu_metal_publish(void) {}
void native_legacy_vsync_observed(unsigned event, unsigned hz) { (void)event; (void)hz; }
void native_legacy_vsync_notify(void *display, unsigned long long timestamp,
                                unsigned long long continuous, unsigned long long interval)
{ (void)display; (void)timestamp; (void)continuous; (void)interval; }
_Bool native_legacy_set_refresh_properties(void *display) { (void)display; return 0; }
unsigned native_ufs_dpu_map(unsigned slot, const unsigned long long *pages, unsigned count)
{ (void)slot; (void)pages; (void)count; return 0; }
volatile unsigned native_ufs_dpu_state = 0;
int boot_progress_prepare_scanout(void) { return 1; }
void boot_progress_cancel_scanout(void) {}
void boot_progress_stage(const char *message) { (void)message; }
int boot_progress_pulse_anchor(unsigned long anchor, unsigned long long length,
                               unsigned width, unsigned height, unsigned stride)
{ (void)anchor; (void)length; (void)width; (void)height; (void)stride; return 0; }
''')
run([clang, '-target', 'arm64-apple-ios17.0', '-c', '-O2', '-ffreestanding', str(stub),
     '-o', str(OUT / 'tb365fc-display-stubs.o')])
print(run(['python3', ROOT / 'scripts/build-arm-driver.py', 'NativeGOPPresenter'], text=True), end='')
gop = ROOT / 'build/native-gop-presenter'
gop_late = gop / 'NativeGOPPresenter.late.o'
late_stage(gop / 'NativeGOPPresenter.o', gop_late, '__GLOBAL__sub_I_NativeGOPPresenter.cpp')
require(code((gop / 'NativeGOPPresenter.o').read_bytes(), macho((gop / 'NativeGOPPresenter.o').read_bytes())) ==
        code(gop_late.read_bytes(), macho(gop_late.read_bytes())),
        'Owned presenter instructions unchanged by late init staging')
archive = OUT / 'libNativeLegacyDisplay.late.a'
archive.unlink(missing_ok=True)
run([llvm_ar, 'rcs', archive, late, OUT / 'original-bindings.o', OUT / 'LegacyDisplayProbe.o',
     OUT / 'tb365fc-display-stubs.o', gop_late])
report_sources = ['drivers/GopFramebuffer/NativeLegacyDisplay.cpp',
                  'drivers/GopFramebuffer/NativeGOPPresenter.cpp',
                  'kits/tb365fc/LegacyDisplayProbe.cpp']
(OUT / 'build-report.json').write_text(json.dumps(dict(
    source_sha256={name: sha(ROOT / name) for name in report_sources},
    generated_abi_sha256=sha(OUT / 'OriginalLegacyFramebufferABI.h'),
    original_candidate_sha256=sha(candidate),
    original_audit_sha256=sha(ROOT / 'build/display-analysis/mobile-graphics-legacy-abi.json'),
    compiled_object_sha256=sha(obj), late_object_sha256=sha(late), archive_sha256=sha(archive),
    bindings={n: hex(v) for n, v in bindings.items()}, virtual_entries=358, owned_entries=owned,
    inherited_native_entries=native, guarded_backend_leaves=[i for i in pure if i not in (213, 337)],
    keystore_probe='stub', hid_probe='stub', original_instructions_modified=False,
    driver_initialized=False, display_presented=False), indent=2) + '\n')
print('Built minimal TB365FC legacy display archive; key/HID probes stubbed')
