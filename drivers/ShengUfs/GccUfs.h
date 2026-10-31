// SPDX-License-Identifier: BSD-2-Clause-Patent
// MT6835 UFS host ranges used by NativeUfs. The filename is inherited from the
// reference driver; the Qualcomm GCC window does not exist on this SoC. LK hands
// the controller over enabled but with the UniPro link parked by its power-off
// notify, so the driver reads these syscon status registers to tell what the
// firmware left on; no clock or reset register is written here.
#ifndef NATIVE_UFS_GCC_UFS_H
#define NATIVE_UFS_GCC_UFS_H

// ufshci@112b0000 in the board DT (reg 0x112b0000, 0x2300); /proc/iomem agrees.
#define UFS_HC_BASE                   0x112b0000ULL
#define UFS_HC_SIZE                   0x2300ULL

// ufs_ao_cfg@112b8000 and ufs_pdn_cfg@112bb000 (same nodes the LK DT carries):
// gate set 0x08 / clr 0x0c / status 0x04; reset assert 0x48 / deassert 0x4c.
#define UFS_AO_BASE                   0x112b8000ULL
#define UFS_PDN_BASE                  0x112bb000ULL

#endif
