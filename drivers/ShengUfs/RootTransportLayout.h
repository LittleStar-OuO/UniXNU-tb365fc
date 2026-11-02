// SPDX-License-Identifier: BSD-2-Clause-Patent
enum : uint32_t {
  kUcdOffset = 4096,
  kRspOffset = 512,
  kPrdtOffset = 1024,
  kDataOffset = 8192,
  kDataMax = 131072,
  kDmaBytes = kDataOffset + kDataMax,
  // diag93 read the preloader ("FILE_INFO") from LUN 0 LBA 1; Android's block map shows sda/sdb as
  // 4 MiB boot LUs and sdc (128 GiB, every by-name partition) as the GPT/userdata LU, the third one.
  kUserdataLun = 2,
};
