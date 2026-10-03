/* =====================================================================
 * ADLX backend -- AMD ADLX (amdadlx64.dll) sensor provider (Windows).
 *
 * Why: ADL2 sensor queries (Overdrive5/OverdriveN, PMLog) return no
 * usable data on RDNA4 GPUs (Radeon RX 9070 series) -- the driver no
 * longer services those legacy entry points.  ADLX is the supported
 * telemetry interface there (it is what AMD Software itself uses) and
 * also covers older generations, so when this backend matches a device
 * it is preferred over the ADL backend.
 *
 * Everything is resolved at runtime from amdadlx64.dll (installed with
 * the AMD driver into System32); there is no import-library dependency
 * and the program still runs on systems without ADLX.
 * ===================================================================== */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#include <ADLX.h>
/* ISystem.h only forward-declares the performance monitoring
 * interfaces; the C structs live here.  3.h adds the fan duty cycle. */
#include <IPerformanceMonitoring.h>
#include <IPerformanceMonitoring3.h>

#include "backend_adlx.h"

#define ADLX_MAX_GPUS 16

typedef struct {
    IADLXGPU *gpu;
    adlx_int   bus, device, function;   /* -1 when unknown */
    uint32_t   vendor_id, device_id;
    adlx_uint  total_vram_mb;
    char       pnp[256];
} AdlxDevice;

static HMODULE g_dll;
static IADLXSystem *g_sys;
static IADLXPerformanceMonitoringServices *g_perf;
static IADLXGPUList *g_list;
static AdlxDevice g_devs[ADLX_MAX_GPUS];
static int g_count;
static int g_inited;

static ADLXTerminate_Fn g_terminate_fn;

/* ------------------------------------------------------------------ */
/* helpers                                                             */
/* ------------------------------------------------------------------ */

/* ADLX reports PCI IDs as hex strings without a prefix (e.g. "1002",
 * "7550"); some versions may include a "0x" prefix, which strtoul with
 * base 16 also accepts. */
static int parse_id_string(const char *s, uint32_t *out)
{
    char *end;
    unsigned long v;
    if (!s || !*s)
        return -1;
    v = strtoul(s, &end, 16);
    if (end == s)
        return -1;
    *out = (uint32_t)v;
    return 0;
}

/* Read "<devkey>LocationInformation" for a device instance under
 * HKLM\SYSTEM\CurrentControlSet\Enum.  The value is either a plain
 * "PCI bus 3, device 0, function 0" string or (usually) an indirect
 * pci.sys template ending in the numbers we want:
 *   "@System32\drivers\pci.sys,#65536;PCI bus %1, ...;(3,0,0)"
 * so prefer the trailing "(bus,device,function)" group. */
static void bus_location_from_pnp(const char *pnp,
                                  adlx_int *bus, adlx_int *device,
                                  adlx_int *function)
{
    char key[512];
    char buf[512];
    DWORD type = 0, size = sizeof(buf) - 1;
    int vals[3];
    int n = 0;
    const char *p;

    *bus = *device = *function = -1;
    if (!pnp || !*pnp || strlen(pnp) > 200)
        return;

    snprintf(key, sizeof(key), "SYSTEM\\CurrentControlSet\\Enum\\%s", pnp);
    if (RegGetValueA(HKEY_LOCAL_MACHINE, key, "LocationInformation",
                     RRF_RT_REG_SZ,
                     &type, buf, &size) != ERROR_SUCCESS)
        return;
    buf[size] = '\0';

    /* trailing "(bus,device,function)" of the pci.sys template */
    p = strrchr(buf, '(');
    if (p) {
        int a, b, c;
        if (sscanf(p, " ( %d , %d , %d )", &a, &b, &c) == 3) {
            *bus = a;
            *device = b;
            *function = c;
            return;
        }
    }

    /* plain localized string: the first three integers in order are
     * bus, device, function ("PCI bus 3, device 0, function 0") */
    p = buf;
    while (*p && n < 3) {
        if (*p >= '0' && *p <= '9') {
            vals[n++] = (int)strtol(p, (char **)&p, 10);
        } else {
            p++;
        }
    }
    if (n == 3) {
        *bus      = vals[0];
        *device   = vals[1];
        *function = vals[2];
    }
}

static IADLXGPUMetrics *fetch_metrics(int idx)
{
    IADLXGPUMetrics *m = NULL;
    ADLX_RESULT res;
    if (idx < 0 || idx >= g_count || !g_perf)
        return NULL;
    res = g_perf->pVtbl->GetCurrentGPUMetrics(g_perf,
                                              g_devs[idx].gpu, &m);
    if (!ADLX_SUCCEEDED(res) || !m)
        return NULL;
    return m;
}

/* ------------------------------------------------------------------ */
/* init / shutdown                                                     */
/* ------------------------------------------------------------------ */

int adlx_init(void)
{
    ADLXInitialize2_Fn init2;
    ADLXInitialize_Fn  init1;
    ADLX_RESULT res;
    adlx_uint size, i;
    int dbg = getenv("GPU_INFO_DEBUG") != NULL;

    if (g_inited)
        return (g_count > 0) ? 0 : -1;

    g_dll = LoadLibraryA("amdadlx64.dll");
    if (!g_dll) {
        if (dbg) fprintf(stderr, "ADLX: LoadLibraryA failed %lu\n",
                         (unsigned long)GetLastError());
        return -1;
    }

    init2 = (ADLXInitialize2_Fn)GetProcAddress(g_dll, ADLX_INIT2_FUNCTION_NAME);
    init1 = (ADLXInitialize_Fn)GetProcAddress(g_dll, ADLX_INIT_FUNCTION_NAME);
    g_terminate_fn =
        (ADLXTerminate_Fn)GetProcAddress(g_dll, ADLX_TERMINATE_FUNCTION_NAME);
    if ((!init2 && !init1) || !g_terminate_fn)
        goto fail;

    if (init2)
        res = init2(ADLX_FULL_VERSION, &g_sys, NULL);
    else
        res = init1(ADLX_FULL_VERSION, &g_sys);
    if (dbg) fprintf(stderr, "ADLX: init res=%d sys=%p\n", (int)res,
                     (void *)g_sys);
    if (!ADLX_SUCCEEDED(res) || !g_sys)
        goto fail;

    res = g_sys->pVtbl->GetGPUs(g_sys, &g_list);
    if (dbg) fprintf(stderr, "ADLX: GetGPUs res=%d list=%p\n", (int)res,
                     (void *)g_list);
    if (!ADLX_SUCCEEDED(res) || !g_list)
        goto fail;

    size = g_list->pVtbl->Size(g_list);
    for (i = 0; i < size && g_count < ADLX_MAX_GPUS; i++) {
        IADLXGPU *gpu = NULL;
        AdlxDevice *d = &g_devs[g_count];
        const char *str = NULL;

        if (!ADLX_SUCCEEDED(g_list->pVtbl->At_GPUList(g_list, i, &gpu)) ||
            !gpu)
            continue;
        d->gpu = gpu;
        d->bus = d->device = d->function = -1;
        d->vendor_id = d->device_id = 0;
        d->total_vram_mb = 0;

        if (ADLX_SUCCEEDED(gpu->pVtbl->VendorId(gpu, &str)))
            parse_id_string(str, &d->vendor_id);
        if (ADLX_SUCCEEDED(gpu->pVtbl->DeviceId(gpu, &str)))
            parse_id_string(str, &d->device_id);
        if (ADLX_SUCCEEDED(gpu->pVtbl->TotalVRAM(gpu, &d->total_vram_mb)))
            d->total_vram_mb = d->total_vram_mb; /* MB */
        if (ADLX_SUCCEEDED(gpu->pVtbl->PNPString(gpu, &str)) && str) {
            strncpy(d->pnp, str, sizeof(d->pnp) - 1);
            bus_location_from_pnp(d->pnp, &d->bus, &d->device,
                                  &d->function);
        }
        if (dbg)
            fprintf(stderr, "ADLX: dev%u ven=%08x dev=%08x vram=%u bdf=%d/%d/%d pnp=%s\n",
                    (unsigned)i, d->vendor_id, d->device_id,
                    d->total_vram_mb, d->bus, d->device, d->function,
                    d->pnp);
        g_count++;
    }

    if (g_count == 0)
        goto fail;

    if (ADLX_SUCCEEDED(g_sys->pVtbl->GetPerformanceMonitoringServices(
            g_sys, &g_perf)) && g_perf) {
        if (dbg)
            fprintf(stderr, "ADLX: performance monitoring services ok\n");
    }

    g_inited = 1;
    return 0;

fail:
    if (g_list) {
        g_list->pVtbl->Release(g_list);
        g_list = NULL;
    }
    /* IADLXSystem has no Release -- its lifetime ends at ADLXTerminate. */
    g_sys = NULL;
    FreeLibrary(g_dll);
    g_dll = NULL;
    g_terminate_fn = NULL;
    g_count = 0;
    return -1;
}

void adlx_shutdown(void)
{
    int i;
    if (!g_inited && !g_dll)
        return;

    if (g_perf) {
        g_perf->pVtbl->Release(g_perf);
        g_perf = NULL;
    }
    for (i = 0; i < g_count; i++) {
        if (g_devs[i].gpu) {
            g_devs[i].gpu->pVtbl->Release(g_devs[i].gpu);
            g_devs[i].gpu = NULL;
        }
    }
    g_count = 0;
    if (g_list) {
        g_list->pVtbl->Release(g_list);
        g_list = NULL;
    }
    /* IADLXSystem has no Release -- ADLXTerminate ends its lifetime. */
    g_sys = NULL;
    if (g_terminate_fn)
        g_terminate_fn();
    g_terminate_fn = NULL;
    if (g_dll) {
        FreeLibrary(g_dll);
        g_dll = NULL;
    }
    g_inited = 0;
}

int adlx_available(void)
{
    return (g_inited && g_count > 0) ? 1 : 0;
}

/* ------------------------------------------------------------------ */
/* device matching                                                     */
/* ------------------------------------------------------------------ */

int adlx_find_by_pci_topology(uint32_t domain, uint32_t bus,
                              uint32_t device, uint32_t function,
                              int *idx)
{
    int i;
    (void)domain; /* registry LocationInformation carries no domain;
                     domain 0 is assumed (all consumer boards). */
    if (!adlx_available() || !idx)
        return -1;
    for (i = 0; i < g_count; i++) {
        if (g_devs[i].bus  >= 0 && g_devs[i].device >= 0 &&
            g_devs[i].function >= 0 &&
            (uint32_t)g_devs[i].bus    == bus  &&
            (uint32_t)g_devs[i].device == device &&
            (uint32_t)g_devs[i].function == function) {
            *idx = i;
            return 0;
        }
    }
    return -1;
}

int adlx_find_by_pci(uint32_t vendor_id, uint32_t device_id, int *idx)
{
    int i;
    if (!adlx_available() || !idx)
        return -1;
    for (i = 0; i < g_count; i++) {
        if (g_devs[i].vendor_id == vendor_id &&
            g_devs[i].device_id == device_id) {
            *idx = i;
            return 0;
        }
    }
    return -1;
}

/* ------------------------------------------------------------------ */
/* sensor getters                                                      */
/* ------------------------------------------------------------------ */

int adlx_get_temperature(int idx, int *temp_milli_c)
{
    IADLXGPUMetrics *m = fetch_metrics(idx);
    adlx_double t;
    if (!m)
        return -1;
    if (!ADLX_SUCCEEDED(m->pVtbl->GPUTemperature(m, &t))) {
        m->pVtbl->Release(m);
        return -1;
    }
    m->pVtbl->Release(m);
    *temp_milli_c = (int)(t * 1000.0 + 0.5);
    return 0;
}

int adlx_get_fan_speed(int idx, int *rpm, int *percent)
{
    IADLXGPUMetrics *m = fetch_metrics(idx);
    IADLXGPUMetrics3 *m3 = NULL;
    adlx_int r = -1, duty = -1;
    int ok = -1;

    if (!m)
        return -1;
    if (ADLX_SUCCEEDED(m->pVtbl->GPUFanSpeed(m, &r)) && r >= 0) {
        *rpm = (int)r;
        ok = 0;
    }
    /* The fan duty cycle (percent) lives on the IADLXGPUMetrics3
     * extension; ADLX interface IDs are the interface name strings. */
    if (ADLX_SUCCEEDED(m->pVtbl->QueryInterface(m, L"IADLXGPUMetrics3",
                                                (void **)&m3)) && m3) {
        if (ADLX_SUCCEEDED(m3->pVtbl->GPUFanDuty(m3, &duty)) &&
            duty >= 0 && duty <= 100) {
            *percent = (int)duty;
            ok = 0;
        }
        m3->pVtbl->Release(m3);
    }
    if (ok != 0)
        *percent = -1;
    m->pVtbl->Release(m);
    return ok;
}

int adlx_get_utilization(int idx, int *pct)
{
    IADLXGPUMetrics *m = fetch_metrics(idx);
    adlx_double u;
    if (!m)
        return -1;
    if (!ADLX_SUCCEEDED(m->pVtbl->GPUUsage(m, &u))) {
        m->pVtbl->Release(m);
        return -1;
    }
    m->pVtbl->Release(m);
    if (u < 0)   u = 0;
    if (u > 100) u = 100;
    *pct = (int)(u + 0.5);
    return 0;
}

int adlx_get_clocks(int idx, int *core_mhz, int *mem_mhz)
{
    IADLXGPUMetrics *m = fetch_metrics(idx);
    adlx_int core = -1, mem = -1;
    int ok = -1;

    if (!m)
        return -1;
    if (ADLX_SUCCEEDED(m->pVtbl->GPUClockSpeed(m, &core)) && core >= 0) {
        *core_mhz = (int)core;
        ok = 0;
    }
    if (ADLX_SUCCEEDED(m->pVtbl->GPUVRAMClockSpeed(m, &mem)) && mem >= 0) {
        *mem_mhz = (int)mem;
        ok = 0;
    }
    m->pVtbl->Release(m);
    return ok;
}

int adlx_get_power(int idx, int *power_milliwatts)
{
    IADLXGPUMetrics *m = fetch_metrics(idx);
    adlx_double w;
    if (!m)
        return -1;
    if (!ADLX_SUCCEEDED(m->pVtbl->GPUPower(m, &w)) || w <= 0) {
        /* fall back to total board power */
        if (!ADLX_SUCCEEDED(m->pVtbl->GPUTotalBoardPower(m, &w)) ||
            w <= 0) {
            m->pVtbl->Release(m);
            return -1;
        }
    }
    m->pVtbl->Release(m);
    *power_milliwatts = (int)(w * 1000.0 + 0.5);
    return 0;
}

int adlx_get_memory(int idx, uint64_t *used_bytes, uint64_t *total_bytes)
{
    IADLXGPUMetrics *m = fetch_metrics(idx);
    adlx_int used_mb = -1;
    int ok = -1;

    if (!m)
        return -1;
    if (ADLX_SUCCEEDED(m->pVtbl->GPUVRAM(m, &used_mb)) && used_mb >= 0) {
        *used_bytes = (uint64_t)used_mb * 1048576ULL;
        ok = 0;
    }
    m->pVtbl->Release(m);

    /* total VRAM comes from the (static) device object, not metrics */
    if (idx >= 0 && idx < g_count && g_devs[idx].total_vram_mb > 0) {
        *total_bytes = (uint64_t)g_devs[idx].total_vram_mb * 1048576ULL;
        ok = 0;
    }
    return ok;
}
