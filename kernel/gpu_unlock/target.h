/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef RTX3080TI_TARGET_H
#define RTX3080TI_TARGET_H
#define RTX3080TI_PCI_ID 0x2208
#define RTX3080TI_SUBVENDOR_ID 0x10de
#define RTX3080TI_SUBDEVICE_ID 0x1535

/* This experiment is restricted to the exact RTX 3080 Ti board identity. */
static inline int rtx3080ti_board_matches(unsigned int vendor, unsigned int device,
                                         unsigned int subvendor, unsigned int subdevice)
{
    return vendor == 0x10de && device == RTX3080TI_PCI_ID &&
           subvendor == RTX3080TI_SUBVENDOR_ID &&
           subdevice == RTX3080TI_SUBDEVICE_ID;
}
#endif
