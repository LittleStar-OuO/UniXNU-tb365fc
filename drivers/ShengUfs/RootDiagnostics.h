// SPDX-License-Identifier: BSD-2-Clause-Patent
// Passive snapshot formatter; caller supplies integer types and snprintf.
// No hardware access or decision about trust, transport or publication here.
struct UfsRootDiagnostics {
  const char *stage = "not started";
  uint64_t dma = 0, list = 0, lba = 0, first = 0, last = 0;
  uint64_t windowBytes = 0, policyCount = 0, reads = 0, failures = 0;
  uint32_t cap = 0, version = 0, hcs = 0, hce = 0, doorbell = 0, interrupt = 0;
  uint32_t smr = 0, s2cr = 0, cbar = 0, sctlr = 0;
  uint32_t type = 0, opcode = 0, lun = 0, command = 0, ocs = 0xff, scsi = 0xffff;
  uint32_t response = 0, result = 0, power = 0, manifest = 0xffffffff;
  uint32_t magic0 = 0, magic1 = 0, windows = 0, prepared = 0, published = 0;
  uint32_t verify = 0xffffffff;
  uint32_t protocol = 0, residual = 0, sense = 0;
  uint64_t far = 0, contiguous = 0;
  uint32_t runStop = 0, ahit = 0, polls = 0, dbSeen = 0, isSeen = 0, isBefore = 0;
  uint32_t gfsrBefore = 0, gfsr = 0, gsyn = 0, fsrBefore = 0, fsr = 0, fsyn = 0;
  uint32_t smmuId2 = 0, cba2r = 0, uic[5] = {};
  uint32_t smmuId1 = 0, smmuSlot = 0xffffffff;
  uint32_t publicationPhase = 0, publicationWindow = 0xffffffff;
  uint32_t entryIs = 0, entryUic[5] = {}, entryHcs = 0, entryHce = 0;
  uint32_t preSet[3] = { 0xffffffff, 0xffffffff, 0xffffffff }, preSet3 = 0xffffffff, savePower = 0;
  uint32_t linkResult = 0xffffffff, linkAttempts = 0, linkHcs = 0, linkInterrupt = 0;
  uint32_t dmeReset = 0xffffffff, dmeEnable = 0xffffffff;
  uint32_t linkCodes[3] = { 0xffffffff, 0xffffffff, 0xffffffff }, linkRound = 0, linkOk = 0;
  uint32_t retryReset = 0xffffffff, retryHib = 0xffffffff, retryPowerOn = 0xffffffff, retryEnable = 0xffffffff;
  uint32_t wakeResult = 0xffffffff, wakeOcs = 0xff, wakeScsi = 0xffff, wakeAttempts = 0;
  uint32_t wakeFirstScsi = 0xffff, wakeFirstSense = 0xffffffff, wakeLastSense = 0xffffffff;
  uint32_t devInitResult = 0xffffffff, devInitAttempts = 0, devInitPolls = 0, devInitFlag = 0xffffffff;
  uint32_t gheadMagic0 = 0xffffffff, gheadMagic1 = 0xffffffff;
  uint32_t gheadCrc = 0xffffffff, gheadCrcCalc = 0xffffffff, gheadEntriesCount = 0xffffffff;
  uint64_t gheadEntriesLba = 0, gheadFirst = 0, gheadLast = 0, gheadBackup = 0;
  uint32_t dataPattern = 0xffffffff, dataAt = 0xffffffff, dataFirst = 0;
  uint32_t dataPrd[4] = { 0xffffffff, 0xffffffff, 0xffffffff, 0xffffffff };
  uint32_t dataUtrd6 = 0xffffffff, dataUtrd7 = 0xffffffff, dataIs = 0xffffffff;
  uint32_t dataShiftOk = 0xffffffff, dataShiftPattern = 0xffffffff, dataShiftAt = 0xffffffff, dataShiftFirst = 0;
  uint32_t dataLu0 = 0xffffffff, dataMbr = 0xffffffff, dataF2 = 0xffffffff, dataF2At = 0xffffffff;
  uint32_t xoufs = 0, refclk = 0, aoStatus = 0, aoReset = 0, pdnStatus = 0, pdnReset = 0;
  uint32_t smc[5] = { 0xffffffff, 0xffffffff, 0xffffffff, 0xffffffff, 0xffffffff };
  uint32_t phy8c = 0xffffffff, phyAck = 0xffffffff, xoufs2 = 0xffffffff;
  uint32_t aoStatus2 = 0xffffffff, pdnStatus2 = 0xffffffff;
  uint32_t resetHce = 0xffffffff, resetHcs = 0;
  uint32_t ipVer = 0xffffffff, gateAo = 0xffffffff, gatePdn = 0xffffffff;
  uint32_t hceClear = 0xffffffff, hceSet = 0xffffffff;
};

static inline int formatUfsRootDiagnostics(char *buffer, size_t capacity,
                                          const UfsRootDiagnostics &d, const char *reason,
                                          const char *title = "UFS ROOT FAILURE")
{
  return snprintf(buffer, capacity,
    "%s: %s\nstage=%s result=%08x\n"
    "HC cap=%08x ver=%08x hcs=%08x hce=%08x db=%08x is=%08x\n"
    "DMA=%llx UTRL=%llx SMMU smr=%08x s2cr=%08x cbar=%08x sctlr=%08x\n"
    "CMD n=%u type=%02x op=%02x lun=%02x ocs=%02x rsp=%08x scsi=%04x power=%02x\n"
    "PROTOCOL reject=%u residual=%u sense=%06x SMMU slot=%u id1=%x\n"
    "QUEUE rsr=%x ahit=%x polls=%u dbSeen=%x isSeen=%x isBefore=%x contiguous=%llx\n"
    "FAULT global=%x->%x syn=%x cb=%x->%x syn=%x far=%llx id2=%x cba2r=%x\n"
    "UIC pa=%08x dl=%08x net=%08x tr=%08x dme=%08x\n"
    "LINK entry is=%08x hcs=%08x hce=%x uic=%x,%x,%x,%x,%x\n"
    "LINK pre=%x,%x sp=%x set=%x ls=%x/%u hcs=%08x is=%08x\n"
    "LINK dme=%x/%x codes=%x,%x,%x round=%u retry=%x,%x,%x,%x ok=%u\n"
    "WAKE ssu=%08x/%02x/%04x att=%u first=%04x/%06x last=%06x\n"
    "INIT fdev=%08x att=%u polls=%u flag=%x\n"
    "GHEAD m=%08x %08x crc=%08x/%08x elba=%llx n=%u\n"
    "DATA cap=%08x dma=%llx pat=%u at=%x first=%08x\n"
    "DPATH u6=%08x u7=%08x prd=%08x,%08x,%08x,%08x is=%08x\n"
    "SHIFT ok=%x pat=%u at=%x first=%08x\n"
    "LUMAP lu0=%08x lu2=%08x f2=%08x@%x\n"
    "GRANGE f=%llx l=%llx b=%llx\n"
    "PWR ao=%08x/%08x pdn=%08x/%08x xo=%08x rc=%08x\n"
    "MTK smc=%x,%x,%x,%x,%x phy=%08x ack=%x xo=%08x ao=%08x pdn=%08x rst=%x/%08x\n"
    "MTK2 ip=%08x gates=%x/%x hce=%x/%x\n"
    "GPT/read lba=%llx userdata=%llx..%llx magic=%08x %08x manifest=%u\n"
    "WINDOWS found=%u prepared=%u published=%u bytes=%llx policies=%llu verify=%08x\n"
    "IO reads=%llu failures=%llu pubPhase=%u window=%u\n",
    title, reason, d.stage, d.result, d.cap, d.version, d.hcs, d.hce, d.doorbell, d.interrupt,
    (unsigned long long)d.dma, (unsigned long long)d.list, d.smr, d.s2cr, d.cbar, d.sctlr,
    d.command, d.type, d.opcode, d.lun, d.ocs, d.response, d.scsi, d.power,
    d.protocol, d.residual, d.sense, d.smmuSlot, d.smmuId1,
    d.runStop, d.ahit, d.polls, d.dbSeen, d.isSeen, d.isBefore, (unsigned long long)d.contiguous,
    d.gfsrBefore, d.gfsr, d.gsyn, d.fsrBefore, d.fsr, d.fsyn, (unsigned long long)d.far,
    d.smmuId2, d.cba2r, d.uic[0], d.uic[1], d.uic[2], d.uic[3], d.uic[4],
    d.entryIs, d.entryHcs, d.entryHce, d.entryUic[0], d.entryUic[1], d.entryUic[2], d.entryUic[3], d.entryUic[4],
    d.preSet[0], d.preSet[1], d.savePower, d.preSet3, d.linkResult, d.linkAttempts, d.linkHcs, d.linkInterrupt,
    d.dmeReset, d.dmeEnable, d.linkCodes[0], d.linkCodes[1], d.linkCodes[2], d.linkRound,
    d.retryReset, d.retryHib, d.retryPowerOn, d.retryEnable, d.linkOk,
    d.wakeResult, d.wakeOcs, d.wakeScsi, d.wakeAttempts, d.wakeFirstScsi, d.wakeFirstSense, d.wakeLastSense,
    d.devInitResult, d.devInitAttempts, d.devInitPolls, d.devInitFlag,
    d.gheadMagic0, d.gheadMagic1, d.gheadCrc, d.gheadCrcCalc,
    (unsigned long long)d.gheadEntriesLba, d.gheadEntriesCount,
    d.cap, (unsigned long long)d.dma, d.dataPattern, d.dataAt, d.dataFirst,
    d.dataUtrd6, d.dataUtrd7, d.dataPrd[0], d.dataPrd[1], d.dataPrd[2], d.dataPrd[3], d.dataIs,
    d.dataShiftOk, d.dataShiftPattern, d.dataShiftAt, d.dataShiftFirst,
    d.dataLu0, d.dataMbr, d.dataF2, d.dataF2At,
    (unsigned long long)d.gheadFirst, (unsigned long long)d.gheadLast, (unsigned long long)d.gheadBackup,
    d.aoStatus, d.aoReset, d.pdnStatus, d.pdnReset, d.xoufs, d.refclk,
    d.smc[0], d.smc[1], d.smc[2], d.smc[3], d.smc[4],
    d.phy8c, d.phyAck, d.xoufs2, d.aoStatus2, d.pdnStatus2, d.resetHce, d.resetHcs,
    d.ipVer, d.gateAo, d.gatePdn, d.hceClear, d.hceSet,
    (unsigned long long)d.lba, (unsigned long long)d.first, (unsigned long long)d.last,
    d.magic0, d.magic1, d.manifest, d.windows, d.prepared, d.published,
    (unsigned long long)d.windowBytes, (unsigned long long)d.policyCount, d.verify,
    (unsigned long long)d.reads, (unsigned long long)d.failures, d.publicationPhase, d.publicationWindow);
}
