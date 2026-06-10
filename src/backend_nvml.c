/* ================================================================
 *  backend_nvml.c  --  NVIDIA Management Library dynamic backend
 *
 *  Loads nvml.dll (Windows) or libnvidia-ml.so (Linux) at runtime
 *  and queries GPU temperature, utilisation, clocks, power,
 *  memory, fan, P-State and ECC status.
 *
 *  Entirely linker-free: no import library needed.
 * ================================================================ */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <windows.h>
#else
#include <dlfcn.h>
#endif

#include "backend_nvml.h"

/* ------------------------------------------------------------------
 *  NVML constants (subset of nvml.h)
 * ------------------------------------------------------------------ */

#define NVML_SUCCESS                  0
#define NVML_TEMPERATURE_GPU          0
#define NVML_CLOCK_SM                 0
#define NVML_CLOCK_MEM                1
#define NVML_MAX_DEVICES              32

typedef void* nvmlDevice_t;

typedef enum {
    NVML_ECC_CURRENT  = 1,
    NVML_ECC_PENDING  = 2
} nvmlEnableState_t;  /* 0 = disabled, 1 = enabled */

typedef struct {
    unsigned int  version;
    char          busIdLegacy[16];
    unsigned int  domain;
    unsigned int  bus;
    unsigned int  device;
    unsigned int  function;
    unsigned int  pciDeviceId;
    unsigned int  pciSubSystemId;
    char          busId[32];
    /* more fields we don't use */
} nvmlPciInfo_t;

typedef struct {
    unsigned int version;
    unsigned int gpu;
    unsigned int memory;
} nvmlUtilization_t;

typedef struct {
    unsigned int version;
    unsigned long long total;   /* bytes */
    unsigned long long free;
    unsigned long long used;
} nvmlMemory_t;

/* ------------------------------------------------------------------
 *  Function pointer typedefs
 * ------------------------------------------------------------------ */

typedef int (*PFN_nvmlInit_v2)(void);
typedef int (*PFN_nvmlShutdown)(void);
typedef int (*PFN_nvmlDeviceGetCount_v2)(unsigned int*);
typedef int (*PFN_nvmlDeviceGetHandleByIndex_v2)(unsigned int, nvmlDevice_t*);
typedef int (*PFN_nvmlDeviceGetPciInfo_v3)(nvmlDevice_t, nvmlPciInfo_t*);
typedef int (*PFN_nvmlDeviceGetTemperature)(nvmlDevice_t, int, unsigned int*);
typedef int (*PFN_nvmlDeviceGetUtilizationRates)(nvmlDevice_t, nvmlUtilization_t*);
typedef int (*PFN_nvmlDeviceGetClockInfo)(nvmlDevice_t, int, unsigned int*);
typedef int (*PFN_nvmlDeviceGetPowerUsage)(nvmlDevice_t, unsigned int*);
typedef int (*PFN_nvmlDeviceGetPowerManagementLimit)(nvmlDevice_t, unsigned int*);
typedef int (*PFN_nvmlDeviceGetMemoryInfo)(nvmlDevice_t, nvmlMemory_t*);
typedef int (*PFN_nvmlDeviceGetFanSpeed)(nvmlDevice_t, unsigned int*);
typedef int (*PFN_nvmlDeviceGetPerformanceState)(nvmlDevice_t, int*);
typedef int (*PFN_nvmlDeviceGetEccMode)(nvmlDevice_t, int*, int*);
typedef int (*PFN_nvmlSystemGetDriverVersion)(char*, unsigned int);

/* ------------------------------------------------------------------
 *  Static state
 * ------------------------------------------------------------------ */

#ifdef _WIN32
static HMODULE g_lib = NULL;
#define LOAD_LIB()   LoadLibraryA("nvml.dll")
#define GET_FN(l, n) GetProcAddress((HMODULE)(l), n)
#define FREE_LIB(l)  FreeLibrary((HMODULE)(l))
#else
static void *g_lib = NULL;
#define LOAD_LIB()   dlopen("libnvidia-ml.so", RTLD_LAZY)
#define GET_FN(l, n) dlsym((l), n)
#define FREE_LIB(l)  dlclose((l))
#endif

static int g_init_ok = 0;

static PFN_nvmlInit_v2                       pfn_Init = NULL;
static PFN_nvmlShutdown                      pfn_Shutdown = NULL;
static PFN_nvmlDeviceGetCount_v2             pfn_GetCount = NULL;
static PFN_nvmlDeviceGetHandleByIndex_v2     pfn_GetHandleByIndex = NULL;
static PFN_nvmlDeviceGetPciInfo_v3           pfn_GetPciInfo = NULL;
static PFN_nvmlDeviceGetTemperature          pfn_GetTemp = NULL;
static PFN_nvmlDeviceGetUtilizationRates     pfn_GetUtil = NULL;
static PFN_nvmlDeviceGetClockInfo            pfn_GetClock = NULL;
static PFN_nvmlDeviceGetPowerUsage           pfn_GetPower = NULL;
static PFN_nvmlDeviceGetPowerManagementLimit pfn_GetPowerLimit = NULL;
static PFN_nvmlDeviceGetMemoryInfo           pfn_GetMem = NULL;
static PFN_nvmlDeviceGetFanSpeed             pfn_GetFan = NULL;
static PFN_nvmlDeviceGetPerformanceState     pfn_GetPState = NULL;
static PFN_nvmlDeviceGetEccMode              pfn_GetEcc = NULL;
static PFN_nvmlSystemGetDriverVersion        pfn_DrvVer = NULL;

typedef struct {
    unsigned int domain;
    unsigned int bus;
    unsigned int device;
    unsigned int function;
    int          valid;
} NvmlPciInfo;

static unsigned int  g_num_devices = 0;
static nvmlDevice_t  g_devices[NVML_MAX_DEVICES];
static NvmlPciInfo   g_pci_info[NVML_MAX_DEVICES];

/* ------------------------------------------------------------------
 *  Helper macro
 * ------------------------------------------------------------------ */

#define RESOLVE(ptr, name) ((ptr) = (void*)GET_FN(g_lib, name))

/* ------------------------------------------------------------------
 *  nvml_init
 * ------------------------------------------------------------------ */

int nvml_init(void)
{
    int r;
    unsigned int i;

    if (g_init_ok)  return 0;
    if (g_init_ok < 0) return -1;

    g_lib = LOAD_LIB();
    if (!g_lib) { g_init_ok = -1; return -1; }

    if (!RESOLVE(pfn_Init, "nvmlInit_v2") &&
        !RESOLVE(pfn_Init, "nvmlInit")) {
        FREE_LIB(g_lib); g_lib = NULL; g_init_ok = -1; return -1;
    }
    if (!RESOLVE(pfn_Shutdown, "nvmlShutdown")) goto fail;
    if (!RESOLVE(pfn_GetCount, "nvmlDeviceGetCount_v2") &&
        !RESOLVE(pfn_GetCount, "nvmlDeviceGetCount")) goto fail;
    if (!RESOLVE(pfn_GetHandleByIndex, "nvmlDeviceGetHandleByIndex_v2") &&
        !RESOLVE(pfn_GetHandleByIndex, "nvmlDeviceGetHandleByIndex")) goto fail;
    /* GetPciInfo is needed for matching identical GPU models */
    RESOLVE(pfn_GetPciInfo, "nvmlDeviceGetPciInfo_v3");

    RESOLVE(pfn_GetTemp,      "nvmlDeviceGetTemperature");
    RESOLVE(pfn_GetUtil,      "nvmlDeviceGetUtilizationRates");
    RESOLVE(pfn_GetClock,     "nvmlDeviceGetClockInfo");
    RESOLVE(pfn_GetPower,     "nvmlDeviceGetPowerUsage");
    RESOLVE(pfn_GetPowerLimit,"nvmlDeviceGetPowerManagementLimit");
    RESOLVE(pfn_GetMem,       "nvmlDeviceGetMemoryInfo");
    RESOLVE(pfn_GetFan,       "nvmlDeviceGetFanSpeed");
    RESOLVE(pfn_GetPState,    "nvmlDeviceGetPerformanceState");
    RESOLVE(pfn_GetEcc,       "nvmlDeviceGetEccMode");
    RESOLVE(pfn_DrvVer,       "nvmlSystemGetDriverVersion");

    r = pfn_Init();
    if (r != NVML_SUCCESS) { goto fail; }

    r = pfn_GetCount(&g_num_devices);
    if (r != NVML_SUCCESS || g_num_devices == 0) { goto fail; }
    if (g_num_devices > NVML_MAX_DEVICES) g_num_devices = NVML_MAX_DEVICES;

    for (i = 0; i < g_num_devices; i++) {
        nvmlPciInfo_t pci = {0};
        g_devices[i] = NULL;
        r = pfn_GetHandleByIndex(i, &g_devices[i]);
        if (r != NVML_SUCCESS) continue;
        if (pfn_GetPciInfo && pfn_GetPciInfo(g_devices[i], &pci) == NVML_SUCCESS) {
            g_pci_info[i].domain   = pci.domain;
            g_pci_info[i].bus      = pci.bus;
            g_pci_info[i].device   = pci.device;
            g_pci_info[i].function = pci.function;
            g_pci_info[i].valid    = 1;
        }
    }

    g_init_ok = 1;
    return 0;

fail:
    FREE_LIB(g_lib);
    g_lib = NULL;
    g_init_ok = -1;
    return -1;
}

/* ------------------------------------------------------------------
 *  nvml_shutdown
 * ------------------------------------------------------------------ */

void nvml_shutdown(void)
{
    if (g_lib) {
        if (pfn_Shutdown) pfn_Shutdown();
        FREE_LIB(g_lib);
        g_lib = NULL;
    }
    memset(g_devices, 0, sizeof(g_devices));
    memset(g_pci_info, 0, sizeof(g_pci_info));
    g_num_devices = 0;
    g_init_ok = 0;
}

/* ------------------------------------------------------------------
 *  nvml_find_by_pci
 * ------------------------------------------------------------------ */

int nvml_find_by_pci(unsigned int domain, unsigned int bus,
                     unsigned int device, unsigned int function,
                     int *index)
{
    unsigned int i;
    if (g_init_ok <= 0) return -1;
    for (i = 0; i < g_num_devices; i++) {
        if (!g_devices[i]) continue;
        if (!g_pci_info[i].valid) continue;
        if (g_pci_info[i].domain   == domain &&
            g_pci_info[i].bus      == bus &&
            g_pci_info[i].device   == device &&
            g_pci_info[i].function == function) {
            *index = (int)i;
            return 0;
        }
    }
    return -1;
}

/* ------------------------------------------------------------------
 *  Query helpers
 * ------------------------------------------------------------------ */

#define CHECK(idx) \
    if (g_init_ok <= 0) return -1; \
    if ((idx) < 0 || (unsigned int)(idx) >= g_num_devices || !g_devices[(idx)]) return -1;

int nvml_get_temperature(int idx, int *celsius)
{
    unsigned int t;
    CHECK(idx);
    if (!pfn_GetTemp) return -1;
    if (pfn_GetTemp(g_devices[idx], NVML_TEMPERATURE_GPU, &t) != NVML_SUCCESS)
        return -1;
    *celsius = (int)t;
    return 0;
}

int nvml_get_utilization(int idx, int *gpu_pct, int *mem_pct)
{
    nvmlUtilization_t u = {0};
    CHECK(idx);
    if (!pfn_GetUtil) return -1;
    u.version = sizeof(u);
    if (pfn_GetUtil(g_devices[idx], &u) != NVML_SUCCESS) return -1;
    *gpu_pct = (int)u.gpu;
    *mem_pct = (int)u.memory;
    return 0;
}

int nvml_get_clocks(int idx, int *sm_mhz, int *mem_mhz)
{
    unsigned int c;
    int ok = 0;
    CHECK(idx);
    if (!pfn_GetClock) return -1;

    if (pfn_GetClock(g_devices[idx], NVML_CLOCK_SM, &c) == NVML_SUCCESS) {
        *sm_mhz = (int)c; ok = 1;
    }
    if (pfn_GetClock(g_devices[idx], NVML_CLOCK_MEM, &c) == NVML_SUCCESS) {
        *mem_mhz = (int)c;
    } else {
        *mem_mhz = -1;
    }
    return ok ? 0 : -1;
}

int nvml_get_power(int idx, int *usage_w, int *limit_w)
{
    unsigned int u, l;
    int ok_u = 0, ok_l = 0;
    CHECK(idx);

    if (pfn_GetPower && pfn_GetPower(g_devices[idx], &u) == NVML_SUCCESS) {
        *usage_w = (int)(u / 1000); /* mW -> W */
        ok_u = 1;
    } else {
        *usage_w = -1;
    }
    if (pfn_GetPowerLimit && pfn_GetPowerLimit(g_devices[idx], &l) == NVML_SUCCESS) {
        *limit_w = (int)(l / 1000);
        ok_l = 1;
    } else {
        *limit_w = -1;
    }
    return (ok_u || ok_l) ? 0 : -1;
}

int nvml_get_memory(int idx, unsigned int *used_mb, unsigned int *total_mb)
{
    nvmlMemory_t m = {0};
    CHECK(idx);
    if (!pfn_GetMem) return -1;
    m.version = sizeof(m);
    if (pfn_GetMem(g_devices[idx], &m) != NVML_SUCCESS) return -1;
    *used_mb  = (unsigned int)(m.used  / (1024ULL * 1024ULL));
    *total_mb = (unsigned int)(m.total / (1024ULL * 1024ULL));
    return 0;
}

int nvml_get_fan(int idx, int *percent)
{
    unsigned int f;
    CHECK(idx);
    if (!pfn_GetFan) return -1;
    if (pfn_GetFan(g_devices[idx], &f) != NVML_SUCCESS) return -1;
    *percent = (int)f;
    return 0;
}

int nvml_get_perf_state(int idx, int *pstate)
{
    int ps;
    CHECK(idx);
    if (!pfn_GetPState) return -1;
    if (pfn_GetPState(g_devices[idx], &ps) != NVML_SUCCESS) return -1;
    *pstate = ps;
    return 0;
}

int nvml_get_ecc(int idx, int *enabled)
{
    int cur, pend;
    CHECK(idx);
    if (!pfn_GetEcc) return -1;
    if (pfn_GetEcc(g_devices[idx], &cur, &pend) != NVML_SUCCESS) return -1;
    *enabled = cur;
    return 0;
}

int nvml_get_driver_version(char *buf, size_t buf_size)
{
    if (g_init_ok <= 0) return -1;
    if (!pfn_DrvVer) return -1;
    if (pfn_DrvVer(buf, (unsigned int)buf_size) != NVML_SUCCESS) return -1;
    return 0;
}
