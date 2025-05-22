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
#include "hw/intc/arm_gicv3_common.h"
#include "qom/object.h"
#include "qemu/module.h"

#ifdef DEBUG_GICV3_WHPX
#define DPRINTF(fmt, ...) \
    do { fprintf(stderr, "kvm_gicv3: " fmt, ## __VA_ARGS__); } while (0)
#else
#define DPRINTF(fmt, ...) \
    do { } while (0)
#endif

#define TYPE_WHPX_ARM_GICV3 "whpx-arm-gicv3"
typedef struct WHPXARMGICv3Class WHPXARMGICv3Class;
/* This is reusing the GICv3State typedef from ARM_GICV3_ITS_COMMON */
DECLARE_OBJ_CHECKERS(GICv3State, WHPXARMGICv3Class,
                     WHPX_ARM_GICV3, TYPE_WHPX_ARM_GICV3)

struct WHPXARMGICv3Class {
    ARMGICv3CommonClass parent_class;
    DeviceRealize parent_realize;
    ResettablePhases parent_phases;
    /* TODO: Do we need additional whpx-specific fields? */
};

static void whpx_arm_gicv3_get(GICv3State *s)
{
    /* TODO: Implement this function */
    g_assert_not_reached();
}

static void whpx_arm_gicv3_put(GICv3State *s)
{
    /* TODO: Implement this function */
    g_assert_not_reached();
}

static void whpx_arm_gicv3_reset_hold(Object *obj, ResetType type)
{
    GICv3State *s = ARM_GICV3_COMMON(obj);
    WHPXARMGICv3Class *wgc = WHPX_ARM_GICV3_GET_CLASS(s);

    DPRINTF("Reset\n");

    if (wgc->parent_phases.hold) {
        wgc->parent_phases.hold(obj, type);
    }

    if (s->migration_blocker) {
        DPRINTF("Cannot put kernel gic state, no kernel interface\n");
        return;
    }

    whpx_arm_gicv3_put(s);
}

static void whpx_arm_gicv3_realize(DeviceState *dev, Error **errp)
{
    /* TODO: Implement this function */
    g_assert_not_reached();
}

static void whpx_arm_gicv3_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    ResettableClass *rc = RESETTABLE_CLASS(klass);
    ARMGICv3CommonClass *agcc = ARM_GICV3_COMMON_CLASS(klass);
    WHPXARMGICv3Class *wgc = WHPX_ARM_GICV3_CLASS(klass);

    agcc->pre_save = whpx_arm_gicv3_get;
    agcc->post_load = whpx_arm_gicv3_put;
    device_class_set_parent_realize(dc, whpx_arm_gicv3_realize,
                                    &wgc->parent_realize);
    resettable_class_set_parent_phases(rc, NULL, whpx_arm_gicv3_reset_hold,
                                       NULL, &wgc->parent_phases);
}

static const TypeInfo whpx_arm_gicv3_info = {
    .name = TYPE_WHPX_ARM_GICV3,
    .parent = TYPE_ARM_GICV3_COMMON,
    .instance_size = sizeof(GICv3State),
    .class_init = whpx_arm_gicv3_class_init,
    .class_size = sizeof(WHPXARMGICv3Class),
};

static void whpx_arm_gicv3_register_types(void)
{
    type_register_static(&whpx_arm_gicv3_info);
}

type_init(whpx_arm_gicv3_register_types)
