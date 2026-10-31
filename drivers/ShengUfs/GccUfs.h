// SPDX-License-Identifier: BSD-2-Clause-Patent
// MT6835 UFS host ranges used by NativeUfs. The filename is inherited from the
// reference driver; the Qualcomm GCC window does not exist on this SoC. LK hands
// the controller over enabled but with the UniPro link parked and the device
// rails cut by its power-off notify, so the driver runs the MTK UEFI power-up
// (SMC rails, PDN/AO reset release, M-PHY unisolation, REFCLK, XOUFS) before
// the link startup.
#ifndef NATIVE_UFS_GCC_UFS_H
#define NATIVE_UFS_GCC_UFS_H

// ufshci@112b0000 in the board DT (reg 0x112b0000, 0x2300); /proc/iomem agrees.
#define UFS_HC_BASE                   0x112b0000ULL
#define UFS_HC_SIZE                   0x2300ULL

// ufs_ao_cfg@112b8000 and ufs_pdn_cfg@112bb000 (same nodes the LK DT carries):
// gate set 0x08 / clr 0x0c / status 0x04; reset assert 0x48 / deassert 0x4c.
#define UFS_AO_BASE                   0x112b8000ULL
#define UFS_PDN_BASE                  0x112bb000ULL

// ufsphy@112a0000 (mt8183-ufsphy per the MTK UEFI driver header); register
// 0xa0ac/0xb044 sit above the first page, so map the whole block.
#define UFS_PHY_BASE                  0x112a0000ULL
#define UFS_PHY_SIZE                  0x10000ULL

#endif
