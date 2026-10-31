// SPDX-License-Identifier: BSD-2-Clause-Patent
// MT6835 UFS host range used by NativeUfs. The filename is inherited from the
// reference driver; the Qualcomm GCC window does not exist on this SoC and no
// clock or power register is written: LK leaves the controller and link running.
#ifndef NATIVE_UFS_GCC_UFS_H
#define NATIVE_UFS_GCC_UFS_H

// ufshci@112b0000 in the board DT (reg 0x112b0000, 0x2300); /proc/iomem agrees.
#define UFS_HC_BASE                   0x112b0000ULL
#define UFS_HC_SIZE                   0x2300ULL

#endif
