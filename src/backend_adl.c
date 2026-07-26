/* ================================================================
 *  backend_adl.c  --  AMD Display Library (atiadlxx.dll) dynamic backend
 *
 *  Loads ADL at runtime via LoadLibrary.  If the DLL is not present
 *  (e.g. no AMD driver installed) the whole backend degrades gracefully.
 *
 *  Uses ADL2 where the modern driver exposes it (memory usage,
 *  OverdriveN) and falls back to legacy ADL / Overdrive5 / Overdrive6
 *  for older drivers.
 * ================================================================ */

#ifdef _WIN32

#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <stdint.h>

#include "backend_adl.h"

/* ------------------------------------------------------------------
 *  Internal structures -- matched to the official ADL SDK headers.
 *  Several structures in the original code had an incorrect leading
 *  iSize or wrong field order; those have been fixed below.
 * ------------------------------------------------------------------ */

#define ADL_OK                    0

typedef void *ADL_CONTEXT_HANDLE;
typedef void* (__stdcall *ADL_MALLOC_CB)(int);

/* AdapterInfo -- exact layout from AMD ADL SDK adl_structures.h
 * (Windows variant, ADL_MAX_PATH = 256, total 1572 bytes).  Do NOT
 * shrink this to an opaque padding blob: ADL_Adapter_AdapterInfo_Get
 * writes the full array at this stride, so a wrong size misaligns
 * every adapter after the first. */
typedef struct {
    int  iSize;                   /*   0 */
    int  iAdapterIndex;           /*   4 */
    char strUDID[256];            /*   8 */
    int  iBusNumber;              /* 264 */
    int  iDeviceNumber;           /* 268 */
    int  iFunctionNumber;         /* 272 */
    int  iVendorID;               /* 276 */
    char strAdapterName[256];     /* 280 */
    char strDisplayName[256];     /* 536 */
    int  iPresent;                /* 792 */
    int  iExist;                  /* 796 */
    char strDriverPath[256];      /* 800 */
    char strDriverPathExt[256];   /* 1056 */
    char strPNPString[256];       /* 1312 */
    int  iOSDisplayIndex;         /* 1568 */
} ADLAdapterInfo;                 /* total: 1572 bytes */

typedef char adl_adapter_info_size_check[
    (sizeof(ADLAdapterInfo) == 1572) ? 1 : -1];

/* Overdrive5 (legacy) */
typedef struct { int iSize; int iTemperature; } ADLOD5Temp;
typedef struct { int iSize; int iFanSpeed; int iFanPercent; int _p[2]; } ADLOD5Fan;
typedef struct { int iSize; int iEngineClock; int iMemoryClock;
                 int iVddc; int iActivityPercent;
                 int iCurrentPerformanceLevel;
                 int iCurrentBusSpeed; int iCurrentBusLanes; } ADLOD5Activity;

/* OverdriveN performance status -- no iSize, official field order */
typedef struct {
    int iCoreClock;
    int iCurrentBusLanes;
    int iCurrentBusSpeed;
    int iCurrentCorePerformanceLevel;
    int iCurrentDCEFPerformanceLevel;
    int iCurrentGFXPerformanceLevel;
    int iCurrentMemoryPerformanceLevel;
    int iDCEFClock;
    int iGFXClock;
    int iGPUActivityPercent;
    int iMaximumBusLanes;
    int iMemoryClock;
    int iUVDClock;
    int iUVDPerformanceLevel;
    int iVCEClock;
    int iVCEPerformanceLevel;
    int iVDDC;
    int iVDDCI;
    int _pad[64];
} ADLODNPerfStatus;

/* ADLMemoryInfo -- no iSize, official field order (iMemorySize first) */
typedef struct {
    long long iMemorySize;        /* total VRAM bytes */
    char      strMemoryType[256];
    long long iMemoryBandwidth;
} ADLMemoryInfo;

/* ADLMemoryInfo2 -- used as a fallback for total memory on some APUs */
typedef struct {
    long long iMemorySize;
    char      strMemoryType[256];
    long long iMemoryBandwidth;
    long long iHyperMemorySize;
    long long iInvisibleMemorySize;
    long long iVisibleMemorySize;
} ADLMemoryInfo2;

/* ------------------------------------------------------------------
 *  PMLog structures (from ADL SDK adl_structures.h / adl_defines.h)
 *
 *  ADL2_New_QueryPMLogData_Get returns ADLPMLogDataOutput -- a snapshot
 *  of all sensor values, indexed directly by ADL_PMLOG_SENSORS enum.
 *  This is the modern AMD telemetry path that works on iGPUs/APUs
 *  where the legacy OverdriveN/Overdrive5 APIs return nothing.
 * ------------------------------------------------------------------ */

#define ADL_PMLOG_MAX_SENSORS 256

/* ADL_PMLOG_SENSORS enum values (subset we use) */
#define ADL_PMLOG_CLK_GFXCLK            1   /* MHz       */
#define ADL_PMLOG_CLK_MEMCLK            2   /* MHz       */
#define ADL_PMLOG_CLK_SOCCLK            3   /* MHz       */
#define ADL_PMLOG_CLK_VCNCLK            7   /* MHz       */
#define ADL_PMLOG_FAN_RPM              14   /* RPM       */
#define ADL_PMLOG_FAN_PERCENTAGE        15   /* percent   */
#define ADL_PMLOG_INFO_ACTIVITY_GFX    19   /* percent   */
#define ADL_PMLOG_INFO_ACTIVITY_MEM    20   /* percent   */
#define ADL_PMLOG_ASIC_POWER           23   /* Watts     */
#define ADL_PMLOG_TEMPERATURE_GFX      28   /* Celsius   */
#define ADL_PMLOG_TEMPERATURE_SOC      29   /* Celsius   */
#define ADL_PMLOG_GFX_POWER            30   /* Watts     */
#define ADL_PMLOG_TEMPERATURE_CPU      32   /* Celsius   */
#define ADL_PMLOG_CPU_POWER            33   /* Watts     */
#define ADL_PMLOG_CLK_CPUCLK           34   /* MHz       */
#define ADL_PMLOG_SSPAIRED_ASICPOWER   46   /* Watts (APU) */
#define ADL_PMLOG_BOARD_POWER          73   /* Watts     */

typedef struct {
    int supported;
    int value;
} ADLSingleSensorData;

typedef struct {
    int size;
    ADLSingleSensorData sensors[ADL_PMLOG_MAX_SENSORS];
} ADLPMLogDataOutput;                          /* 4 + 256*8 = 2052 bytes */

/* ------------------------------------------------------------------
 *  Function pointer typedefs (__stdcall matches the DLL exports)
 * ------------------------------------------------------------------ */

typedef int (__stdcall *PFN_CREATE) (ADL_MALLOC_CB, int);
typedef int (__stdcall *PFN_DESTROY)(void);
typedef int (__stdcall *PFN_NUMADAPTERS)(int*);
typedef int (__stdcall *PFN_ADAPTERINFO)(ADLAdapterInfo*, int);

/* ADL2 control */
typedef int (__stdcall *PFN_CREATE2) (ADL_MALLOC_CB, int, ADL_CONTEXT_HANDLE*);
typedef int (__stdcall *PFN_DESTROY2)(ADL_CONTEXT_HANDLE);

/* Overdrive5 (legacy, global context) */
typedef int (__stdcall *PFN_OD5_TEMP)  (int, int, ADLOD5Temp*);
typedef int (__stdcall *PFN_OD5_FAN)   (int, int, ADLOD5Fan*);
typedef int (__stdcall *PFN_OD5_ACT)   (int, ADLOD5Activity*);

/* OverdriveN (ADL2 only) */
typedef int (__stdcall *PFN_ODN_TEMP2) (ADL_CONTEXT_HANDLE, int, int, int*);
typedef int (__stdcall *PFN_ODN_PERF2) (ADL_CONTEXT_HANDLE, int, ADLODNPerfStatus*);

/* Overdrive6 (also has a plain ADL variant) */
typedef int (__stdcall *PFN_OD6_TEMP)  (int, int*);

/* memory */
typedef int (__stdcall *PFN_MEMORY_INFO)  (int, ADLMemoryInfo*);
typedef int (__stdcall *PFN_MEMORY_INFO2) (int, ADLMemoryInfo2*);

/* ADL2 memory usage -- returned value is in megabytes */
typedef int (__stdcall *PFN_VRAM_USAGE2)          (ADL_CONTEXT_HANDLE, int, int*);
typedef int (__stdcall *PFN_DEDICATED_VRAM_USAGE2)(ADL_CONTEXT_HANDLE, int, int*);
typedef int (__stdcall *PFN_SHARED_VRAM_USAGE2)   (ADL_CONTEXT_HANDLE, int, int*);

/* PMLog snapshot -- ADL2_New_QueryPMLogData_Get */
typedef int (__stdcall *PFN_PMLOG_QUERY)(ADL_CONTEXT_HANDLE, int, ADLPMLogDataOutput*);

/* ------------------------------------------------------------------
 *  Static state
 * ------------------------------------------------------------------ */

static HMODULE g_hDll     = NULL;
static int     g_init_ok  = 0;

static ADL_CONTEXT_HANDLE g_adl2_ctx = NULL;

static PFN_CREATE       pfn_Create      = NULL;
static PFN_DESTROY      pfn_Destroy     = NULL;
static PFN_CREATE2      pfn_Create2     = NULL;
static PFN_DESTROY2     pfn_Destroy2    = NULL;
static PFN_NUMADAPTERS  pfn_NumAdapters = NULL;
static PFN_ADAPTERINFO  pfn_AdapterInfo = NULL;

static PFN_OD5_TEMP     pfn_OD5_Temp    = NULL;
static PFN_OD5_FAN      pfn_OD5_Fan     = NULL;
static PFN_OD5_ACT      pfn_OD5_Act     = NULL;

static PFN_ODN_TEMP2    pfn_ODN_Temp2   = NULL;
static PFN_ODN_PERF2    pfn_ODN_Perf2   = NULL;

static PFN_OD6_TEMP     pfn_OD6_Temp    = NULL;

static PFN_MEMORY_INFO  pfn_MemoryInfo  = NULL;
static PFN_MEMORY_INFO2 pfn_MemoryInfo2 = NULL;

static PFN_VRAM_USAGE2           pfn_VramUsage2           = NULL;
static PFN_DEDICATED_VRAM_USAGE2 pfn_DedicatedVramUsage2  = NULL;
static PFN_SHARED_VRAM_USAGE2    pfn_SharedVramUsage2     = NULL;

static PFN_PMLOG_QUERY  pfn_PMLogQuery = NULL;

static int              g_num_adapters  = 0;
static ADLAdapterInfo  *g_adapters      = NULL;

/* ------------------------------------------------------------------
 *  Helpers
 * ------------------------------------------------------------------ */

static void* __stdcall adl_malloc(int size) { return malloc((size_t)size); }

#define GET(ptr, name) \
    (ptr) = (void*)GetProcAddress(g_hDll, name)

#define REQUIRED(ptr, name) do { \
    GET(ptr, name); \
    if (!(ptr)) { fprintf(stderr, "ADL: missing %s\n", name); goto fail; } \
} while (0)

static int str_contains_nocase(const char *haystack, const char *needle)
{
    size_t n = strlen(needle);
    if (!n) return 0;
    for (; *haystack; haystack++) {
        size_t i;
        for (i = 0; i < n; i++)
            if (tolower((unsigned char)haystack[i]) !=
                tolower((unsigned char)needle[i])) break;
        if (i == n) return 1;
    }
    return 0;
}

/* ------------------------------------------------------------------
 *  adl_init
 * ------------------------------------------------------------------ */

int adl_init(void)
{
    int i, r;

    if (g_init_ok > 0)  return 0;
    if (g_init_ok < 0) return -1;

    g_hDll = LoadLibraryA("atiadlxx.dll");
    if (!g_hDll) g_hDll = LoadLibraryA("atiadlxy.dll");
    if (!g_hDll) { g_init_ok = -1; return -1; }

    /* global control */
    REQUIRED(pfn_Create,      "ADL_Main_Control_Create");
    REQUIRED(pfn_Destroy,     "ADL_Main_Control_Destroy");
    REQUIRED(pfn_NumAdapters, "ADL_Adapter_NumberOfAdapters_Get");
    REQUIRED(pfn_AdapterInfo, "ADL_Adapter_AdapterInfo_Get");

    /* ADL2 control (optional but strongly preferred) */
    GET(pfn_Create2,  "ADL2_Main_Control_Create");
    GET(pfn_Destroy2, "ADL2_Main_Control_Destroy");

    /* Overdrive5 (legacy) */
    GET(pfn_OD5_Temp, "ADL_Overdrive5_Temperature_Get");
    GET(pfn_OD5_Fan,  "ADL_Overdrive5_FanSpeed_Get");
    GET(pfn_OD5_Act,  "ADL_Overdrive5_CurrentActivity_Get");

    /* OverdriveN (modern, ADL2 only) */
    GET(pfn_ODN_Temp2, "ADL2_OverdriveN_Temperature_Get");
    GET(pfn_ODN_Perf2, "ADL2_OverdriveN_PerformanceStatus_Get");

    /* Overdrive6 (has a plain ADL variant) */
    GET(pfn_OD6_Temp, "ADL_Overdrive6_Temperature_Get");

    /* memory information */
    GET(pfn_MemoryInfo,  "ADL_Adapter_MemoryInfo_Get");
    GET(pfn_MemoryInfo2, "ADL_Adapter_MemoryInfo2_Get");

    /* ADL2 memory usage -- returned in megabytes */
    GET(pfn_VramUsage2,           "ADL2_Adapter_VRAMUsage_Get");
    GET(pfn_DedicatedVramUsage2,  "ADL2_Adapter_DedicatedVRAMUsage_Get");
    GET(pfn_SharedVramUsage2,     "ADL2_Adapter_SharedVRAMUsage_Get");

    /* PMLog snapshot -- the modern telemetry path for iGPUs/APUs */
    GET(pfn_PMLogQuery, "ADL2_New_QueryPMLogData_Get");

    /* create ADL2 context first (needed by modern functions) */
    if (pfn_Create2) {
        r = pfn_Create2(adl_malloc, 1, &g_adl2_ctx);
        if (r != ADL_OK) {
            fprintf(stderr, "ADL: ADL2_Main_Control_Create failed (%d)\n", r);
            g_adl2_ctx = NULL;
        }
    }

    /* create global ADL context (needed by legacy functions) */
    r = pfn_Create(adl_malloc, 1);
    if (r != ADL_OK) {
        fprintf(stderr, "ADL: Create failed (%d)\n", r);
        goto fail;
    }

    /* enumerate adapters */
    r = pfn_NumAdapters(&g_num_adapters);
    if (r != ADL_OK || g_num_adapters <= 0) {
        fprintf(stderr, "ADL: no adapters\n");
        goto fail;
    }

    g_adapters = (ADLAdapterInfo*)calloc((size_t)g_num_adapters,
                                         sizeof(ADLAdapterInfo));
    if (!g_adapters) goto fail;

    for (i = 0; i < g_num_adapters; i++)
        g_adapters[i].iSize = sizeof(ADLAdapterInfo);

    r = pfn_AdapterInfo(g_adapters,
                        g_num_adapters * (int)sizeof(ADLAdapterInfo));
    if (r != ADL_OK) {
        fprintf(stderr, "ADL: AdapterInfo failed (%d)\n", r);
        free(g_adapters); g_adapters = NULL;
        goto fail;
    }

    g_init_ok = 1;
    return 0;

fail:
    if (g_adl2_ctx && pfn_Destroy2) { pfn_Destroy2(g_adl2_ctx); g_adl2_ctx = NULL; }
    if (g_hDll) { FreeLibrary(g_hDll); g_hDll = NULL; }
    g_init_ok = -1;
    return -1;
}

/* ------------------------------------------------------------------
 *  adl_shutdown
 * ------------------------------------------------------------------ */

void adl_shutdown(void)
{
    if (g_adapters)  { free(g_adapters); g_adapters = NULL; }
    if (g_adl2_ctx && pfn_Destroy2) {
        pfn_Destroy2(g_adl2_ctx);
        g_adl2_ctx = NULL;
    }
    if (g_hDll) {
        if (pfn_Destroy) pfn_Destroy();
        FreeLibrary(g_hDll);
        g_hDll = NULL;
    }
    g_num_adapters = 0;
    g_init_ok = 0;
}

/* ------------------------------------------------------------------
 *  adl_find_by_name
 * ------------------------------------------------------------------ */

int adl_find_by_name(const char *name, int *adl_index)
{
    int i;
    if (g_init_ok <= 0 || !g_adapters) return -1;
    for (i = 0; i < g_num_adapters; i++) {
        if (str_contains_nocase(g_adapters[i].strAdapterName, name)) {
            *adl_index = i;
            return 0;
        }
    }
    return -1;
}

/* ------------------------------------------------------------------
 *  adl_find_by_pci
 * ------------------------------------------------------------------ */

int adl_find_by_pci(uint32_t vendor_id, uint32_t device_id, int *adl_index)
{
    int i;
    if (g_init_ok <= 0 || !g_adapters) return -1;
    for (i = 0; i < g_num_adapters; i++) {
        if ((uint32_t)g_adapters[i].iVendorID != vendor_id)
            continue;
        /* AdapterInfo has no device-ID field, but the UDID embeds it
         * (e.g. "PCI_VEN_1002&DEV_15BF...").  Compare it when parseable
         * so an iGPU and a dGPU from the same vendor aren't confused. */
        {
            const char *dev = strstr(g_adapters[i].strUDID, "DEV_");
            if (dev) {
                unsigned parsed = 0;
                if (sscanf(dev + 4, "%4x", &parsed) == 1 &&
                    parsed != device_id)
                    continue;
            }
        }
        *adl_index = i;
        return 0;
    }
    return -1;
}

/* ------------------------------------------------------------------
 *  adl_find_by_pci_topology  --  match by PCI bus/device/function
 *
 *  AdapterInfo has no domain field, so the domain argument is accepted
 *  but ignored. Matching on bus/device/function is what lets two
 *  identical AMD GPUs (same vendor+device) be told apart; falling back
 *  to adl_find_by_pci would otherwise always return adapter 0.
 * ------------------------------------------------------------------ */

int adl_find_by_pci_topology(uint32_t domain, uint32_t bus,
                             uint32_t device, uint32_t function,
                             int *adl_index)
{
    int i;
    (void)domain;  /* ADL AdapterInfo does not expose the PCI domain */
    if (g_init_ok <= 0 || !g_adapters) return -1;

    for (i = 0; i < g_num_adapters; i++) {
        if ((uint32_t)g_adapters[i].iBusNumber      == bus &&
            (uint32_t)g_adapters[i].iDeviceNumber   == device &&
            (uint32_t)g_adapters[i].iFunctionNumber == function) {
            *adl_index = i;
            return 0;
        }
    }
    return -1;
}

/* ------------------------------------------------------------------
 *  PMLog snapshot  --  ADL2_New_QueryPMLogData_Get
 *
 *  Returns 0 and fills *out on success, -1 on failure.
 *  The caller owns the output; pass a stack/heap ADLPMLogDataOutput.
 *  This is the path that actually works on AMD iGPUs/APUs.
 * ------------------------------------------------------------------ */

int adl_get_pmlog(int adapter_index, void *out)
{
    ADLPMLogDataOutput *pm = (ADLPMLogDataOutput*)out;
    if (g_init_ok <= 0 || !g_adl2_ctx || !pfn_PMLogQuery)
        return -1;
    memset(pm, 0, sizeof(*pm));
    pm->size = (int)sizeof(*pm);
    if (pfn_PMLogQuery(g_adl2_ctx, adapter_index, pm) != ADL_OK)
        return -1;
    return 0;
}

/* adl_get_pmlog_power -- best available power reading in milliwatts.
 * PMLog power sensors return integer Watts; convert to mW (*1000). */
int adl_get_pmlog_power(int adapter_index, int *power_milliwatts)
{
    ADLPMLogDataOutput pm;
    if (adl_get_pmlog(adapter_index, &pm) != 0)
        return -1;
    /* prefer ASIC_POWER (whole-package), then GFX_POWER, then APU_POWER */
    if (pm.sensors[ADL_PMLOG_ASIC_POWER].supported) {
        *power_milliwatts = pm.sensors[ADL_PMLOG_ASIC_POWER].value * 1000;
        return 0;
    }
    if (pm.sensors[ADL_PMLOG_GFX_POWER].supported) {
        *power_milliwatts = pm.sensors[ADL_PMLOG_GFX_POWER].value * 1000;
        return 0;
    }
    if (pm.sensors[ADL_PMLOG_SSPAIRED_ASICPOWER].supported) {
        *power_milliwatts = pm.sensors[ADL_PMLOG_SSPAIRED_ASICPOWER].value * 1000;
        return 0;
    }
    return -1;
}

/* adl_get_pmlog_fan -- fan RPM and percent from PMLog.
 * iGPUs/APUs typically have no fan sensor (returns -1). */
int adl_get_pmlog_fan(int adapter_index, int *rpm, int *percent)
{
    ADLPMLogDataOutput pm;
    int got = 0;
    if (adl_get_pmlog(adapter_index, &pm) != 0)
        return -1;
    *rpm = 0; *percent = 0;
    if (pm.sensors[ADL_PMLOG_FAN_RPM].supported) {
        *rpm = pm.sensors[ADL_PMLOG_FAN_RPM].value;
        got = 1;
    }
    if (pm.sensors[ADL_PMLOG_FAN_PERCENTAGE].supported) {
        *percent = pm.sensors[ADL_PMLOG_FAN_PERCENTAGE].value;
        got = 1;
    }
    return got ? 0 : -1;
}

/* ------------------------------------------------------------------
 *  Query helpers  --  try OverdriveN first, fall back to Overdrive5/6
 * ------------------------------------------------------------------ */

int adl_get_temperature(int idx, int *milli_c)
{
    /* try PMLog first -- the path that works on iGPUs/APUs.
     * PMLog returns integer Celsius; convert to millidegrees. */
    if (g_adl2_ctx && pfn_PMLogQuery) {
        ADLPMLogDataOutput pm;
        if (adl_get_pmlog(idx, &pm) == 0) {
            /* prefer TEMPERATURE_GFX (sensor 28), fall back to SOC (29) */
            if (pm.sensors[ADL_PMLOG_TEMPERATURE_GFX].supported &&
                pm.sensors[ADL_PMLOG_TEMPERATURE_GFX].value > 0) {
                *milli_c = pm.sensors[ADL_PMLOG_TEMPERATURE_GFX].value * 1000;
                return 0;
            }
            if (pm.sensors[ADL_PMLOG_TEMPERATURE_SOC].supported &&
                pm.sensors[ADL_PMLOG_TEMPERATURE_SOC].value > 0) {
                *milli_c = pm.sensors[ADL_PMLOG_TEMPERATURE_SOC].value * 1000;
                return 0;
            }
        }
    }

    /* try OverdriveN (ADL2) -- returned value is millidegrees C */
    if (g_adl2_ctx && pfn_ODN_Temp2) {
        int t = 0;
        if (pfn_ODN_Temp2(g_adl2_ctx, idx, 1, &t) == ADL_OK && t > 0) {
            *milli_c = t;
            return 0;
        }
    }

    /* try Overdrive5 (thermal controller 0) */
    if (pfn_OD5_Temp) {
        ADLOD5Temp t; memset(&t, 0, sizeof(t)); t.iSize = sizeof(t);
        if (pfn_OD5_Temp(idx, 0, &t) == ADL_OK && t.iTemperature > 0) {
            *milli_c = t.iTemperature;
            return 0;
        }
    }

    /* try Overdrive6 (plain ADL) */
    if (pfn_OD6_Temp) {
        int val = 0;
        int r = pfn_OD6_Temp(idx, &val);
        if (r == ADL_OK && val > 0) {
            *milli_c = val;
            return 0;
        }
    }

    return -1;
}

int adl_get_fan_speed(int idx, int *rpm, int *percent)
{
    /* Overdrive5 fan is sufficient for the data we expose */
    if (pfn_OD5_Fan) {
        ADLOD5Fan f; memset(&f, 0, sizeof(f)); f.iSize = sizeof(f);
        if (pfn_OD5_Fan(idx, 0, &f) == ADL_OK) {
            *rpm     = f.iFanSpeed;
            *percent = f.iFanPercent;
            return 0;
        }
    }

    return -1;
}

/* adl_get_activity -- returns a bitmask of ADL_ACT_VALID_* describing
 * which output fields were actually filled, or -1 on total failure.
 * PMLog reports per-sensor "supported" flags; a missing sensor must NOT
 * be reported as 0 -- the caller uses the mask to decide which fields
 * to trust and whether to fall back to another data source. */
int adl_get_activity(int idx,
                      int *eng_clock_10khz,
                      int *mem_clock_10khz,
                      int *activity_pct,
                      int *perf_level)
{
    /* try PMLog first -- the path that works on iGPUs/APUs.
     * PMLog returns GFXCLK/MEMCLK in MHz; convert to 10 kHz (*100).
     * activity is already 0-100 percent.  perf_level is not in PMLog,
     * so its bit is never set on this path. */
    if (g_adl2_ctx && pfn_PMLogQuery) {
        ADLPMLogDataOutput pm;
        if (adl_get_pmlog(idx, &pm) == 0) {
            int mask = 0;
            if (pm.sensors[ADL_PMLOG_INFO_ACTIVITY_GFX].supported) {
                *activity_pct = pm.sensors[ADL_PMLOG_INFO_ACTIVITY_GFX].value;
                mask |= ADL_ACT_VALID_ACTIVITY;
            }
            if (pm.sensors[ADL_PMLOG_CLK_GFXCLK].supported) {
                *eng_clock_10khz = pm.sensors[ADL_PMLOG_CLK_GFXCLK].value * 100;
                mask |= ADL_ACT_VALID_ENG_CLOCK;
            }
            if (pm.sensors[ADL_PMLOG_CLK_MEMCLK].supported) {
                *mem_clock_10khz = pm.sensors[ADL_PMLOG_CLK_MEMCLK].value * 100;
                mask |= ADL_ACT_VALID_MEM_CLOCK;
            }
            if (mask) return mask;
        }
    }

    /* try OverdriveN (ADL2) -- all four fields valid on success */
    if (g_adl2_ctx && pfn_ODN_Perf2) {
        ADLODNPerfStatus p; memset(&p, 0, sizeof(p));
        if (pfn_ODN_Perf2(g_adl2_ctx, idx, &p) == ADL_OK) {
            *eng_clock_10khz = p.iCoreClock;
            *mem_clock_10khz = p.iMemoryClock;
            *activity_pct    = p.iGPUActivityPercent;
            *perf_level      = p.iCurrentCorePerformanceLevel;
            return ADL_ACT_VALID_ALL;
        }
    }

    /* try Overdrive5 */
    if (pfn_OD5_Act) {
        ADLOD5Activity a; memset(&a, 0, sizeof(a)); a.iSize = sizeof(a);
        if (pfn_OD5_Act(idx, &a) == ADL_OK) {
            *eng_clock_10khz = a.iEngineClock;
            *mem_clock_10khz = a.iMemoryClock;
            *activity_pct    = a.iActivityPercent;
            *perf_level      = a.iCurrentPerformanceLevel;
            return ADL_ACT_VALID_ALL;
        }
    }

    return -1;
}

/* adl_get_memory -- returns a bitmask of ADL_MEM_VALID_* describing
 * which output fields were actually filled, or -1 on total failure.
 * An output is only written when its bit is set, so the caller never
 * mistakes "unknown" for 0 bytes. */
int adl_get_memory(int idx, uint64_t *used_bytes, uint64_t *total_bytes)
{
    int mask = 0;

    /* total VRAM from ADL_Adapter_MemoryInfo_Get */
    if (pfn_MemoryInfo) {
        ADLMemoryInfo mi; memset(&mi, 0, sizeof(mi));
        if (pfn_MemoryInfo(idx, &mi) == ADL_OK && mi.iMemorySize > 0) {
            *total_bytes = (uint64_t)mi.iMemorySize;
            mask |= ADL_MEM_VALID_TOTAL;
        }
    }

    /* fallback total for APUs / newer drivers */
    if (!(mask & ADL_MEM_VALID_TOTAL) && pfn_MemoryInfo2) {
        ADLMemoryInfo2 mi2; memset(&mi2, 0, sizeof(mi2));
        if (pfn_MemoryInfo2(idx, &mi2) == ADL_OK && mi2.iMemorySize > 0) {
            *total_bytes = (uint64_t)mi2.iMemorySize;
            mask |= ADL_MEM_VALID_TOTAL;
        }
    }

    if (!g_adl2_ctx)
        return mask ? mask : -1;

    /* ADL2 usage -- functions return megabytes.
     * Prefer dedicated + shared; fall back to the aggregate value. */
    {
        int ded_mb = 0, shared_mb = 0, total_mb = 0;
        int r1 = -1, r2 = -1, r3 = -1;

        if (pfn_DedicatedVramUsage2)
            r1 = pfn_DedicatedVramUsage2(g_adl2_ctx, idx, &ded_mb);
        if (pfn_SharedVramUsage2)
            r2 = pfn_SharedVramUsage2(g_adl2_ctx, idx, &shared_mb);

        if ((r1 == ADL_OK && ded_mb >= 0) || (r2 == ADL_OK && shared_mb >= 0)) {
            int sum = 0;
            if (r1 == ADL_OK) sum += ded_mb;
            if (r2 == ADL_OK) sum += shared_mb;
            *used_bytes = (uint64_t)sum * 1024ULL * 1024ULL;
            mask |= ADL_MEM_VALID_USED;
        } else if (pfn_VramUsage2 &&
                   (r3 = pfn_VramUsage2(g_adl2_ctx, idx, &total_mb)) == ADL_OK &&
                   total_mb >= 0) {
            *used_bytes = (uint64_t)total_mb * 1024ULL * 1024ULL;
            mask |= ADL_MEM_VALID_USED;
        }
    }

    return mask ? mask : -1;
}

#endif /* _WIN32 */
