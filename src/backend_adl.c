/* ================================================================
 *  backend_adl.c  --  AMD Display Library (atiadlxx.dll) dynamic backend
 *
 *  Loads ADL at runtime via LoadLibrary.  If the DLL is not present
 *  (e.g. no AMD driver installed) the whole backend degrades gracefully.
 *
 *  Queries dynamic GPU info using OverdriveN (modern) with automatic
 *  fallback to Overdrive5 (legacy).
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
 *  Internal structures
 * ------------------------------------------------------------------ */

#define ADL_OK                    0

typedef void* (*ADL_MALLOC_CB)(int);

/* AdapterInfo -- layout from AMD Adrenalin 32.x+ (strAdapterName at 280) */
typedef struct {
    int  iSize;                   /*   0 */
    int  iAdapterIndex;           /*   4 */
    char strUDID[256];            /*   8 */
    int  iPresent;                /* 264 */
    int  iExist;                  /* 268 */
    int  iVendorID;               /* 272 */
    int  iDeviceID;               /* 276 */
    char strAdapterName[256];     /* 280 */
    char _pad[1488];
} ADLAdapterInfo;

/* Overdrive5 (legacy) */
typedef struct { int iSize; int iTemperature; } ADLOD5Temp;
typedef struct { int iSize; int iFanSpeed; int iFanPercent; int _p[2]; } ADLOD5Fan;
typedef struct { int iSize; int iEngineClock; int iMemoryClock;
                 int iVddc; int iActivityPercent;
                 int iCurrentPerformanceLevel;
                 int iCurrentBusSpeed; int iCurrentBusLanes; } ADLOD5Activity;

/* OverdriveN (modern) -- generous padding to survive struct growth */

/* ADLODNTemperature: used by ADL2_OverdriveN_Temperature_Get.
 * iTemperatureType[0] = input (which sensor, e.g. 1=Edge)
 * iTemperatureValue[0] = output (millidegrees C) */
typedef struct {
    int iSize;
    int iTemperatureType[8];
    int iTemperatureValue[8];
    int iTemperatureFlags[8];
} ADLODNTemp;

typedef struct { int iSize; int iCoreClock; int iMemoryClock;
                 int iGPUActivityPercent; int iCurrentCorePerformanceLevel;
                 int _pad[64]; } ADLODNPerfStatus;

typedef struct { int iSize; int iFanSpeedPercent; int iFanSpeedRPM;
                 int _pad[32]; } ADLODNFan;

/* AdapterSpeed -- from ADL2_Adapter_Speed_Get (works on APUs) */
typedef struct { int iSize; int iCoreClock; int iMemoryClock;
                 int _pad[32]; } ADLAdapterSpeed;

/* ------------------------------------------------------------------
 *  Function pointer typedefs
 * ------------------------------------------------------------------ */

typedef int (*PFN_CREATE) (ADL_MALLOC_CB, int);
typedef int (*PFN_DESTROY)(void);
typedef int (*PFN_NUMADAPTERS)(int*);
typedef int (*PFN_ADAPTERINFO)(ADLAdapterInfo*, int);

/* Overdrive5 */
typedef int (*PFN_OD5_TEMP)  (int, int, ADLOD5Temp*);
typedef int (*PFN_OD5_FAN)   (int, int, ADLOD5Fan*);
typedef int (*PFN_OD5_ACT)   (int, ADLOD5Activity*);

/* OverdriveN */
typedef int (*PFN_ODN_TEMP)  (int, ADLODNTemp*);
typedef int (*PFN_ODN_PERF)  (int, ADLODNPerfStatus*);
typedef int (*PFN_ODN_FAN)   (int, ADLODNFan*);

typedef int (*PFN_ADAPTER_SPEED)(int, ADLAdapterSpeed*);

/* ------------------------------------------------------------------
 *  Static state
 * ------------------------------------------------------------------ */

static HMODULE g_hDll     = NULL;
static int     g_init_ok  = 0;

static PFN_CREATE       pfn_Create      = NULL;
static PFN_DESTROY      pfn_Destroy     = NULL;
static PFN_NUMADAPTERS  pfn_NumAdapters = NULL;
static PFN_ADAPTERINFO  pfn_AdapterInfo = NULL;

static PFN_OD5_TEMP     pfn_OD5_Temp    = NULL;
static PFN_OD5_FAN      pfn_OD5_Fan     = NULL;
static PFN_OD5_ACT      pfn_OD5_Act     = NULL;

static PFN_ODN_TEMP     pfn_ODN_Temp    = NULL;
static PFN_ODN_PERF     pfn_ODN_Perf    = NULL;
static PFN_ODN_FAN      pfn_ODN_Fan     = NULL;

/* Overdrive6 has a different signature: int func(int idx, int *out) */
typedef int (*PFN_OD6_TEMP)(int, int*);
static PFN_OD6_TEMP     pfn_OD6_Temp     = NULL;

static PFN_ADAPTER_SPEED pfn_AdapterSpeed = NULL;

static int              g_num_adapters  = 0;
static ADLAdapterInfo  *g_adapters      = NULL;

/* ------------------------------------------------------------------
 *  Helpers
 * ------------------------------------------------------------------ */

static void* adl_malloc(int size) { return malloc((size_t)size); }

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

    if (g_init_ok)  return 0;
    if (g_init_ok < 0) return -1;

    g_hDll = LoadLibraryA("atiadlxx.dll");
    if (!g_hDll) g_hDll = LoadLibraryA("atiadlxy.dll");
    if (!g_hDll) { g_init_ok = -1; return -1; }

    REQUIRED(pfn_Create,      "ADL_Main_Control_Create");
    REQUIRED(pfn_Destroy,     "ADL_Main_Control_Destroy");
    REQUIRED(pfn_NumAdapters, "ADL_Adapter_NumberOfAdapters_Get");
    REQUIRED(pfn_AdapterInfo, "ADL_Adapter_AdapterInfo_Get");

    /* optional: Overdrive5 (may not exist on very old drivers) */
    GET(pfn_OD5_Temp, "ADL_Overdrive5_Temperature_Get");
    GET(pfn_OD5_Fan,  "ADL_Overdrive5_FanSpeed_Get");
    GET(pfn_OD5_Act,  "ADL_Overdrive5_CurrentActivity_Get");

    /* optional: OverdriveN (present on Adrenalin 2019+) */
    GET(pfn_ODN_Temp, "ADL2_OverdriveN_Temperature_Get");
    GET(pfn_ODN_Perf, "ADL2_OverdriveN_PerformanceStatus_Get");
    GET(pfn_ODN_Fan,  "ADL2_OverdriveN_FanControl_Get");

    /* also try Overdrive6 (some APU integrated graphics use this) */
    GET(pfn_OD6_Temp, "ADL2_Overdrive6_Temperature_Get");

    /* Adapter Speed (clock frequencies) -- works even on APUs */
    GET(pfn_AdapterSpeed, "ADL2_Adapter_Speed_Get");

    /* create ADL context */
    r = pfn_Create(adl_malloc, 1);
    if (r != ADL_OK) { fprintf(stderr, "ADL: Create failed (%d)\n", r); goto fail; }

    /* enumerate adapters */
    r = pfn_NumAdapters(&g_num_adapters);
    if (r != ADL_OK || g_num_adapters <= 0) {
        fprintf(stderr, "ADL: no adapters\n"); goto fail;
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
        if ((uint32_t)g_adapters[i].iVendorID  == vendor_id &&
            (uint32_t)g_adapters[i].iDeviceID  == device_id) {
            *adl_index = i;
            return 0;
        }
    }
    return -1;
}

/* ------------------------------------------------------------------
 *  Query helpers  --  try OverdriveN first, fall back to Overdrive5
 * ------------------------------------------------------------------ */

int adl_get_temperature(int idx, int *milli_c)
{
    /* try OverdriveN */
    if (pfn_ODN_Temp) {
        ADLODNTemp t; memset(&t, 0, sizeof(t)); t.iSize = sizeof(t);
        t.iTemperatureType[0] = 1; /* 1 = GPU edge */
        int r = pfn_ODN_Temp(idx, &t);
        if (r == ADL_OK && t.iTemperatureValue[0] > 0) {
            *milli_c = t.iTemperatureValue[0];
            return 0;
        }
    }

    /* try Overdrive5 (thermal controller 0) */
    if (pfn_OD5_Temp) {
        ADLOD5Temp t; memset(&t, 0, sizeof(t)); t.iSize = sizeof(t);
        int r = pfn_OD5_Temp(idx, 0, &t);
        if (r == ADL_OK && t.iTemperature > 0) {
            *milli_c = t.iTemperature;
            return 0;
        }
    }

    /* try Overdrive6 (simpler signature: idx, &temp) */
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
    /* try OverdriveN */
    if (pfn_ODN_Fan) {
        ADLODNFan f; f.iSize = sizeof(f);
        if (pfn_ODN_Fan(idx, &f) == ADL_OK) {
            *rpm     = f.iFanSpeedRPM;
            *percent = f.iFanSpeedPercent;
            return 0;
        }
    }

    /* try Overdrive5 */
    if (pfn_OD5_Fan) {
        ADLOD5Fan f; f.iSize = sizeof(f);
        if (pfn_OD5_Fan(idx, 0, &f) == ADL_OK) {
            *rpm     = f.iFanSpeed;
            *percent = f.iFanPercent;
            return 0;
        }
    }

    return -1;
}

int adl_get_activity(int idx,
                      int *eng_clock_10khz,
                      int *mem_clock_10khz,
                      int *activity_pct,
                      int *perf_level)
{
    /* try OverdriveN */
    if (pfn_ODN_Perf) {
        ADLODNPerfStatus p; memset(&p, 0, sizeof(p)); p.iSize = sizeof(p);
        int r = pfn_ODN_Perf(idx, &p);
        if (r == ADL_OK) {
            *eng_clock_10khz = p.iCoreClock;
            *mem_clock_10khz = p.iMemoryClock;
            *activity_pct    = p.iGPUActivityPercent;
            *perf_level      = p.iCurrentCorePerformanceLevel;
            return 0;
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
            return 0;
        }
    }

    return -1;
}

int adl_get_speed(int idx, int *core_mhz, int *mem_mhz)
{
    if (pfn_AdapterSpeed) {
        ADLAdapterSpeed s; memset(&s, 0, sizeof(s)); s.iSize = sizeof(s);
        int r = pfn_AdapterSpeed(idx, &s);
        if (r == ADL_OK) {
            *core_mhz = s.iCoreClock;
            *mem_mhz  = s.iMemoryClock;
            return 0;
        }
    }
    return -1;
}

#endif /* _WIN32 */
