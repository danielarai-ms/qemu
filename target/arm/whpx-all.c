/*
 * QEMU Windows Hypervisor Platform accelerator (WHPX)
 *
 * Copyright Microsoft Corp. 2025
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or later.
 * See the COPYING file in the top-level directory.
 *
 */

#include "qemu/osdep.h"
#include "qemu/accel.h"

static bool whpx_allowed;

static int whpx_accel_init(MachineState *ms)
{
    return -ENOSYS;
}

static void whpx_accel_class_init(ObjectClass *oc, const void *data)
{
    AccelClass *ac = ACCEL_CLASS(oc);
    ac->name = "WHPX";
    ac->init_machine = whpx_accel_init;
    ac->allowed = &whpx_allowed;
}

static void whpx_accel_instance_init(Object *obj)
{
    /* TODO: Any necessary global initialization should go here. */
}

static const TypeInfo whpx_accel_type = {
    .name = ACCEL_CLASS_NAME("whpx"),
    .parent = TYPE_ACCEL,
    .instance_init = whpx_accel_instance_init,
    .class_init = whpx_accel_class_init,
};

static void whpx_type_init(void)
{
    type_register_static(&whpx_accel_type);
}

type_init(whpx_type_init);
