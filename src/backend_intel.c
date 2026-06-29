/* ================================================================
 *  backend_intel.c  --  Intel Graphics Control Library (IGCL)
 *
 *  Loads ControlLib.dll at runtime via LoadLibrary.  If the DLL is
 *  not present (e.g. no Intel driver installed) the whole backend
 *  degrades gracefully.
 *
 *  Covers Intel Arc discrete GPUs and Iris Xe / UHD integrated
 *  graphics.  Telemetry APIs (temperature, utilisation, clocks,
 *  power, memory, fan) require the Level Zero backend and a 64-bit
 *  process, both of which are requested in ctlInit.
 *
 *  Utilisation and power are derived from two-sample deltas of
 *  monotonic counters (engine active-time and energy counter).  A
 *  baseline sample is taken during intel_init so that the first
 *  query after init already yields a valid reading; each query then
 *  refreshes the baseline for the next call.
 * ================================================================ */

#ifdef _WIN32

#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>

#include "backend_intel.h"

/* ------------------------------------------------------------------
 *  IGCL constants
 * ------------------------------------------------------------------ */

#define CTL_RESULT_SUCCESS              0x00000000u
#define CTL_INIT_FLAG_USE_LEVEL_ZERO    (1u << 0)
#define CTL_MAX_DEVICES                 32

#define CTL_MAKE_VERSION(maj, min) (((uint32_t)(maj) << 16) | ((uint32_t)(min) & 0xFFFF))

/* Enum values used by the APIs we call */
typedef enum {
    CTL_DEVICE_TYPE_GRAPHICS = 1,
    CTL_DEVICE_TYPE_SYSTEM   = 2
} ctl_device_type_t;

typedef enum {
    CTL_TEMP_SENSORS_GLOBAL = 0,
    CTL_TEMP_SENSORS_GPU    = 1,
    CTL_TEMP_SENSORS_MEMORY = 2
} ctl_temp_sensors_t;

typedef enum {
    CTL_ENGINE_GROUP_GT     = 0,
    CTL_ENGINE_GROUP_RENDER = 1,
    CTL_ENGINE_GROUP_MEDIA  = 2
} ctl_engine_group_t;

typedef enum {
    CTL_FREQ_DOMAIN_GPU    = 0,
    CTL_FREQ_DOMAIN_MEMORY = 1
} ctl_freq_domain_t;

typedef enum {
    CTL_FAN_SPEED_UNITS_RPM     = 0,
    CTL_FAN_SPEED_UNITS_PERCENT = 1
} ctl_fan_speed_units_t;

/* ------------------------------------------------------------------
 *  IGCL structures -- field order/size must match igcl_api.h exactly
 *  (MSVC default alignment, no packing).  Only fields we read are
 *  fully typed; trailing reserved fields are kept for correct sizeof.
 * ------------------------------------------------------------------ */

typedef struct _ctl_api_handle_t            *ctl_api_handle_t;
typedef struct _ctl_device_adapter_handle_t *ctl_device_adapter_handle_t;
typedef struct _ctl_temp_handle_t           *ctl_temp_handle_t;
typedef struct _ctl_freq_handle_t           *ctl_freq_handle_t;
typedef struct _ctl_pwr_handle_t            *ctl_pwr_handle_t;
typedef struct _ctl_mem_handle_t            *ctl_mem_handle_t;
typedef struct _ctl_engine_handle_t         *ctl_engine_handle_t;
typedef struct _ctl_fan_handle_t            *ctl_fan_handle_t;

typedef struct {
    uint32_t Data1;
    uint16_t Data2;
    uint16_t Data3;
    uint8_t  Data4[8];
} ctl_application_id_t;  /* 16 bytes */

typedef struct {
    uint32_t             Size;             /* in  */
    uint8_t              Version;          /* in  */
    uint32_t             AppVersion;       /* in  */
    uint32_t             flags;            /* in  */
    uint32_t             SupportedVersion; /* out */
    ctl_application_id_t ApplicationUID;   /* in  */
} ctl_init_args_t;

typedef struct {
    uint8_t bus;
    uint8_t device;
    uint8_t function;
} ctl_adapter_bdf_t;

typedef struct {
    uint64_t major_version;
    uint64_t minor_version;
    uint64_t build_number;
} ctl_firmware_version_t;  /* 24 bytes */

typedef struct {
    uint32_t               Size;                        /* in  */
    uint8_t                Version;                    /* in  */
    void*                  pDeviceID;                  /* in  -- LUID buffer on Windows */
    uint32_t               device_id_size;             /* in  */
    ctl_device_type_t      device_type;                /* out */
    uint32_t               supported_subfunction_flags;/* out */
    uint64_t               driver_version;             /* out */
    ctl_firmware_version_t firmware_version;           /* out */
    uint32_t               pci_vendor_id;              /* out */
    uint32_t               pci_device_id;              /* out */
    uint32_t               rev_id;                     /* out */
    uint32_t               num_eus_per_sub_slice;      /* out */
    uint32_t               num_sub_slices_per_slice;   /* out */
    uint32_t               num_slices;                 /* out */
    char                   name[100];                  /* out */
    uint32_t               graphics_adapter_properties;/* out */
    uint32_t               Frequency;                  /* out */
    uint16_t               pci_subsys_id;              /* out */
    uint16_t               pci_subsys_vendor_id;       /* out */
    ctl_adapter_bdf_t      adapter_bdf;                /* out */
    uint32_t               num_xe_cores;               /* out */
    char                   reserved[108];              /* out */
} ctl_device_adapter_properties_t;

typedef struct {
    uint32_t           Size;
    uint8_t            Version;
    ctl_temp_sensors_t type;
    double             maxTemperature;
} ctl_temp_properties_t;

typedef struct {
    uint32_t           Size;
    uint8_t            Version;
    ctl_engine_group_t type;
} ctl_engine_properties_t;

typedef struct {
    uint32_t Size;
    uint8_t  Version;
    uint64_t activeTime;   /* microseconds, monotonic */
    uint64_t timestamp;    /* microseconds */
} ctl_engine_stats_t;

typedef struct {
    uint32_t          Size;
    uint8_t           Version;
    ctl_freq_domain_t type;
    bool              canControl;
    double            min;
    double            max;
} ctl_freq_properties_t;

typedef struct {
    uint32_t Size;
    uint8_t  Version;
    double   currentVoltage;
    double   request;
    double   tdp;
    double   efficient;
    double   actual;
    uint32_t throttleReasons;
} ctl_freq_state_t;

typedef struct {
    uint32_t Size;
    uint8_t  Version;
    bool     canControl;
    int32_t  defaultLimit;  /* milliwatts */
    int32_t  minLimit;      /* milliwatts */
    int32_t  maxLimit;      /* milliwatts */
} ctl_power_properties_t;

typedef struct {
    uint32_t Size;
    uint8_t  Version;
    uint64_t energy;     /* microjoules, monotonic */
    uint64_t timestamp;  /* microseconds */
} ctl_power_energy_counter_t;

typedef struct {
    uint32_t Size;
    uint8_t  Version;
    int32_t  type;       /* ctl_mem_type_t -- int to avoid enum size issues */
    int32_t  location;   /* ctl_mem_loc_t */
    uint64_t physicalSize;
    int32_t  busWidth;
    int32_t  numChannels;
} ctl_mem_properties_t;

typedef struct {
    uint32_t Size;
    uint8_t  Version;
    uint64_t free;  /* bytes */
    uint64_t size;  /* bytes, allocatable total */
} ctl_mem_state_t;

typedef struct {
    uint32_t Size;
    uint8_t  Version;
    bool     canControl;
    uint32_t supportedModes;
    uint32_t supportedUnits;
    int32_t  maxRPM;
    int32_t  maxPoints;
} ctl_fan_properties_t;

/* ------------------------------------------------------------------
 *  Function pointer typedefs (__cdecl, undecorated names on x64)
 * ------------------------------------------------------------------ */

typedef uint32_t (__cdecl *PFN_ctlInit)(ctl_init_args_t*, ctl_api_handle_t*);
typedef uint32_t (__cdecl *PFN_ctlClose)(ctl_api_handle_t);
typedef uint32_t (__cdecl *PFN_ctlEnumerateDevices)(ctl_api_handle_t, uint32_t*, ctl_device_adapter_handle_t*);
typedef uint32_t (__cdecl *PFN_ctlGetDeviceProperties)(ctl_device_adapter_handle_t, ctl_device_adapter_properties_t*);
typedef uint32_t (__cdecl *PFN_ctlEnumTemperatureSensors)(ctl_device_adapter_handle_t, uint32_t*, ctl_temp_handle_t*);
typedef uint32_t (__cdecl *PFN_ctlTemperatureGetState)(ctl_temp_handle_t, double*);
typedef uint32_t (__cdecl *PFN_ctlEnumEngineGroups)(ctl_device_adapter_handle_t, uint32_t*, ctl_engine_handle_t*);
typedef uint32_t (__cdecl *PFN_ctlEngineGetProperties)(ctl_engine_handle_t, ctl_engine_properties_t*);
typedef uint32_t (__cdecl *PFN_ctlEngineGetActivity)(ctl_engine_handle_t, ctl_engine_stats_t*);
typedef uint32_t (__cdecl *PFN_ctlEnumFrequencyDomains)(ctl_device_adapter_handle_t, uint32_t*, ctl_freq_handle_t*);
typedef uint32_t (__cdecl *PFN_ctlFrequencyGetProperties)(ctl_freq_handle_t, ctl_freq_properties_t*);
typedef uint32_t (__cdecl *PFN_ctlFrequencyGetState)(ctl_freq_handle_t, ctl_freq_state_t*);
typedef uint32_t (__cdecl *PFN_ctlEnumPowerDomains)(ctl_device_adapter_handle_t, uint32_t*, ctl_pwr_handle_t*);
typedef uint32_t (__cdecl *PFN_ctlPowerGetProperties)(ctl_pwr_handle_t, ctl_power_properties_t*);
typedef uint32_t (__cdecl *PFN_ctlPowerGetEnergyCounter)(ctl_pwr_handle_t, ctl_power_energy_counter_t*);
typedef uint32_t (__cdecl *PFN_ctlEnumMemoryModules)(ctl_device_adapter_handle_t, uint32_t*, ctl_mem_handle_t*);
typedef uint32_t (__cdecl *PFN_ctlMemoryGetState)(ctl_mem_handle_t, ctl_mem_state_t*);
typedef uint32_t (__cdecl *PFN_ctlEnumFans)(ctl_device_adapter_handle_t, uint32_t*, ctl_fan_handle_t*);
typedef uint32_t (__cdecl *PFN_ctlFanGetState)(ctl_fan_handle_t, ctl_fan_speed_units_t, int32_t*);

/* ------------------------------------------------------------------
 *  Static state
 * ------------------------------------------------------------------ */

static HMODULE g_lib = NULL;
static int     g_init_ok = 0;

static PFN_ctlInit                 pfn_Init = NULL;
static PFN_ctlClose                pfn_Close = NULL;
static PFN_ctlEnumerateDevices     pfn_EnumDevices = NULL;
static PFN_ctlGetDeviceProperties  pfn_GetDevProps = NULL;
static PFN_ctlEnumTemperatureSensors pfn_EnumTemp = NULL;
static PFN_ctlTemperatureGetState  pfn_TempGetState = NULL;
static PFN_ctlEnumEngineGroups     pfn_EnumEngines = NULL;
static PFN_ctlEngineGetProperties  pfn_EngineGetProps = NULL;
static PFN_ctlEngineGetActivity    pfn_EngineGetActivity = NULL;
static PFN_ctlEnumFrequencyDomains pfn_EnumFreq = NULL;
static PFN_ctlFrequencyGetProperties pfn_FreqGetProps = NULL;
static PFN_ctlFrequencyGetState    pfn_FreqGetState = NULL;
static PFN_ctlEnumPowerDomains     pfn_EnumPower = NULL;
static PFN_ctlPowerGetProperties   pfn_PowerGetProps = NULL;
static PFN_ctlPowerGetEnergyCounter pfn_PowerGetEnergy = NULL;
static PFN_ctlEnumMemoryModules    pfn_EnumMem = NULL;
static PFN_ctlMemoryGetState       pfn_MemGetState = NULL;
static PFN_ctlEnumFans             pfn_EnumFans = NULL;
static PFN_ctlFanGetState          pfn_FanGetState = NULL;

static ctl_api_handle_t g_api_handle = NULL;

/* Per-device cached handles and baseline samples for two-sample deltas */
typedef struct {
    ctl_device_adapter_handle_t adapter;
    uint32_t                    vendor_id;
    uint32_t                    device_id;
    uint8_t                     bus;
    uint8_t                     device;
    uint8_t                     function;
    int                         valid;

    /* engine handle for GT (overall) utilisation + baseline sample */
    ctl_engine_handle_t  engine_gt;
    int                  engine_gt_ok;
    ctl_engine_stats_t   engine_prev;

    /* power handle + baseline energy counter */
    ctl_pwr_handle_t     power;
    int                  power_ok;
    ctl_power_energy_counter_t power_prev;
} IntelDevice;

static IntelDevice g_devs[CTL_MAX_DEVICES];
static uint32_t    g_num_devs = 0;

/* ------------------------------------------------------------------
 *  Helper macros
 * ------------------------------------------------------------------ */

#define RESOLVE(ptr, name) ((ptr) = (void*)GetProcAddress(g_lib, name))

/* ------------------------------------------------------------------
 *  intel_init
 * ------------------------------------------------------------------ */

int intel_init(void)
{
    uint32_t i, n;

    if (g_init_ok)  return 0;
    if (g_init_ok < 0) return -1;

    /* The canonical IGCL DLL is ControlLib.dll (shipped with the Intel
     * graphics driver).  Try it first, then a couple of historical names
     * as a safety net. */
    g_lib = LoadLibraryA("ControlLib.dll");
    if (!g_lib) g_lib = LoadLibraryA("igclxx.dll");
    if (!g_lib) g_lib = LoadLibraryA("IntelControlLib.dll");
    if (!g_lib) { g_init_ok = -1; return -1; }

    /* required entry points */
    RESOLVE(pfn_Init,        "ctlInit");
    RESOLVE(pfn_Close,       "ctlClose");
    RESOLVE(pfn_EnumDevices, "ctlEnumerateDevices");
    if (!pfn_Init || !pfn_Close || !pfn_EnumDevices) goto fail;

    /* device properties (needed for PCI matching) */
    RESOLVE(pfn_GetDevProps, "ctlGetDeviceProperties");
    if (!pfn_GetDevProps) goto fail;

    /* telemetry -- all optional, degrade to N/A if missing */
    RESOLVE(pfn_EnumTemp,         "ctlEnumTemperatureSensors");
    RESOLVE(pfn_TempGetState,     "ctlTemperatureGetState");
    RESOLVE(pfn_EnumEngines,      "ctlEnumEngineGroups");
    RESOLVE(pfn_EngineGetProps,   "ctlEngineGetProperties");
    RESOLVE(pfn_EngineGetActivity,"ctlEngineGetActivity");
    RESOLVE(pfn_EnumFreq,         "ctlEnumFrequencyDomains");
    RESOLVE(pfn_FreqGetProps,     "ctlFrequencyGetProperties");
    RESOLVE(pfn_FreqGetState,     "ctlFrequencyGetState");
    RESOLVE(pfn_EnumPower,        "ctlEnumPowerDomains");
    RESOLVE(pfn_PowerGetProps,    "ctlPowerGetProperties");
    RESOLVE(pfn_PowerGetEnergy,   "ctlPowerGetEnergyCounter");
    RESOLVE(pfn_EnumMem,          "ctlEnumMemoryModules");
    RESOLVE(pfn_MemGetState,      "ctlMemoryGetState");
    RESOLVE(pfn_EnumFans,         "ctlEnumFans");
    RESOLVE(pfn_FanGetState,      "ctlFanGetState");

    /* initialise IGCL -- Level Zero flag is mandatory for telemetry */
    {
        ctl_init_args_t args;
        memset(&args, 0, sizeof(args));
        args.Size        = (uint32_t)sizeof(args);
        args.Version     = 0;
        args.AppVersion  = CTL_MAKE_VERSION(1, 1);
        args.flags       = CTL_INIT_FLAG_USE_LEVEL_ZERO;
        if (pfn_Init(&args, &g_api_handle) != CTL_RESULT_SUCCESS) {
            fprintf(stderr, "Intel IGCL: ctlInit failed\n");
            goto fail;
        }
    }

    /* enumerate adapters */
    n = 0;
    if (pfn_EnumDevices(g_api_handle, &n, NULL) != CTL_RESULT_SUCCESS || n == 0)
        goto fail;
    if (n > CTL_MAX_DEVICES) n = CTL_MAX_DEVICES;
    g_num_devs = n;

    /* EnumerateDevices is two-pass: first call with NULL to get the
     * count, second call with an array to fill the handles.  It always
     * writes the full list starting from adapter 0. */
    {
        ctl_device_adapter_handle_t handles[CTL_MAX_DEVICES];
        memset(handles, 0, sizeof(handles));
        if (pfn_EnumDevices(g_api_handle, &n, handles) != CTL_RESULT_SUCCESS)
            goto fail;
        g_num_devs = n;

        for (i = 0; i < n; i++) {
            ctl_device_adapter_properties_t props;
            uint8_t luid[8];  /* LUID is 8 bytes on Windows */
            IntelDevice *d = &g_devs[i];

            d->adapter = handles[i];
            if (!d->adapter) continue;

            memset(&props, 0, sizeof(props));
            props.Size    = (uint32_t)sizeof(props);
            props.Version = 2;            /* need adapter_bdf (Version >= 2) */
            props.pDeviceID = luid;
            props.device_id_size = (uint32_t)sizeof(luid);

            if (pfn_GetDevProps(d->adapter, &props) != CTL_RESULT_SUCCESS)
                continue;

            d->vendor_id  = props.pci_vendor_id;
            d->device_id  = props.pci_device_id;
            d->bus        = props.adapter_bdf.bus;
            d->device     = props.adapter_bdf.device;
            d->function   = props.adapter_bdf.function;
            d->valid      = 1;

            /* cache engine handle for GT group + baseline sample */
            if (pfn_EnumEngines && pfn_EngineGetProps && pfn_EngineGetActivity) {
                uint32_t ecnt = 0, e;
                ctl_engine_handle_t ehs[16];
                if (pfn_EnumEngines(d->adapter, &ecnt, NULL) == CTL_RESULT_SUCCESS && ecnt > 0) {
                    if (ecnt > 16) ecnt = 16;
                    if (pfn_EnumEngines(d->adapter, &ecnt, ehs) == CTL_RESULT_SUCCESS) {
                        for (e = 0; e < ecnt; e++) {
                            ctl_engine_properties_t ep;
                            memset(&ep, 0, sizeof(ep));
                            ep.Size = (uint32_t)sizeof(ep);
                            if (pfn_EngineGetProps(ehs[e], &ep) == CTL_RESULT_SUCCESS &&
                                ep.type == CTL_ENGINE_GROUP_GT) {
                                d->engine_gt = ehs[e];
                                d->engine_gt_ok = 1;
                                memset(&d->engine_prev, 0, sizeof(d->engine_prev));
                                d->engine_prev.Size = (uint32_t)sizeof(d->engine_prev);
                                pfn_EngineGetActivity(d->engine_gt, &d->engine_prev);
                                break;
                            }
                        }
                    }
                }
            }

            /* cache power handle + baseline energy counter */
            if (pfn_EnumPower && pfn_PowerGetEnergy) {
                uint32_t pcnt = 0;
                ctl_pwr_handle_t phs[8];
                if (pfn_EnumPower(d->adapter, &pcnt, NULL) == CTL_RESULT_SUCCESS && pcnt > 0) {
                    if (pcnt > 8) pcnt = 8;
                    if (pfn_EnumPower(d->adapter, &pcnt, phs) == CTL_RESULT_SUCCESS) {
                        d->power = phs[0];
                        d->power_ok = 1;
                        memset(&d->power_prev, 0, sizeof(d->power_prev));
                        d->power_prev.Size = (uint32_t)sizeof(d->power_prev);
                        pfn_PowerGetEnergy(d->power, &d->power_prev);
                    }
                }
            }
        }
    }

    g_init_ok = 1;
    return 0;

fail:
    if (g_api_handle && pfn_Close) { pfn_Close(g_api_handle); g_api_handle = NULL; }
    if (g_lib) { FreeLibrary(g_lib); g_lib = NULL; }
    g_init_ok = -1;
    return -1;
}

/* ------------------------------------------------------------------
 *  intel_shutdown
 * ------------------------------------------------------------------ */

void intel_shutdown(void)
{
    if (g_api_handle && pfn_Close) {
        pfn_Close(g_api_handle);
        g_api_handle = NULL;
    }
    if (g_lib) {
        FreeLibrary(g_lib);
        g_lib = NULL;
    }
    memset(g_devs, 0, sizeof(g_devs));
    g_num_devs = 0;
    g_init_ok = 0;
}

/* ------------------------------------------------------------------
 *  Matching
 * ------------------------------------------------------------------ */

int intel_find_by_pci(uint32_t vendor_id, uint32_t device_id, int *index)
{
    uint32_t i;
    if (g_init_ok <= 0) return -1;
    for (i = 0; i < g_num_devs; i++) {
        if (!g_devs[i].valid) continue;
        if (g_devs[i].vendor_id == vendor_id &&
            g_devs[i].device_id == device_id) {
            *index = (int)i;
            return 0;
        }
    }
    return -1;
}

int intel_find_by_pci_topology(uint32_t domain, uint32_t bus,
                               uint32_t device, uint32_t function,
                               int *index)
{
    uint32_t i;
    (void)domain;  /* IGCL adapter_bdf has no domain field */
    if (g_init_ok <= 0) return -1;

    for (i = 0; i < g_num_devs; i++) {
        if (!g_devs[i].valid) continue;
        if ((uint32_t)g_devs[i].bus      == bus &&
            (uint32_t)g_devs[i].device   == device &&
            (uint32_t)g_devs[i].function == function) {
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
    if ((idx) < 0 || (uint32_t)(idx) >= g_num_devs || !g_devs[(idx)].valid) return -1;

int intel_get_temperature(int idx, int *milli_c)
{
    uint32_t cnt = 0, t;
    ctl_temp_handle_t ths[8];
    double temp;

    CHECK(idx);
    if (!pfn_EnumTemp || !pfn_TempGetState) return -1;

    if (pfn_EnumTemp(g_devs[idx].adapter, &cnt, NULL) != CTL_RESULT_SUCCESS || cnt == 0)
        return -1;
    if (cnt > 8) cnt = 8;
    if (pfn_EnumTemp(g_devs[idx].adapter, &cnt, ths) != CTL_RESULT_SUCCESS)
        return -1;

    /* prefer GPU sensor, fall back to global */
    for (t = 0; t < cnt; t++) {
        /* We don't need properties -- just try each sensor.  The GPU
         * sensor is usually index 0 or 1.  ctlTemperatureGetState
         * returns the reading directly; pick the first valid one. */
        temp = 0.0;
        if (pfn_TempGetState(ths[t], &temp) == CTL_RESULT_SUCCESS && temp > 0.0) {
            *milli_c = (int)(temp * 1000.0);
            return 0;
        }
    }
    return -1;
}

int intel_get_utilization(int idx, int *gpu_pct, int *mem_pct)
{
    ctl_engine_stats_t now;
    uint64_t dt, da;

    CHECK(idx);
    *gpu_pct = -1;
    *mem_pct = -1;  /* IGCL has no memory utilisation percentage */

    if (!g_devs[idx].engine_gt_ok || !pfn_EngineGetActivity)
        return -1;

    memset(&now, 0, sizeof(now));
    now.Size = (uint32_t)sizeof(now);
    if (pfn_EngineGetActivity(g_devs[idx].engine_gt, &now) != CTL_RESULT_SUCCESS)
        return -1;

    dt = now.timestamp - g_devs[idx].engine_prev.timestamp;
    da = now.activeTime - g_devs[idx].engine_prev.activeTime;

    /* refresh baseline for next call */
    g_devs[idx].engine_prev = now;

    if (dt == 0) return -1;

    {
        long long pct = (long long)(da * 100ULL / dt);
        if (pct < 0)   pct = 0;
        if (pct > 100) pct = 100;
        *gpu_pct = (int)pct;
    }
    return 0;
}

int intel_get_clocks(int idx, int *core_mhz, int *mem_mhz)
{
    uint32_t cnt = 0, f;
    ctl_freq_handle_t fhs[8];

    CHECK(idx);
    *core_mhz = -1;
    *mem_mhz  = -1;

    if (!pfn_EnumFreq || !pfn_FreqGetState) return -1;

    if (pfn_EnumFreq(g_devs[idx].adapter, &cnt, NULL) != CTL_RESULT_SUCCESS || cnt == 0)
        return -1;
    if (cnt > 8) cnt = 8;
    if (pfn_EnumFreq(g_devs[idx].adapter, &cnt, fhs) != CTL_RESULT_SUCCESS)
        return -1;

    for (f = 0; f < cnt; f++) {
        ctl_freq_properties_t fp;
        ctl_freq_state_t fs;

        memset(&fp, 0, sizeof(fp));
        fp.Size = (uint32_t)sizeof(fp);
        if (pfn_FreqGetProps(fhs[f], &fp) != CTL_RESULT_SUCCESS)
            continue;

        memset(&fs, 0, sizeof(fs));
        fs.Size = (uint32_t)sizeof(fs);
        if (pfn_FreqGetState(fhs[f], &fs) != CTL_RESULT_SUCCESS)
            continue;

        if (fp.type == CTL_FREQ_DOMAIN_GPU) {
            *core_mhz = (int)fs.actual;
        } else if (fp.type == CTL_FREQ_DOMAIN_MEMORY) {
            *mem_mhz = (int)fs.actual;
        }
    }

    return (*core_mhz >= 0) ? 0 : -1;
}

int intel_get_power(int idx, int *usage_mw, int *limit_mw)
{
    ctl_power_energy_counter_t now;
    uint64_t dt, de;

    CHECK(idx);
    *usage_mw = -1;
    *limit_mw = -1;

    /* power limit from power properties (single call, no sampling) */
    if (pfn_EnumPower && pfn_PowerGetProps) {
        uint32_t pcnt = 0;
        ctl_pwr_handle_t phs[8];
        if (pfn_EnumPower(g_devs[idx].adapter, &pcnt, NULL) == CTL_RESULT_SUCCESS && pcnt > 0) {
            if (pcnt > 8) pcnt = 8;
            if (pfn_EnumPower(g_devs[idx].adapter, &pcnt, phs) == CTL_RESULT_SUCCESS) {
                ctl_power_properties_t pp;
                memset(&pp, 0, sizeof(pp));
                pp.Size = (uint32_t)sizeof(pp);
                if (pfn_PowerGetProps(phs[0], &pp) == CTL_RESULT_SUCCESS &&
                    pp.defaultLimit > 0) {
                    *limit_mw = pp.defaultLimit;
                }
            }
        }
    }

    /* power usage from two-sample energy counter delta */
    if (!g_devs[idx].power_ok || !pfn_PowerGetEnergy)
        return (*limit_mw >= 0) ? 0 : -1;

    memset(&now, 0, sizeof(now));
    now.Size = (uint32_t)sizeof(now);
    if (pfn_PowerGetEnergy(g_devs[idx].power, &now) != CTL_RESULT_SUCCESS)
        return (*limit_mw >= 0) ? 0 : -1;

    dt = now.timestamp - g_devs[idx].power_prev.timestamp;
    de = now.energy    - g_devs[idx].power_prev.energy;

    /* refresh baseline */
    g_devs[idx].power_prev = now;

    if (dt == 0) return (*limit_mw >= 0) ? 0 : -1;

    /* energy is microjoules, timestamp is microseconds.
     * power_w = dEnergy_uJ / dTime_us  (since uJ/us = W) */
    {
        long long mw = (long long)(de * 1000ULL / dt);  /* W -> mW */
        if (mw < 0) mw = 0;
        *usage_mw = (int)mw;
    }
    return 0;
}

int intel_get_memory(int idx, uint64_t *used_bytes, uint64_t *total_bytes)
{
    uint32_t cnt = 0, m;
    ctl_mem_handle_t mhs[8];

    CHECK(idx);
    *used_bytes  = 0;
    *total_bytes = 0;

    if (!pfn_EnumMem || !pfn_MemGetState) return -1;

    if (pfn_EnumMem(g_devs[idx].adapter, &cnt, NULL) != CTL_RESULT_SUCCESS || cnt == 0)
        return -1;
    if (cnt > 8) cnt = 8;
    if (pfn_EnumMem(g_devs[idx].adapter, &cnt, mhs) != CTL_RESULT_SUCCESS)
        return -1;

    /* Sum all memory modules.  For discrete GPUs there is typically one
     * device-local module (VRAM); for iGPUs there may be a system-memory
     * module.  We report the largest module's size/used to match the
     * "VRAM" semantics expected by GpuInfo. */
    for (m = 0; m < cnt; m++) {
        ctl_mem_state_t ms;
        memset(&ms, 0, sizeof(ms));
        ms.Size = (uint32_t)sizeof(ms);
        if (pfn_MemGetState(mhs[m], &ms) != CTL_RESULT_SUCCESS)
            continue;
        if (ms.size > *total_bytes) {
            *total_bytes = ms.size;
            *used_bytes  = ms.size - ms.free;
        }
    }

    return (*total_bytes > 0) ? 0 : -1;
}

int intel_get_fan(int idx, int *rpm, int *percent)
{
    uint32_t cnt = 0;
    ctl_fan_handle_t fhs[8];
    int32_t speed_rpm = -1, speed_pct = -1;

    CHECK(idx);
    *rpm = -1;
    *percent = -1;

    if (!pfn_EnumFans || !pfn_FanGetState) return -1;

    if (pfn_EnumFans(g_devs[idx].adapter, &cnt, NULL) != CTL_RESULT_SUCCESS || cnt == 0)
        return -1;  /* iGPUs have no fans */
    if (cnt > 8) cnt = 8;
    if (pfn_EnumFans(g_devs[idx].adapter, &cnt, fhs) != CTL_RESULT_SUCCESS)
        return -1;

    /* take the first fan */
    if (pfn_FanGetState(fhs[0], CTL_FAN_SPEED_UNITS_RPM, &speed_rpm) == CTL_RESULT_SUCCESS)
        *rpm = (int)speed_rpm;
    if (pfn_FanGetState(fhs[0], CTL_FAN_SPEED_UNITS_PERCENT, &speed_pct) == CTL_RESULT_SUCCESS)
        *percent = (int)speed_pct;

    return (*rpm >= 0 || *percent >= 0) ? 0 : -1;
}

#endif /* _WIN32 */
