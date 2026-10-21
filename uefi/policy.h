/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef RTX3080TI_EFI_POLICY_H
#define RTX3080TI_EFI_POLICY_H
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include "../kernel/gpu_unlock/target.h"
struct gu_options {
    unsigned int segment,bus,device,function,board_vendor,board_device;
    bool has_target,execute,ss1_full,windows;
    bool has_windows_partition;
    uint8_t windows_partition[16];
};
int gu_parse_options(const char *,size_t,struct gu_options *);
int gu_capability_offset(const uint8_t[256],unsigned int);
uint64_t gu_bar_resource_length(const uint8_t *,size_t);
int gu_partition_matches(const uint8_t *,size_t,const uint8_t[16]);
#endif
