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
#include "qemu/error-report.h"
#include "hw/boards.h"
#include "whpx-arm.h"

#include "whpx-internal.h"
#include "whpx-accel-ops.h"

// XXX do not merge
#include <stdio.h>

/* Partially copied from i386 */
struct AccelCPUState {
    bool window_registered;
    bool interruptable;
    /*
    bool ready_for_pic_interrupt;
    uint64_t tpr;
    uint64_t apic_base;
    bool interruption_pending;
    */
    bool dirty;

    /* Must be the last field as it may have a tail */
    /*WHV_RUN_VP_EXIT_CONTEXT exit_ctx;*/
};

/* All of these copied from i386. */
static bool whpx_allowed;
static bool whp_dispatch_initialized;
static HMODULE hWinHvPlatform, hWinHvEmulation;

struct whpx_state whpx_global;

struct WHPDispatch whp_dispatch;

void whpx_arm_set_cpu_features_from_host(ARMCPU *cpu)
{
    /* TODO: Implement this function */
}

/* TODO: Refactor to share code with i386 if possible. */
static void whpx_set_registers(CPUState *cpu, int level)
{
    /* TODO: Implement this function */
    assert(false);
}

static void whpx_get_registers(CPUState *cpu)
{
    /* TODO: Implement this function */
    assert(false);
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

int whpx_init_vcpu(CPUState *cpu)
{
    /* TODO: Implement this function */
    assert(false);
    return 0;
}

int whpx_vcpu_exec(CPUState *cpu)
{
    /* TODO: Implement this function */
    assert(false);
    return 0;
}

void whpx_destroy_vcpu(CPUState *cpu)
{
    /* TODO: Implement this function */
    assert(false);
}

void whpx_vcpu_kick(CPUState *cpu)
{
    /* TODO: Implement this function */
    assert(false);
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

    /*
    if (add) {
        printf("WHPX: ADD PA:%p Size:%p, Host:%p, %s, '%s'\n",
               (void*)start_pa, (void*)size, host_va,
               (rom ? "ROM" : "RAM"), name);
    } else {
        printf("WHPX: DEL PA:%p Size:%p, Host:%p,      '%s'\n",
               (void*)start_pa, (void*)size, host_va, name);
    }
    */

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

    if (!memory_region_is_ram(mr)) {
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

    if (hWinHvEmulation) {
        FreeLibrary(hWinHvEmulation);
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
