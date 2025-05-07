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
#include "qemu/accel.h"
#include "qemu/error-report.h"
#include "hw/boards.h"

#include "whpx-internal.h"

// XXX do not merge
#include <stdio.h>

/* All of these copied from i386. */
static bool whpx_allowed;
static bool whp_dispatch_initialized;
static HMODULE hWinHvPlatform, hWinHvEmulation;

struct whpx_state whpx_global;

struct WHPDispatch whp_dispatch;

/* TODO: Refactor to share code with i386 if possible. */

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

    /* TODO: whpx_memory_init() */

    printf("Windows Hypervisor Platform accelerator is initialized (but not working)\n");
    return 0;

error:
    return ret;
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
