/*
 * QEMU Windows Hypervisor Platform accelerator (WHPX)
 *
 * Copyright Microsoft Corp. 2017
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or later.
 * See the COPYING file in the top-level directory.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "system/kvm_int.h"
#include "qemu/main-loop.h"
#include "system/accel-ops.h"
#include "system/cpus.h"

#include "system/whpx.h"
#include "whpx-internal.h"
#include "whpx-accel-ops.h"

static void whpx_start_vcpu_thread(CPUState *cpu)
{
    char thread_name[VCPU_THREAD_NAME_SIZE];

    snprintf(thread_name, VCPU_THREAD_NAME_SIZE, "CPU %d/WHPX",
             cpu->cpu_index);
    qemu_thread_create(cpu->thread, thread_name, whpx_cpu_thread_fn,
                       cpu, QEMU_THREAD_JOINABLE);
}

static void whpx_kick_vcpu_thread(CPUState *cpu)
{
    if (!qemu_cpu_is_self(cpu)) {
        whpx_vcpu_kick(cpu);
    }
}

static bool whpx_vcpu_thread_is_idle(CPUState *cpu)
{
    /* The i386 version of this function returns true when the hypervisor
     * is not providing APIC emulation, and false when it is. On ARM, the WHP
     * interfaces require that the hypervisor provide interrupt controller
     * virtualization, so we always return false here.
     */
    return false;
}

static void whpx_accel_ops_class_init(ObjectClass *oc, const void *data)
{
    AccelOpsClass *ops = ACCEL_OPS_CLASS(oc);

    ops->create_vcpu_thread = whpx_start_vcpu_thread;
    ops->kick_vcpu_thread = whpx_kick_vcpu_thread;
    ops->cpu_thread_is_idle = whpx_vcpu_thread_is_idle;

    ops->synchronize_post_reset = whpx_cpu_synchronize_post_reset;
    ops->synchronize_post_init = whpx_cpu_synchronize_post_init;
    ops->synchronize_state = whpx_cpu_synchronize_state;
    ops->synchronize_pre_loadvm = whpx_cpu_synchronize_pre_loadvm;
    ops->synchronize_pre_resume = whpx_cpu_synchronize_pre_resume;
}

static const TypeInfo whpx_accel_ops_type = {
    .name = ACCEL_OPS_NAME("whpx"),

    .parent = TYPE_ACCEL_OPS,
    .class_init = whpx_accel_ops_class_init,
    .abstract = true,
};

static void whpx_accel_ops_register_types(void)
{
    type_register_static(&whpx_accel_ops_type);
}
type_init(whpx_accel_ops_register_types);
