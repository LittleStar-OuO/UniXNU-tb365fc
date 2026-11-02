// SPDX-License-Identifier: BSD-2-Clause-Patent
#pragma once
// Shared production parser for the owner-signed root policy region (A04RPOL1), the board layout of
// UniXNU-a04br3. The owner-signed VerifiedRoot policies of the root windows live in userdata, after
// the SHNGROOT windows:
//
//   Location: the end of the window that ends farthest into userdata, rounded up to 1 MiB. See
//             rootPolicyRegionOffset(). The userdata image ends there (scripts/make-tb365fc-root-policy.py).
//
//   Layout, little endian, offsets from the start of the region:
//     0      char[8]  magic "A04RPOL1"
//     8      u32      version = 1
//     12     u32      count of entries, 1..16
//     16     u64      total_bytes: the whole region, this 4096-byte header included
//     24     count x { u64 offset (from the start of the region), u64 bytes }   (16 bytes each)
//     ...    unused up to 4064, ignored (write zero)
//     4064   SHA-256 of the 4096-byte header computed with these 32 bytes taken as zero
//
//   Each entry is one VROOT01 envelope exactly as kits/VerifiedRoot/tools/manifest.py writes it.
//   The header hash only catches a torn, stale or misplaced region. Trust comes from vr_open: the
//   owner signature over each envelope under the independently linked key. A window is served only
//   through the one entry whose signed image size equals the window size.
// Requires RootLayout.h (le32/le64).
constexpr uint64_t kRootPolicyAlign = 1024 * 1024;
constexpr unsigned kRootPolicyHeaderBytes = 4096;
constexpr unsigned kRootPolicyHashed = 4064;
constexpr unsigned kRootPolicyMaxEntries = 16;
constexpr uint64_t kRootPolicyMaxBytes = 256ULL * 1024 * 1024;
constexpr uint64_t kRootPolicyEnvelopeMinBytes = VR_HEADER_BYTES + VR_SIGNATURE_BYTES;

struct RootPolicyEntry { uint64_t offset, bytes; };
struct RootPolicyHeader {
  uint64_t totalBytes = 0;
  unsigned count = 0;
  RootPolicyEntry entry[kRootPolicyMaxEntries] = {};
};

enum RootPolicyStatus : unsigned {
  kRootPolicyOk = 0, kRootPolicyNoMagic = 1, kRootPolicyVersion = 2, kRootPolicyHash = 3,
  kRootPolicySize = 4, kRootPolicyEntryBounds = 5, kRootPolicyEntryOverlap = 6,
};

// Byte offset of the policy region from the start of userdata.
static inline uint64_t rootPolicyRegionOffset (const RootLayout *layout)
{
  uint64_t end = 0;
  for (unsigned i = 0; layout && i < layout->windows && i < 4; ++i) {
    uint64_t windowEnd = layout->window[i].offset + layout->window[i].size;
    if (windowEnd > end) end = windowEnd;
  }
  return (end + kRootPolicyAlign - 1) & ~(kRootPolicyAlign - 1);
}

// header: the first 4096 bytes of the region; digest: SHA-256 of them with bytes 4064..4095 taken
// as zero (computed by the caller); available: bytes of userdata from the start of the region to the
// end of the partition.
static inline unsigned parseRootPolicyHeader (const uint8_t *header, const uint8_t *digest,
                                              uint64_t available, RootPolicyHeader &policy)
{
  policy = {};
  if (memcmp (header, "A04RPOL1", 8)) return kRootPolicyNoMagic;
  uint32_t version = le32 (header + 8), count = le32 (header + 12);
  uint64_t total = le64 (header + 16);
  if (version != 1 || !count || count > kRootPolicyMaxEntries) return kRootPolicyVersion;
  if (memcmp (digest, header + kRootPolicyHashed, 32)) return kRootPolicyHash;
  // The shared transport reads whole 4096-byte blocks; the region writer pads to 4096.
  if (total < kRootPolicyHeaderBytes || (total & 4095) || total > kRootPolicyMaxBytes ||
      total > available) return kRootPolicySize;
  for (uint32_t index = 0; index < count; ++index) {
    uint64_t offset = le64 (header + 24 + 16 * index), bytes = le64 (header + 32 + 16 * index);
    if (offset < kRootPolicyHeaderBytes || (offset & 4095) || offset > total ||
        bytes < kRootPolicyEnvelopeMinBytes || bytes > total - offset) return kRootPolicyEntryBounds;
    for (uint32_t prior = 0; prior < index; ++prior) {
      const RootPolicyEntry &other = policy.entry[prior];
      if (offset < other.offset + other.bytes && other.offset < offset + bytes) return kRootPolicyEntryOverlap;
    }
    policy.entry[index] = { offset, bytes };
  }
  policy.totalBytes = total;
  policy.count = count;
  return kRootPolicyOk;
}

// The signed image size of a VROOT01 envelope (header byte 24). Unauthenticated: used only to
// choose the entry that vr_open then authenticates and whose authenticated size must equal the
// window again.
static inline uint64_t rootPolicyImageBytes (const uint8_t *envelope) { return le64 (envelope + 24); }

enum RootPolicySelect : unsigned { kRootPolicySelected = 0, kRootPolicyNoMatch = 1, kRootPolicyDuplicate = 2 };

static inline unsigned selectRootPolicy (const uint8_t *region, const RootPolicyHeader &policy,
                                         uint64_t windowBytes, unsigned &selected)
{
  unsigned matches = 0;
  for (unsigned index = 0; index < policy.count; ++index) {
    if (rootPolicyImageBytes (region + policy.entry[index].offset) != windowBytes) continue;
    selected = index;
    ++matches;
  }
  return matches == 1 ? kRootPolicySelected : matches ? kRootPolicyDuplicate : kRootPolicyNoMatch;
}
