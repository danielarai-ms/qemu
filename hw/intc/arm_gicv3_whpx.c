/*
 * QEMU Windows Hypervisor Platform accelerator (WHPX)
 *
 * Copyright Microsoft Corp. 2025
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or later.
 * See the COPYING file in the top-level directory.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qapi/error.h"
#include "hw/intc/arm_gicv3_common.h"
#include "target/arm/cpu.h"
#include "qom/object.h"
#include "migration/blocker.h"
#include "qemu/module.h"
#include "target/arm/cpregs.h"
#include "target/arm/whpx-internal.h"

/* TODO: Fix up debug logging */
#if 0

#ifdef DEBUG_GICV3_WHPX
#define DPRINTF(fmt, ...) \
    do { fprintf(stderr, "whpx_gicv3: " fmt, ## __VA_ARGS__); } while (0)
#else
#define DPRINTF(fmt, ...) \
    do { } while (0)
#endif

#endif // if 0

static bool arm_gicv3_whpx_debug;

#define DPRINTF(fmt, ...) \
    do { if (arm_gicv3_whpx_debug) { \
        fprintf(stderr, "whpx_gicv3: " fmt, ## __VA_ARGS__); \
    } } while (0)

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

static void whpx_arm_gicv3_set_irq(void *opaque, int irq, int level)
{
    GICv3State *s = (GICv3State *) opaque;
    uint32_t num_external_irq = s->num_irq;
    /* Meaning of the 'irq' parameter:
     *  [0..N-1] : SPI (device) interrupts
     *  [N..N+31] : SGI/PPI (internal) interrupts for CPU 0
     *  [N+32..N+63] : SGI/PPI (internal) interrupts for CPU 1
     *  ...
     * Convert this to WHP's desired encoding. Architecturally, SGIs are
     * numbered 0-15 and PPIs are numbered 16-31. The SGIs and PPIs are banked
     * per CPU. SPIs are numbered 32-1019 and additionally 4096-5119 (if
     * supported by the GIC). WHP wants different encodings for SPIs and
     * SGIs/PPIs.
     */
    uint32_t vector;
    uint32_t destination;

    /* XXX logging */
    /* Unclear which of these numbers is correct */
    if (irq == 46 || irq == 36) {
        printf("XXX whpx_arm_gicv3_set_irq %d: %d\n", irq, level);
    }

    assert(num_external_irq > GIC_INTERNAL);

    if (irq < (num_external_irq - GIC_INTERNAL)) {
        /* SPI. The architectural interrupt number (32-1019) goes in
         * vector, and destination must be 0. IRQ routing will be performed
         * according to VM configuration (including GIC state).
         */
        uint32_t arch_irq = irq + GIC_INTERNAL;
        DPRINTF("whpx_arm_gicv3_set_irq %d level %d\n", arch_irq, level);
        vector = arch_irq;
        destination = 0;
    } else {
        int banked_irq = irq - (num_external_irq - GIC_INTERNAL);
        int cpu = irq / GIC_INTERNAL;

        printf("whpx_arm_gicv3_set_irq unsupported irq %d for cpu %d level %d\n",
               banked_irq, cpu, level);
        /* TODO: values for vector and destination are TBD */
        g_assert_not_reached();
    }
    whpx_arm_set_irq(vector, destination, level);
}

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

static void arm_gicv3_icc_reset(CPUARMState *env, const ARMCPRegInfo *ri)
{
    /* TODO: Maybe we don't need to register this function if it doesn't
     * need to do anything?
     */
    DPRINTF("GICV3 ICC reset (NOP)\n");
}

/*
 * CPU interface registers of GIC needs to be reset on CPU reset.
 * For the calling arm_gicv3_icc_reset() on CPU reset, we register
 * below ARMCPRegInfo. As we reset the whole cpu interface under single
 * register reset, we define only one register of CPU interface instead
 * of defining all the registers.
 */
static const ARMCPRegInfo gicv3_cpuif_reginfo[] = {
    { .name = "ICC_CTLR_EL1", .state = ARM_CP_STATE_BOTH,
      .opc0 = 3, .opc1 = 0, .crn = 12, .crm = 12, .opc2 = 4,
      /*
       * If ARM_CP_NOP is used, resetfn is not called,
       * So ARM_CP_NO_RAW is appropriate type.
       */
      .type = ARM_CP_NO_RAW,
      .access = PL1_RW,
      .readfn = arm_cp_read_zero,
      .writefn = arm_cp_write_ignore,
      /*
       * We hang the whole cpu interface reset routine off here
       * rather than parcelling it out into one little function
       * per register
       */
      .resetfn = arm_gicv3_icc_reset,
    },
};

static void whpx_arm_gicv3_realize(DeviceState *dev, Error **errp)
{
    GICv3State *s = WHPX_ARM_GICV3(dev);
    WHPXARMGICv3Class *wgc = WHPX_ARM_GICV3_GET_CLASS(s);
    Error *local_err = NULL;
    int i;

    DPRINTF("whpx_arm_gicv3_realize\n");

    wgc->parent_realize(dev, &local_err);
    if (local_err) {
        error_propagate(errp, local_err);
        return;
    }

    if (s->revision != 3) {
        error_setg(errp, "unsupported GIC revision %d for WHP GIC",
                   s->revision);
    }

    if (s->security_extn) {
        error_setg(errp, "the WHP VGICv3 does not implement the "
                   "security extensions");
        return;
    }

    gicv3_init_irqs_and_mmio(s, whpx_arm_gicv3_set_irq, NULL);

    for (i = 0; i < s->num_cpu; i++) {
        ARMCPU *cpu = ARM_CPU(qemu_get_cpu(i));

        define_arm_cp_regs(cpu, gicv3_cpuif_reginfo);
    }

    /* TODO: Does WHP support multiple redistributor regions? */
    if (s->nb_redist_regions > 1) {
        error_setg(errp, "Multiple VGICv3 redistributor regions are not "
                   "supported by WHP");
        error_append_hint(errp, "A maximum of %d VCPUs can be used",
                          s->redist_region_count[0]);
        return;
    }

    /* TODO:redist region setup */
    /* TODO: GSI routing? */

    /* TODO: Migration? */
    error_setg(&s->migration_blocker, "WHPX does not support VGICv3 migration");
    if (migrate_add_blocker(&s->migration_blocker, errp) < 0) {
        return;
    }
    /* TODO: vm_change_state handler if VGIC_GRP_CTRL? */
}

static void whpx_arm_gicv3_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    ResettableClass *rc = RESETTABLE_CLASS(klass);
    ARMGICv3CommonClass *agcc = ARM_GICV3_COMMON_CLASS(klass);
    WHPXARMGICv3Class *wgc = WHPX_ARM_GICV3_CLASS(klass);

    /* TODO: Real logging support */
    if (getenv("WHPX_GIC_DEBUG") != NULL) {
        arm_gicv3_whpx_debug = true;
    }

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
