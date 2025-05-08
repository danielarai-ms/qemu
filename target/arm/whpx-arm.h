/*
 * QEMU Windows Hypervisor Platform accelerator (WHPX) support
 *
 * Copyright Microsoft, Corp. 2025
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or later.
 * See the COPYING file in the top-level directory.
 *
 */


#ifndef QEMU_WHPX_ARM_H
#define QEMU_WHPX_ARM_H

#include "cpu.h"

/**
 * whpx_arm_set_cpu_features_from_host:
 * @cpu: ARMCPU to set the features for
 *
 * Set up the ARMCPU struct fields up to match the information probed
 * from the host CPU.
 */
void whpx_arm_set_cpu_features_from_host(ARMCPU *cpu);


#endif
