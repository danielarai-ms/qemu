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
#include "cpu.h"
#include "system/address-spaces.h"
#include "qemu/accel.h"
#include "system/whpx.h"
#include "system/runstate.h"
#include "qemu/error-report.h"
#include "qapi/error.h"
#include "hw/boards.h"
#include "whpx-arm.h"
#include "migration/blocker.h"
#include "syndrome.h"

#include "whpx-internal.h"
#include "whpx-accel-ops.h"

// XXX do not merge
#include <stdio.h>
#include <stdlib.h>

#define SAS_BYTE       0
#define SAS_HALFWORD   1
#define SAS_WORD       2
#define SAS_DOUBLEWORD 3

/*
 * The register layout roughly follows the layout of CPUARMState and
 * not necessarily the order of the WHP definitions.
 */
static const WHV_REGISTER_NAME whpx_register_names[] = {
    /* Aarch64 General purpose registers */
    WHvArm64RegisterX0,
    WHvArm64RegisterX1,
    WHvArm64RegisterX2,
    WHvArm64RegisterX3,
    WHvArm64RegisterX4,
    WHvArm64RegisterX5,
    WHvArm64RegisterX6,
    WHvArm64RegisterX7,
    WHvArm64RegisterX8,
    WHvArm64RegisterX9,
    WHvArm64RegisterX10,
    WHvArm64RegisterX11,
    WHvArm64RegisterX12,
    WHvArm64RegisterX13,
    WHvArm64RegisterX14,
    WHvArm64RegisterX15,
    WHvArm64RegisterX16,
    WHvArm64RegisterX17,
    WHvArm64RegisterX18,
    WHvArm64RegisterX19,
    WHvArm64RegisterX20,
    WHvArm64RegisterX21,
    WHvArm64RegisterX22,
    WHvArm64RegisterX23,
    WHvArm64RegisterX24,
    WHvArm64RegisterX25,
    WHvArm64RegisterX26,
    WHvArm64RegisterX27,
    WHvArm64RegisterX28,

    /* Aarch64 Frame pointer and link register (general purpose 29 and 30) */
    WHvArm64RegisterFp,
    WHvArm64RegisterLr,

    /* Aarc64 stack pointer (general purpose 31, sometimes) */
    WHvArm64RegisterSp,

    /* Aarch64 Program counter */
    WHvArm64RegisterPc,

    /* Aarch64 Process state register */
    WHvArm64RegisterPstate,

    /* Aarch64 saved program status registers */
    WHvArm64RegisterSpsrEl1,

    /* Aarch64 exception link register */
    WHvArm64RegisterElrEl1,

    /* Aarch64 banked stack pointers */
    WHvArm64RegisterSpEl0,
    WHvArm64RegisterSpEl1,

    /* MMU translation table base registers */
    WHvArm64RegisterTtbr0El1,
    WHvArm64RegisterTtbr1El1,

    /* Aarch64 floating point registers */
    WHvArm64RegisterQ0,
    WHvArm64RegisterQ1,
    WHvArm64RegisterQ2,
    WHvArm64RegisterQ3,
    WHvArm64RegisterQ4,
    WHvArm64RegisterQ5,
    WHvArm64RegisterQ6,
    WHvArm64RegisterQ7,
    WHvArm64RegisterQ8,
    WHvArm64RegisterQ9,
    WHvArm64RegisterQ10,
    WHvArm64RegisterQ11,
    WHvArm64RegisterQ12,
    WHvArm64RegisterQ13,
    WHvArm64RegisterQ14,
    WHvArm64RegisterQ15,
    WHvArm64RegisterQ16,
    WHvArm64RegisterQ17,
    WHvArm64RegisterQ18,
    WHvArm64RegisterQ19,
    WHvArm64RegisterQ20,
    WHvArm64RegisterQ21,
    WHvArm64RegisterQ22,
    WHvArm64RegisterQ23,
    WHvArm64RegisterQ24,
    WHvArm64RegisterQ25,
    WHvArm64RegisterQ26,
    WHvArm64RegisterQ27,
    WHvArm64RegisterQ28,
    WHvArm64RegisterQ29,
    WHvArm64RegisterQ30,
    WHvArm64RegisterQ31,
    WHvArm64RegisterFpsr,
    WHvArm64RegisterFpcr,

    /* TODO: Other required registers? */
};

struct whpx_register_set {
    WHV_REGISTER_VALUE values[RTL_NUMBER_OF(whpx_register_names)];
};

/* Partially copied from i386 */
struct AccelCPUState {
    /*
    bool window_registered;
    bool interruptable;
    bool ready_for_pic_interrupt;
    uint64_t tpr;
    uint64_t apic_base;
    bool interruption_pending;
    */
    bool dirty;

    /* Must be the last field as it may have a tail */
    WHV_RUN_VP_EXIT_CONTEXT exit_ctx;
};

struct AarchSyndromeGeneric {
    union {
        struct {
            uint32_t res: 25;
            uint32_t il: 1;
            uint32_t ec: 6;
        };
        uint32_t as_uint32;
    };
};

/* Aarch64 data abort syndrome structure. */
struct AarchSyndromeDataAbort {
    union {
        struct {
            /* The format of the least significant bits depends on the
             * exception class. This layout is only valid for data aborts
             */
            uint32_t dfsc: 6;
            uint32_t wnr: 1;
            uint32_t s1ptw: 1;
            uint32_t cm: 1;
            uint32_t ea: 1;
            uint32_t fnv: 1;
            uint32_t res0: 3;
            uint32_t ar: 1;
            uint32_t sf: 1;
            uint32_t srt: 5;

            /* Syndrome sign extend (valid if isv is 1). On a load, indicates whether the
             * value should be sign extended.
             */
            uint32_t sse: 1;

            /* Syndrome access size (valid if isv is 1):
             * 00 - Byte
             * 01 - Halfword
             * 10 - Word
             * 11 - Doubleword.
             */
            uint32_t sas: 2;

            /* Instruction syndrome valid. Indicates whether the SAS
             * through AR fields are valid.
             */
            uint32_t isv: 1;

            /* The final two fields are valid for all exception classes */

            /* Instruction length. 0 = 16 bit, 1 = 32 bit */
            uint32_t il: 1;

            /* Exception class, 100100 or 100101 for data abort */
            uint32_t ec: 6;
        };
        uint32_t as_uint32;
    };
};

/* All of these copied from i386. */
static bool whpx_allowed;
static bool whp_dispatch_initialized;
static HMODULE hWinHvPlatform;
static uint32_t max_vcpu_index;

struct whpx_state whpx_global;
struct WHPDispatch whp_dispatch;

/* XXX debug only - do not merge */
static void dump_syndrome(struct AarchSyndromeDataAbort syndrome)
{
    printf("ec=%#x, il=%d, isv=%d, sas=%d sse=%d, srt=%d, sf=%d, ar=%d"
           " fnv=%d ea=%d cm=%d s1ptw=%d, wnr=%d, dfsc=%d\n",
           syndrome.ec, syndrome.il, syndrome.isv, syndrome.sas, syndrome.sse,
           syndrome.srt, syndrome.sf, syndrome.ar, syndrome.fnv, syndrome.ea,
           syndrome.cm, syndrome.s1ptw, syndrome.wnr, syndrome.dfsc);
}

#if 0
/* XXX debug only - do not merge */
static void dump_cpu(CPUState *cpu, const char *label)
{
    uint32_t pstate;
    CPUARMState *env = &ARM_CPU(cpu)->env;

    printf("Dumping CPU state for %s\n", label);

#define DUMP_FIELD(_name) \
    printf("%16s: %#16llx\n", #_name, (uint64_t) env->_name)

    /* aarch32 general purpose registers */
    DUMP_FIELD(regs[0]);
    DUMP_FIELD(regs[1]);
    DUMP_FIELD(regs[2]);
    DUMP_FIELD(regs[3]);
    DUMP_FIELD(regs[4]);
    DUMP_FIELD(regs[5]);
    DUMP_FIELD(regs[6]);
    DUMP_FIELD(regs[7]);
    DUMP_FIELD(regs[8]);
    DUMP_FIELD(regs[9]);
    DUMP_FIELD(regs[10]);
    DUMP_FIELD(regs[11]);
    DUMP_FIELD(regs[12]);
    DUMP_FIELD(regs[13]);
    DUMP_FIELD(regs[14]);
    DUMP_FIELD(regs[15]);

    /* aarch64 general purpose registers */
    DUMP_FIELD(xregs[0]);
    DUMP_FIELD(xregs[1]);
    DUMP_FIELD(xregs[2]);
    DUMP_FIELD(xregs[3]);
    DUMP_FIELD(xregs[4]);
    DUMP_FIELD(xregs[5]);
    DUMP_FIELD(xregs[6]);
    DUMP_FIELD(xregs[7]);
    DUMP_FIELD(xregs[8]);
    DUMP_FIELD(xregs[9]);
    DUMP_FIELD(xregs[10]);
    DUMP_FIELD(xregs[11]);
    DUMP_FIELD(xregs[12]);
    DUMP_FIELD(xregs[13]);
    DUMP_FIELD(xregs[14]);
    DUMP_FIELD(xregs[15]);
    DUMP_FIELD(xregs[16]);
    DUMP_FIELD(xregs[17]);
    DUMP_FIELD(xregs[18]);
    DUMP_FIELD(xregs[19]);
    DUMP_FIELD(xregs[20]);
    DUMP_FIELD(xregs[21]);
    DUMP_FIELD(xregs[22]);
    DUMP_FIELD(xregs[23]);
    DUMP_FIELD(xregs[24]);
    DUMP_FIELD(xregs[25]);
    DUMP_FIELD(xregs[26]);
    DUMP_FIELD(xregs[27]);
    DUMP_FIELD(xregs[28]);
    DUMP_FIELD(xregs[29]);
    DUMP_FIELD(xregs[30]);
    DUMP_FIELD(xregs[31]);

    DUMP_FIELD(pc);
    DUMP_FIELD(spsr);

    /* pstate is special */
    if (is_a64(env)) {
        pstate = pstate_read(env);
    } else {
        pstate = cpsr_read(env);
    }
    printf("%16s: %#16llx\n", "pstate", (uint64_t) pstate);
}
#endif

/*
 * The WHP names of the ID registers. These can all be read in a single call
 * to the appropriate WHP API.
 */
static WHV_REGISTER_NAME whpx_isar_register_names[] = {
    WHvArm64RegisterIdIsar0El1,
    WHvArm64RegisterIdIsar1El1,
    WHvArm64RegisterIdIsar2El1,
    WHvArm64RegisterIdIsar3El1,
    WHvArm64RegisterIdIsar4El1,
    WHvArm64RegisterIdIsar5El1,

    WHvArm64RegisterIdMmfr0El1,
    WHvArm64RegisterIdMmfr1El1,
    WHvArm64RegisterIdMmfr2El1,
    WHvArm64RegisterIdMmfr3El1,

    WHvArm64RegisterIdPfr0El1,
    WHvArm64RegisterIdPfr1El1,

    WHvArm64RegisterIdMvfr0El1,
    WHvArm64RegisterIdMvfr1El1,
    WHvArm64RegisterIdMvfr2El1,

    WHvArm64RegisterIdDfr0El1,

    /* TODO: dbgdidr, dbgdevid, dbgdevid1 */

    WHvArm64RegisterIdAa64Isar0El1,
    WHvArm64RegisterIdAa64Isar1El1,
    WHvArm64RegisterIdAa64Isar2El1,

    WHvArm64RegisterIdAa64Pfr0El1,
    WHvArm64RegisterIdAa64Pfr1El1,

    WHvArm64RegisterIdAa64Mmfr0El1,
    WHvArm64RegisterIdAa64Mmfr1El1,
    WHvArm64RegisterIdAa64Mmfr2El1,
    WHvArm64RegisterIdAa64Mmfr3El1,

    WHvArm64RegisterIdAa64Dfr0El1,
    WHvArm64RegisterIdAa64Dfr1El1,

    WHvArm64RegisterIdAa64Zfr0El1,
    WHvArm64RegisterIdAa64Smfr0El1,

    /* TODO: reset_pmcr_el0,*/
};

static bool whpx_arm_get_cpu_features_from_host(ARMCPU *cpu)
{
    struct whpx_state *whpx = &whpx_global;
    HRESULT hr;
    /* TODO: Is there an existing macro to get this size? */
    uint32_t register_count = sizeof(whpx_isar_register_names) / sizeof(WHV_REGISTER_NAME);
    WHV_REGISTER_VALUE isar_values[sizeof(whpx_isar_register_names) / sizeof(WHV_REGISTER_NAME)];
    struct ARMISARegisters *isar = &cpu->isar;
    WHV_REGISTER_VALUE *cur_isar_value;

    /* TODO: what's the indentation style? */
    hr = whp_dispatch.WHvGetVirtualProcessorRegisters(
        whpx->partition, WHV_ANY_VP, whpx_isar_register_names, register_count,
        isar_values);

    if (FAILED(hr)) {
        error_report("WHPX: Failed to read ISAR register values, hr=%08lx", hr);
        return false;
    }


    /* TODO: These assignments need to be kept in sync with the order of
     * register names in whpx_isar_register_names. There's probably a better
     * way to structure this that would be easier to maintain.
     */
    memset(isar, 0, sizeof(ARMISARegisters));

    cur_isar_value = &isar_values[0];

    /* TODO: Verify these values against the raw array values to make sure
     * the assignment code is doing what I think it's doing.
     */
    isar->id_isar0 = (cur_isar_value++)->Reg32;
    isar->id_isar1 = (cur_isar_value++)->Reg32;
    isar->id_isar2 = (cur_isar_value++)->Reg32;
    isar->id_isar3 = (cur_isar_value++)->Reg32;
    isar->id_isar4 = (cur_isar_value++)->Reg32;
    isar->id_isar5 = (cur_isar_value++)->Reg32;
    /* TODO: isar->id_isar6 */

    isar->id_mmfr0 = (cur_isar_value++)->Reg32;
    isar->id_mmfr1 = (cur_isar_value++)->Reg32;
    isar->id_mmfr2 = (cur_isar_value++)->Reg32;
    isar->id_mmfr3 = (cur_isar_value++)->Reg32;
    /* TODO: isar->id_mmfr4 */
    /* TODO: isar->id_mmfr5 */

    isar->id_pfr0 = (cur_isar_value++)->Reg32;
    isar->id_pfr1 = (cur_isar_value++)->Reg32;
    /* TODO: isar->id_pfr2 */

    isar->mvfr0 = (cur_isar_value++)->Reg32;
    isar->mvfr1 = (cur_isar_value++)->Reg32;
    isar->mvfr2 = (cur_isar_value++)->Reg32;

    isar->id_dfr0 = (cur_isar_value++)->Reg32;
    /* TODO: isar->id_dfr1 */

    isar->id_aa64isar0 = (cur_isar_value++)->Reg64;
    isar->id_aa64isar1 = (cur_isar_value++)->Reg64;
    isar->id_aa64isar2 = (cur_isar_value++)->Reg64;

    isar->id_aa64pfr0 = (cur_isar_value++)->Reg64;
    /* TODO: Implement VSE support. For now, hide it. */
    isar->id_aa64pfr0 &= ~(0x0000000f00000000ll);

    isar->id_aa64pfr1 = (cur_isar_value++)->Reg64;

    isar->id_aa64mmfr0 = (cur_isar_value++)->Reg64;
    isar->id_aa64mmfr1 = (cur_isar_value++)->Reg64;
    isar->id_aa64mmfr2 = (cur_isar_value++)->Reg64;
    isar->id_aa64mmfr3 = (cur_isar_value++)->Reg64;

    isar->id_aa64dfr0 = (cur_isar_value++)->Reg64;
    isar->id_aa64dfr1 = (cur_isar_value++)->Reg64;

    isar->id_aa64zfr0 = (cur_isar_value++)->Reg64;
    isar->id_aa64smfr0 = (cur_isar_value++)->Reg64;

    /* If this check fails, it's actually too late, because we've already
     * accessed off the end of the array.
     */
    assert(cur_isar_value - isar_values == register_count);

#define XSTR(s) str(s)
#define STR(s) #s

#define DUMP_ID(reg) printf("%16s: %016llx\n", STR(reg), (uint64_t)isar->reg)
    DUMP_ID(id_aa64pfr0);

    return true;
}

void whpx_arm_set_cpu_features_from_host(ARMCPU *cpu)
{
    /* TODO: Implement this function */
    /* TODO: look at kvm code in kvm_arm_get_host_cpu_features */
    /* WHV_ARM64_PROCESSOR_FEATURES/FEATURES1/WHV_PROCESSOR_FEATURES_BANKS
     * exposes some ARM CPU features.
     *
     * This is exposed via WHV_CAPABILITY/WHV_CAPABILITY_CODE, which is in
     * turn exposed through WhvGetCapability.
     *
     * winhvplatformdefs.h/winhvplatform.h
     */
    WHV_CAPABILITY capability = {};
    uint32_t capability_size;
    HRESULT hr;
    uint64_t features = 0;
    CPUARMState *env = &cpu->env;

    hr = whp_dispatch.WHvGetCapability(WHvCapabilityCodeProcessorFeaturesBanks,
                                       &capability, sizeof(WHV_CAPABILITY),
                                       &capability_size);

    if (FAILED(hr)) {
        error_report("WHPX: Failed to read processor capabilities, hr=%08lx",
                     hr);
        return;
    }

    if (capability.ProcessorFeaturesBanks.Bank0.PmuV3) {
        features |= 1ULL << ARM_FEATURE_PMU;
    }
    /* TODO: These are what KVM sets. Are they appropriate for WHPX? */
    features |= 1ULL << ARM_FEATURE_V8;
    features |= 1ULL << ARM_FEATURE_NEON;
    features |= 1ULL << ARM_FEATURE_AARCH64;
    features |= 1ULL << ARM_FEATURE_GENERIC_TIMER;

    /*
     * TODO: Hard-coding support for generic V8. Support actual target if
     * we're running on it?
     */
    env->features = features;
    cpu->dtb_compatible = "arm,arm-v8";
    /* TODO: Unclear if this will work with a non-KVM accelerator, or if we
     * have to pretend to have some different target.
     */
    cpu->kvm_target = QEMU_KVM_ARM_TARGET_NONE;

    /* TODO: ARMISARegisters, which are available from
     * GetVirtualProcessorRegisters, possibly after creating a dummy VP
     * like KVM does.
     */
    if (!whpx_arm_get_cpu_features_from_host(cpu)) {
        error_report("WHPX: Unable to read host CPU features");

        /* We can't report this error yet, so flag that we need to in
         * arm_cpu_realizefn().
         */
        cpu->host_cpu_probe_failed = true;
    }

}

/* Partially derived from i386 */
/* The corresponding functions in KVM is kvm_arch_put_registers */
static void whpx_set_registers(CPUState *cpu, int level)
{
    struct whpx_state *whpx = &whpx_global;
    ARMCPU *arm_cpu = ARM_CPU(cpu);
    CPUARMState *env = &arm_cpu->env;
    struct whpx_register_set vcxt;
    HRESULT hr;
    int idx;
    int fp_reg_nr;
    uint32_t pstate;

    assert(cpu_is_stopped(cpu) || qemu_cpu_is_self(cpu));

    /* XXX debugging */
    //dump_cpu(cpu, "set_registers");

    /* TODO: aarch32 support */

    /* TODO: Is there an equivalent of the TSC? */

    memset(&vcxt, 0, sizeof(struct whpx_register_set));

    /* The X registers are the first 32 registers in the WHPX array.
     * This includes the frame pointer, link register, and non-banked
     * stack pointer.
     * TODO: SP probably needs to be handled more like KVM.
     */
    for (idx = 0; idx < CPU_NB_REGS64; idx++) {
        vcxt.values[idx].Reg64 = env->xregs[idx];
    }

    /* Program counter */
    vcxt.values[idx++].Reg64 = env->pc;

    /* WHP treats pstate as a single register. QEMU splits pstate across
     * several different fields. Translate between the two formats.
     */
    if (is_a64(env)) {
        pstate = pstate_read(env);
    } else {
        pstate = cpsr_read(env);
    }
    vcxt.values[idx++].Reg32 = pstate;

    /* TODO: uncached_cpsr */
    vcxt.values[idx++].Reg32 = env->spsr;

    /* TODO: banked_spsr */
    /* TODO: banked_r13 */
    /* TODO: banked_r14 */
    /* TODO: usr_regs */
    /* TODO: fiq_regs */

    /* TODO: SVE */

    /* TODO: Other exception link registers */
    vcxt.values[idx++].Reg64 = env->elr_el[1];

    /* TODO: Other banked stack pointers */
    vcxt.values[idx++].Reg64 = env->sp_el[0];
    vcxt.values[idx++].Reg64 = env->sp_el[1];

    /* MMU translation registers */
    vcxt.values[idx++].Reg64 = env->cp15.ttbr0_ns;
    vcxt.values[idx++].Reg64 = env->cp15.ttbr1_ns;

    /* TODO: Other MMU translation registers */

    /* The 32 floating point registers are arranged in the same relative
     * order in QEMU and WHP.
     *
     * TODO: This code is likely wrong. The Q registers are 128 bits.
     */
    for (fp_reg_nr = 0; fp_reg_nr < 32; fp_reg_nr++) {
        vcxt.values[idx++].Reg64 = *aa64_vfp_qreg(env, fp_reg_nr);
    }

    vcxt.values[idx++].Reg64 = env->vfp.fpsr;
    vcxt.values[idx++].Reg64 = env->vfp.fpcr;

    /* TODO: fp_status */
    /* TODO: zcr */
    /* TODO: smcr */

    assert(idx == RTL_NUMBER_OF(whpx_register_names));
    hr = whp_dispatch.WHvSetVirtualProcessorRegisters(
        whpx->partition, cpu->cpu_index,
        whpx_register_names,
        RTL_NUMBER_OF(whpx_register_names),
        &vcxt.values[0]);

    if (FAILED(hr)) {
        error_report("WHPX:Failed to set virtual processor context, hr=%08lx",
                     hr);
    }
}

static void whpx_get_registers(CPUState *cpu)
{
    struct whpx_state *whpx = &whpx_global;
    ARMCPU *arm_cpu = ARM_CPU(cpu);
    CPUARMState *env = &arm_cpu->env;
    struct whpx_register_set vcxt;
    HRESULT hr;
    int idx;
    int fp_reg_nr;
    uint32_t pstate;

    assert(cpu_is_stopped(cpu) || qemu_cpu_is_self(cpu));

    /* TODO: Is there an equivalent of the TSC? */

    hr = whp_dispatch.WHvGetVirtualProcessorRegisters(
        whpx->partition, cpu->cpu_index,
        whpx_register_names,
        RTL_NUMBER_OF(whpx_register_names),
        &vcxt.values[0]);
    if (FAILED(hr)) {
        error_report("WHPX: Failed to get virtual processor context, hr=%08lx",
                     hr);
    }

    /* The X registers are the first 32 registers in the WHPX array.
     * This includes the frame pointer, link register, and non-banked
     * stack pointer
     */
    for (idx = 0; idx < CPU_NB_REGS64; idx++) {
        env->xregs[idx] = vcxt.values[idx].Reg64;
    }

    /* Program counter */
    env->pc = vcxt.values[idx++].Reg64;

    /* WHP treats pstate as a single register. QEMU splits pstate across
     * several different fields. Translate between the two formats.
     */
    pstate = vcxt.values[idx++].Reg32;
    if (is_a64(env)) {
        pstate_write(env, pstate);
    } else {
        assert(0);
        cpsr_write(env, pstate, 0xffffffff, CPSRWriteRaw);
    }

    /* TODO: uncached_cpsr */
    env->spsr = vcxt.values[idx++].Reg32;

    /* TODO: banked_spsr */
    /* TODO: banked_r13 */
    /* TODO: banked_r14 */
    /* TODO: usr_regs */
    /* TODO: fiq_regs */

    /* TODO: Other exception link registers */
    env->elr_el[1] = vcxt.values[idx++].Reg64;

    /* TODO: Other banked stack pointers */
    env->sp_el[0] = vcxt.values[idx++].Reg64;
    env->sp_el[1] = vcxt.values[idx++].Reg64;

    /* MMU translation registers */
    env->cp15.ttbr0_ns = vcxt.values[idx++].Reg64;
    env->cp15.ttbr1_ns = vcxt.values[idx++].Reg64;

    /* TODO: Other MMU translation registers */

    /* The 32 floating point registers are arranged in the same relative
     * order in QEMU and WHP.
     *
     * TODO: These are probably wrong.
     */
    for (fp_reg_nr = 0; fp_reg_nr < 32; fp_reg_nr++) {
        *aa64_vfp_qreg(env, fp_reg_nr) = vcxt.values[idx++].Reg64;
    }

    env->vfp.fpsr = vcxt.values[idx++].Reg64;
    env->vfp.fpcr = vcxt.values[idx++].Reg64;

    /* TODO: fp_status */
    /* TODO: zcr */
    /* TODO: smcr */

    assert(idx == RTL_NUMBER_OF(whpx_register_names));

    //dump_cpu(cpu, "get_registers");
}

static void do_whpx_cpu_synchronize_state(CPUState *cpu, run_on_cpu_data arg)
{
    if (!cpu->accel->dirty) {
        whpx_get_registers(cpu);
        cpu->accel->dirty = true;
    }
}

static void do_whpx_cpu_synchronize_post_reset(CPUState *cpu,
                                               run_on_cpu_data arg)
{
    whpx_set_registers(cpu, WHPX_SET_RESET_STATE);
    cpu->accel->dirty = false;
}

static void do_whpx_cpu_synchronize_post_init(CPUState *cpu,
                                              run_on_cpu_data arg)
{
    whpx_set_registers(cpu, WHPX_SET_FULL_STATE);
    cpu->accel->dirty = false;
}

static void do_whpx_cpu_synchronize_pre_loadvm(CPUState *cpu,
                                               run_on_cpu_data arg)
{
    cpu->accel->dirty = true;
}

/*
 * CPU support.
 */

void whpx_cpu_synchronize_state(CPUState *cpu)
{
    if (!cpu->accel->dirty) {
        run_on_cpu(cpu, do_whpx_cpu_synchronize_state, RUN_ON_CPU_NULL);
    }
}

void whpx_cpu_synchronize_post_reset(CPUState *cpu)
{
    run_on_cpu(cpu, do_whpx_cpu_synchronize_post_reset, RUN_ON_CPU_NULL);
}

void whpx_cpu_synchronize_post_init(CPUState *cpu)
{
    run_on_cpu(cpu, do_whpx_cpu_synchronize_post_init, RUN_ON_CPU_NULL);
}

void whpx_cpu_synchronize_pre_loadvm(CPUState *cpu)
{
    run_on_cpu(cpu, do_whpx_cpu_synchronize_pre_loadvm, RUN_ON_CPU_NULL);
}

void whpx_cpu_synchronize_pre_resume(bool step_pending)
{
    whpx_global.step_pending = step_pending;
}

/*
 * Vcpu support.
 */
static Error *whpx_migration_blocker;

/* Partially derived from i386 */
int whpx_init_vcpu(CPUState *cpu)
{
    HRESULT hr;
    struct whpx_state *whpx = &whpx_global;
    AccelCPUState *vcpu = NULL;
    Error *local_error = NULL;
    int ret;

    /* Add migration blockers for all unsupported features of the
     * Windows Hypervisor Platform. TODO: WHP on ARM may have additional
     * missing features that are not listed here.
     */
    if (whpx_migration_blocker == NULL) {
        error_setg(&whpx_migration_blocker,
               "State blocked due to dirty memory tracking support");

        if (migrate_add_blocker(&whpx_migration_blocker, &local_error) < 0) {
            error_report_err(local_error);
            ret = -EINVAL;
            goto error;
        }
    }

    vcpu = g_new0(AccelCPUState, 1);

    hr = whp_dispatch.WHvCreateVirtualProcessor(
        whpx->partition, cpu->cpu_index, 0 /* flags, must be zero */);
    if (FAILED(hr)) {
        error_report("WHPX: Failed to create a virtual processor,"
                     " hr=%08lx", hr);
        ret = -EINVAL;
        goto error;
    }

    /* TODO: is there an equivalent of tsc_khz? */
    /* TODO: is there an equivalent of apic_bus_freq? */

    vcpu->dirty = true;
    cpu->accel = vcpu;
    max_vcpu_index = max(max_vcpu_index, cpu->cpu_index);

    return 0;

error:
    g_free(vcpu);

    return ret;
}

static hwaddr syndrome_to_data_len(struct AarchSyndromeDataAbort syndrome)
{
    hwaddr len;
    assert(syndrome.isv);
    len = (hwaddr) 1 << (syndrome.sas);
    assert(len >= 1 && len <= 8);
    return len;
}

static uint64_t mask_value(uint64_t val, hwaddr len, bool sign_extend)
{
    int64_t final;
    uint64_t mask = ~0;

    switch (len) {
    case 1:
        mask >>= (64 - 8);
        final = (int64_t)(int8_t)(val & mask);
        break;

    case 2:
        mask >>= (64 - 16);
        final = (int64_t)(int16_t)(val & mask);
        break;

    case 4:
        mask >>= (64 - 32);
        final = (int64_t)(int32_t)(val & mask);
        break;

    case 8:
        final = (int64_t) val;
        break;

    default:
        g_assert_not_reached();
    }
    return (uint64_t) final;
}

/* TODO: We're not tracking registered MMIO regions. Do we need to?
 *
 * TODO: This probably doesn't work for AARCH32 code yet.
 */
static int handle_gpa_exit(CPUState *cpu)
{
    struct whpx_state *whpx = &whpx_global;
    AccelCPUState *vcpu = cpu->accel;
    WHV_MEMORY_ACCESS_CONTEXT *access_info;

    /* XXX - debugging - probably don't need this */
    whpx_get_registers(cpu);
    access_info = &vcpu->exit_ctx.MemoryAccess;
    struct AarchSyndromeGeneric gen_syndrome;
    struct AarchSyndromeDataAbort da_syndrome;
    hwaddr data_len;
    hwaddr data_addr;
#define REG_PAIR 2
    WHV_REGISTER_VALUE regs[REG_PAIR];
    WHV_REGISTER_NAME reg_names[REG_PAIR];
    HRESULT hr;

    /* XXX debugging */
    static uint64_t last_pc;
    bool should_log = (last_pc != access_info->Header.Pc);
    last_pc = access_info->Header.Pc;

    gen_syndrome.as_uint32 = (uint32_t) access_info->Syndrome;
    if (should_log) {
        printf("Unhandled GPA with syndrome ec %#06x il %d\n", gen_syndrome.ec,
               gen_syndrome.il);
    }

    if (gen_syndrome.ec == EC_DATAABORT ||
        gen_syndrome.ec == EC_DATAABORT_SAME_EL) {
        da_syndrome.as_uint32 = (uint32_t) access_info->Syndrome;

        if (should_log) {
            dump_syndrome(da_syndrome);
        }
        /* XXX - debugging */
        WHV_INTERCEPT_MESSAGE_HEADER *int_hdr = &access_info->Header;
        if (should_log) {
            printf("Intercept header len %d, access_type %d, Pc %016llx\n",
                   int_hdr->InstructionLength, int_hdr->InterceptAccessType,
                   int_hdr->Pc);
            printf("Unmapped GPA: len %d inst %#010x info %#04x GPA %#018llx GVA %#018llx syndrome %#010x\n",
                   access_info->AccessInfo.AsUINT8,
                   access_info->InstructionByteCount,
                   *(uint32_t*) access_info->InstructionBytes,
                   access_info->Gpa, access_info->Gva,
                   da_syndrome.as_uint32);
        }

        /* TODO: Handle situations where the instruction syndrome info is
         * not valid
         */
        assert(da_syndrome.isv);

        /* TODO: There are addressing modes that update the base register. How
         * should those be handled?
         */

        /* Information common to both reads and writes */
        /* TODO: It may be necessary to more than just simple reads/write */
        data_len = syndrome_to_data_len(da_syndrome);
        /* This is the guest physical address being accessed */
        data_addr = access_info->Gpa;

        memset(&regs, 0, sizeof (WHV_REGISTER_VALUE) * REG_PAIR);
        reg_names[0] = WHvArm64RegisterX0 + da_syndrome.srt;
        assert(reg_names[0] >= WHvArm64RegisterX0 &&
               reg_names[0] <= WHvArm64RegisterLr);

        regs[1].Reg64 = int_hdr->Pc + int_hdr->InstructionLength;
        reg_names[1] = WHvArm64RegisterPc;

        if (da_syndrome.wnr) {
            /* wnr=1 means write to memory */
            /* Get register contents from WHP */
            hr = whp_dispatch.WHvGetVirtualProcessorRegisters(
                whpx->partition,
                cpu->cpu_index,
                &reg_names[0],
                1,
                &regs[0]);
            if (FAILED(hr)) {
                error_report("WHPX: Failed to read virtual register %d\n",
                             reg_names[0]);
                return -1;
            }
            /* TODO: This function doesn't have a return value. That may mean
             * that we need to track physically-not-present memory and
             * handle it ourselves? Or does this function work if the address
             * is unbacked?
             */
            cpu_physical_memory_write(data_addr, &regs[0].Reg64, data_len);

            /* Update Pc */
            hr = whp_dispatch.WHvSetVirtualProcessorRegisters(
                whpx->partition,
                cpu->cpu_index,
                &reg_names[1],
                1,
                &regs[1]);
            if (FAILED(hr)) {
                error_report("WHPX: Failed to write Pc\n");
                return -1;
            }
        } else {
            /* wnr=0 means read from memory */

            /* TODO: This function doesn't have a return value. That may mean
             * that we need to track physically-not-present memory and
             * handle it ourselves? Or does this function return correct
             * data when a physically not present address is read?
             */
            cpu_physical_memory_read(data_addr, &regs[0].Reg64, data_len);
            regs[0].Reg64 = mask_value(regs[0].Reg64, data_len, da_syndrome.sse);

            assert(int_hdr->InstructionLength == 2 ||
                   int_hdr->InstructionLength == 4);

            /* Set the target register and update PC */
            hr = whp_dispatch.WHvSetVirtualProcessorRegisters(
                whpx->partition,
                cpu->cpu_index,
                &reg_names[0], REG_PAIR, &regs[0]);
            if (FAILED(hr)) {
                error_report("WHPX: Failed to write virtual register %d or Pc\n",
                             reg_names[0]);
                return -1;
            }
        }
    } else {
        /* TODO: Handle other types of GPA exits. */
        g_assert_not_reached();
    }

    assert(!cpu->accel->dirty);
    return 1;
}

/* XXX debugging - force emulation to convinue even if there are unexpected
 * exits.
 */
static bool force_continue(void)
{
    const char *val = getenv("QEMU_FORCE_CONTINUE");
    return(val != NULL);
}

static int whpx_vcpu_run(CPUState *cpu)
{
    struct whpx_state *whpx = &whpx_global;
    HRESULT hr;
    AccelCPUState *vcpu = cpu->accel;
    int ret;

    g_assert(bql_locked());

    /* TODO: Breakpoint handling */

    bql_unlock();

    /* TODO: WHPX step mode (more breakpoint handling) */

    cpu_exec_start(cpu);

    do {
        if (cpu->accel->dirty) {
            whpx_set_registers(cpu, WHPX_SET_RUNTIME_STATE);
            cpu->accel->dirty = false;
        }

        /* TODO: Single step handling */

        /* TODO: Interrupt injection */

        hr = whp_dispatch.WHvRunVirtualProcessor(
            whpx->partition, cpu->cpu_index,
            &vcpu->exit_ctx, sizeof(vcpu->exit_ctx));

        if (FAILED(hr)) {
            error_report("WHPX: Failed to exec a virtual processor,"
                         " hr=%08lx", hr);
            ret = -1;
            break;
        }

        /* TODO: Is there any post-run work required? */

        switch (vcpu->exit_ctx.ExitReason) {
        case WHvRunVpExitReasonCanceled:
            cpu->exception_index = EXCP_INTERRUPT;
            ret = 1;
            break;

        case WHvRunVpExitReasonUnmappedGpa:
            ret = handle_gpa_exit(cpu);
            if (force_continue()) {
                ret = 1;
            }
            break;

        case WHvRunVpExitReasonNone:
        case WHvRunVpExitReasonUnrecoverableException:
        case WHvRunVpExitReasonInvalidVpRegisterValue:
        case WHvRunVpExitReasonUnsupportedFeature:
        default:
            if (force_continue()) {
                ret = 1;
                break;
            }
            error_report("WHPX: Unexpected VP exit code %08x",
                         vcpu->exit_ctx.ExitReason);
            whpx_get_registers(cpu);
            bql_lock();
            qemu_system_guest_panicked(cpu_get_crash_info(cpu));
            bql_unlock();
            ret = -1;
            break;
        }

    } while (!ret);

    /* TODO: Additional breakpoint handling */
    cpu_exec_end(cpu);

    bql_lock();
    current_cpu = cpu;
    /*
     * TODO: Handle last VCPU stopping by removing any previously set
     * breakpoints.
     */
    qatomic_set(&cpu->exit_request, false);
    return ret < 0;
}

int whpx_vcpu_exec(CPUState *cpu)
{
    int ret;
    int fatal;

    for (;;) {
        if (cpu->exception_index >= EXCP_INTERRUPT) {
            ret = cpu->exception_index;
            cpu->exception_index = -1;
            break;
        }

        fatal = whpx_vcpu_run(cpu);

        if (fatal) {
            error_report("WHPX: Failed to exec a virtual processor");
            abort();
        }
    }

    return ret;
}

void whpx_destroy_vcpu(CPUState *cpu)
{
    /* TODO: Implement this function */
    assert(false);
}

/* Identical to i386 */
void whpx_vcpu_kick(CPUState *cpu)
{
    struct whpx_state *whpx = &whpx_global;
    whp_dispatch.WHvCancelRunVirtualProcessor(
        whpx->partition, cpu->cpu_index, 0);
}

/*
 * Memory support.
 */

/* Same as i386 */
static void whpx_update_mapping(hwaddr start_pa, ram_addr_t size,
                                void *host_va, int add, int rom,
                                const char *name)
{
    struct whpx_state *whpx = &whpx_global;
    HRESULT hr;

    if (add) {
        printf("WHPX: ADD PA:%p Size:%p, Host:%p, %s, '%s'\n",
               (void*)start_pa, (void*)size, host_va,
               (rom ? "ROM" : "RAM"), name);
    } else {
        printf("WHPX: DEL PA:%p Size:%p, Host:%p,      '%s'\n",
               (void*)start_pa, (void*)size, host_va, name);
    }

    if (add) {
        hr = whp_dispatch.WHvMapGpaRange(whpx->partition,
                                         host_va,
                                         start_pa,
                                         size,
                                         (WHvMapGpaRangeFlagRead |
                                          WHvMapGpaRangeFlagExecute |
                                          (rom ? 0 : WHvMapGpaRangeFlagWrite)));
    } else {
        hr = whp_dispatch.WHvUnmapGpaRange(whpx->partition,
                                           start_pa,
                                           size);
    }

    if (FAILED(hr)) {
        error_report("WHPX: Failed to %s GPA range '%s' PA:%p, Size:%p bytes,"
                     " Host:%p, hr=%08lx",
                     (add ? "MAP" : "UNMAP"), name,
                     (void *)(uintptr_t)start_pa, (void *)size, host_va, hr);
    }
}

/* Same as i386 */
static void whpx_process_section(MemoryRegionSection *section, int add)
{
    MemoryRegion *mr = section->mr;
    hwaddr start_pa = section->offset_within_address_space;
    ram_addr_t size = int128_get64(section->size);
    unsigned int delta;
    uint64_t host_va;

    if (!memory_region_is_ram(mr) && !memory_region_is_romd(mr)) {
        /* XXX  - debugging - just to find out where regions are */
        printf("WHPX: HID PA:%p Size:%p, '%s'\n",
               (void*)start_pa, (void*)size, mr->name);
        return;
    }

    delta = qemu_real_host_page_size() - (start_pa & ~qemu_real_host_page_mask());
    delta &= ~qemu_real_host_page_mask();
    if (delta > size) {
        return;
    }
    start_pa += delta;
    size -= delta;
    size &= qemu_real_host_page_mask();
    if (!size || (start_pa & ~qemu_real_host_page_mask())) {
        return;
    }

    host_va = (uintptr_t)memory_region_get_ram_ptr(mr)
            + section->offset_within_region + delta;

    whpx_update_mapping(start_pa, size, (void *)(uintptr_t)host_va, add,
                        memory_region_is_rom(mr), mr->name);
}

/* All of these same as i386 */
static void whpx_transaction_begin(MemoryListener *listener)
{
}

static void whpx_transaction_commit(MemoryListener *listener)
{
}

static void whpx_region_add(MemoryListener *listener,
                           MemoryRegionSection *section)
{
    memory_region_ref(section->mr);
    whpx_process_section(section, 1);
}

static void whpx_region_del(MemoryListener *listener,
                           MemoryRegionSection *section)
{
    whpx_process_section(section, 0);
    memory_region_unref(section->mr);
}

static void whpx_log_sync(MemoryListener *listener,
                          MemoryRegionSection *section)
{
    MemoryRegion *mr = section->mr;

    if (!memory_region_is_ram(mr)) {
        return;
    }

    memory_region_set_dirty(mr, 0, int128_get64(section->size));
}

static MemoryListener whpx_memory_listener = {
    .name = "whpx",
    .begin = whpx_transaction_begin,
    .commit = whpx_transaction_commit,
    .region_add = whpx_region_add,
    .region_del = whpx_region_del,
    .log_sync = whpx_log_sync,
    .priority = MEMORY_LISTENER_PRIORITY_ACCEL,
};

static void whpx_memory_init(void)
{
    memory_listener_register(&whpx_memory_listener, &address_space_memory);
}

/*
 * Load the functions from the given library, using the given handle. If a
 * handle is provided, it is used, otherwise the library is opened. The
 * handle will be updated on return with the opened one.
 */
static bool load_whp_dispatch_fns(HMODULE *handle,
    WHPFunctionList function_list)
{
    HMODULE hLib = *handle;

    #define WINHV_PLATFORM_DLL "WinHvPlatform.dll"
    #define WHP_LOAD_FIELD_OPTIONAL(return_type, function_name, signature) \
        whp_dispatch.function_name = \
            (function_name ## _t)GetProcAddress(hLib, #function_name); \

    #define WHP_LOAD_FIELD(return_type, function_name, signature) \
        whp_dispatch.function_name = \
            (function_name ## _t)GetProcAddress(hLib, #function_name); \
        if (!whp_dispatch.function_name) { \
            error_report("Could not load function %s", #function_name); \
            goto error; \
        } \

    #define WHP_LOAD_LIB(lib_name, handle_lib) \
    if (!handle_lib) { \
        handle_lib = LoadLibrary(lib_name); \
        if (!handle_lib) { \
            error_report("Could not load library %s.", lib_name); \
            goto error; \
        } \
    } \

    switch (function_list) {
    case WINHV_PLATFORM_FNS_DEFAULT:
        WHP_LOAD_LIB(WINHV_PLATFORM_DLL, hLib)
        LIST_WINHVPLATFORM_FUNCTIONS(WHP_LOAD_FIELD)
        break;

    case WINHV_PLATFORM_FNS_SUPPLEMENTAL:
        WHP_LOAD_LIB(WINHV_PLATFORM_DLL, hLib)
        LIST_WINHVPLATFORM_FUNCTIONS_SUPPLEMENTAL(WHP_LOAD_FIELD_OPTIONAL)
        break;
    }

    *handle = hLib;
    return true;

error:
    if (hLib) {
        FreeLibrary(hLib);
    }

    return false;
}

static bool init_whp_dispatch(void)
{
    if (whp_dispatch_initialized) {
        return true;
    }

    if (!load_whp_dispatch_fns(&hWinHvPlatform, WINHV_PLATFORM_FNS_DEFAULT)) {
        goto error;
    }

    assert(load_whp_dispatch_fns(&hWinHvPlatform,
        WINHV_PLATFORM_FNS_SUPPLEMENTAL));
    whp_dispatch_initialized = true;

    return true;
error:
    if (hWinHvPlatform) {
        FreeLibrary(hWinHvPlatform);
    }

    return false;
}

/* Partially copied from i386. */
static int whpx_accel_init(MachineState *ms)
{
    struct whpx_state *whpx;
    int ret;
    HRESULT hr;
    WHV_CAPABILITY whpx_cap;
    UINT32 whpx_cap_size;
    WHV_PARTITION_PROPERTY prop;
    WHV_ARM64_IC_PARAMETERS *ic_param;

    whpx = &whpx_global;

    if (!init_whp_dispatch()) {
        ret = -ENOSYS;
        printf("Failed to initialize whp dispatch\n");
        goto error;
    }

    hr = whp_dispatch.WHvGetCapability(
        WHvCapabilityCodeHypervisorPresent, &whpx_cap,
        sizeof(whpx_cap), &whpx_cap_size);
    if (FAILED(hr) || !whpx_cap.HypervisorPresent) {
        error_report("WHPX: No accelerator found, hr=%08lx", hr);
        ret = -ENOSPC;
        goto error;
    }

    hr = whp_dispatch.WHvCreatePartition(&whpx->partition);
    if (FAILED(hr)) {
        error_report("WHPX: Failed to create partition, hr=%08lx", hr);
        ret = -EINVAL;
        goto error;
    }

    /*
     * TODO: query any required or optional partition capabilities that
     * are relevant to acceleration.
     */

    memset(&prop, 0, sizeof(WHV_PARTITION_PROPERTY));
    prop.ProcessorCount = ms->smp.cpus;
    hr = whp_dispatch.WHvSetPartitionProperty(
        whpx->partition,
        WHvPartitionPropertyCodeProcessorCount,
        &prop,
        sizeof(WHV_PARTITION_PROPERTY));

    if (FAILED(hr)) {
        error_report("WHPX: Failed to set partition processor count to %u,"
                     " hr=%08lx", prop.ProcessorCount, hr);
        ret = -EINVAL;
        goto error;
    }

    /* TODO: If necessary, register any required extended VM exits. */

    /*
     * Initialize the interrupt controller.
     * TODO: Use the requested interrupt controller properties instead
     * of hard-coded ones.
     */
    memset(&prop, 0, sizeof(WHV_PARTITION_PROPERTY));
    ic_param = &prop.Arm64IcParameters;
    ic_param->EmulationMode = WHvArm64IcEmulationModeGicV3;
    ic_param->GicV3Parameters.GicdBaseAddress = 0xffff0000;
    ic_param->GicV3Parameters.GitsTranslaterBaseAddress = 0xeff68000;
    ic_param->GicV3Parameters.GicLpiIntIdBits = 1;
    ic_param->GicV3Parameters.GicPpiOverflowInterruptFromCntv = 0x1B;
    ic_param->GicV3Parameters.GicPpiPerformanceMonitorsInterrupt = 0x17;
    hr = whp_dispatch.WHvSetPartitionProperty(
        whpx->partition,
        WHvPartitionPropertyCodeArm64IcParameters,
        &prop,
        sizeof(WHV_PARTITION_PROPERTY));

    if (FAILED(hr)) {
        error_report("WHPX: Failed to set interrupt controller properties,"
                     " hr=%08lx", hr);
        ret = -EINVAL;
        goto error;
    }

    hr = whp_dispatch.WHvSetupPartition(whpx->partition);
    if (FAILED(hr)) {
        error_report("WHPX: Failed to set up partition, hr=%08lx", hr);
        ret = -EINVAL;
        goto error;
    }

    whpx_memory_init();

    printf("Windows Hypervisor Platform accelerator is initialized (but not working)\n");
    return 0;

error:
    return ret;
}

int whpx_enabled(void)
{
    return whpx_allowed;
}

bool whpx_irqchip_in_platform(void) {

    return true;
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
