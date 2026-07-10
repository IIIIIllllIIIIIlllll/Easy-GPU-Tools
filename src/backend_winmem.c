/* ================================================================
 *  backend_winmem.c  --  Windows-native GPU memory & utilization
 *
 *  Uses PDH (Performance Data Helper) to read the WDDM GPU
 *  performance counters:
 *    \GPU Adapter Memory(luid_<hex>_<hex>_phys_0)\Dedicated Usage
 *    \GPU Adapter Memory(luid_<hex>_<hex>_phys_0)\Shared Usage
 *    \GPU Adapter Memory(luid_<hex>_<hex>_phys_0)\Total Committed
 *    \GPU Engine(pid_*_luid_<hex>_<hex>_phys_0_eng_*_*)\Utilization Percentage
 *
 *  These counters are exposed by the WDDM driver and work for all
 *  GPUs (AMD iGPU, dGPUs, Intel, NVIDIA) without vendor SDKs.
 *
 *  LUID comes from the caller, typically obtained from Vulkan's
 *  VkPhysicalDeviceIDProperties::deviceLUID on Windows.
 * ================================================================ */

#ifdef _WIN32

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <pdh.h>
#include <pdhmsg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#include "backend_winmem.h"

/* ------------------------------------------------------------------
 *  Static state
 * ------------------------------------------------------------------ */

static int           g_init_ok     = 0;

/* ------------------------------------------------------------------
 *  winmem_init  /  winmem_shutdown
 *
 *  PDH is available system-wide; no persistent query handle needed
 *  since each call opens and closes its own query.  This is just a
 *  guard so we report init status.
 * ------------------------------------------------------------------ */

int winmem_init(void)
{
    if (g_init_ok) return 0;
    if (g_init_ok < 0) return -1;
    g_init_ok = 1;
    return 0;
}

void winmem_shutdown(void)
{
    g_init_ok = 0;
}

/* ------------------------------------------------------------------
 *  winmem_get_memory  --  read GPU Adapter Memory PDH counters
 *
 *  Builds the counter path from the LUID and reads all three counters
 *  in a single PDH query.  Values are in bytes.
 * ------------------------------------------------------------------ */

int winmem_get_memory(uint32_t luid_low, uint32_t luid_high,
                      uint64_t *dedicated_used,
                      uint64_t *shared_used,
                      uint64_t *total_committed)
{
    if (g_init_ok <= 0) return -1;

    char instance[128];
    /* Instance name format: luid_0xHIGH_0xLOW_phys_0 */
    snprintf(instance, sizeof(instance),
             "luid_0x%08X_0x%08X_phys_0", luid_high, luid_low);

    /* Build counter paths */
    char path_ded[256], path_sha[256], path_tot[256];
    snprintf(path_ded, sizeof(path_ded),
             "\\GPU Adapter Memory(%s)\\Dedicated Usage", instance);
    snprintf(path_sha, sizeof(path_sha),
             "\\GPU Adapter Memory(%s)\\Shared Usage", instance);
    snprintf(path_tot, sizeof(path_tot),
             "\\GPU Adapter Memory(%s)\\Total Committed", instance);

    PDH_HQUERY query = NULL;
    PDH_STATUS ps = PdhOpenQueryH(NULL, 0, &query);
    if (ps != ERROR_SUCCESS || !query) return -1;

    PDH_HCOUNTER c_ded = NULL, c_sha = NULL, c_tot = NULL;
    if (dedicated_used)
        PdhAddCounterA(query, path_ded, 0, &c_ded);
    if (shared_used)
        PdhAddCounterA(query, path_sha, 0, &c_sha);
    if (total_committed)
        PdhAddCounterA(query, path_tot, 0, &c_tot);

    /* Collect current values */
    ps = PdhCollectQueryData(query);
    if (ps != ERROR_SUCCESS) {
        /* First collect may fail; try once more after a brief wait */
        Sleep(50);
        ps = PdhCollectQueryData(query);
    }

    int got = 0;
    PDH_FMT_COUNTERVALUE val;

    if (c_ded && dedicated_used) {
        if (PdhGetFormattedCounterValue(c_ded, PDH_FMT_LARGE | PDH_FMT_NOCAP100,
                                        NULL, &val) == ERROR_SUCCESS) {
            *dedicated_used = (uint64_t)val.largeValue;
            got = 1;
        }
    }
    if (c_sha && shared_used) {
        if (PdhGetFormattedCounterValue(c_sha, PDH_FMT_LARGE | PDH_FMT_NOCAP100,
                                        NULL, &val) == ERROR_SUCCESS) {
            *shared_used = (uint64_t)val.largeValue;
            got = 1;
        }
    }
    if (c_tot && total_committed) {
        if (PdhGetFormattedCounterValue(c_tot, PDH_FMT_LARGE | PDH_FMT_NOCAP100,
                                        NULL, &val) == ERROR_SUCCESS) {
            *total_committed = (uint64_t)val.largeValue;
            got = 1;
        }
    }

    PdhCloseQuery(query);
    return got ? 0 : -1;
}

/* ------------------------------------------------------------------
 *  winmem_get_utilization  --  read GPU Engine utilization via PDH
 *
 *  Enumerates all "\GPU Engine(pid_*_luid_<luid>_phys_0_eng_*)\Utilization"
 *  counter instances, collects data twice (baseline + delta), and
 *  returns the maximum utilization across all engine types.
 *
 *  The "Utilization Percentage" counter is a timer-based counter:
 *  it reports the fraction of time the engine was busy during the
 *  sampling interval.  We take the max across all engines to get the
 *  overall GPU utilization (matching Task Manager's "GPU %" view).
 * ------------------------------------------------------------------ */

int winmem_get_utilization(uint32_t luid_low, uint32_t luid_high,
                           int *util_pct)
{
    if (g_init_ok <= 0) return -1;

    /* Build the LUID prefix to match instances */
    char luid_prefix[64];
    snprintf(luid_prefix, sizeof(luid_prefix),
             "luid_0x%08X_0x%08X", luid_high, luid_low);

    /* Enumerate all counter instances for "GPU Engine" */
    DWORD buf_size = 0;
    DWORD ret;

    /* First call to get buffer size */
    ret = PdhEnumObjectItemsHA(NULL, NULL, "GPU Engine",
                                NULL, &buf_size,
                                NULL, NULL,
                                PERF_DETAIL_WIZARD, 0);
    if (ret != PDH_MORE_DATA || buf_size == 0) return -1;

    char *instances = (char*)malloc(buf_size);
    if (!instances) return -1;
    memset(instances, 0, buf_size);

    DWORD inst_size = buf_size;
    ret = PdhEnumObjectItemsHA(NULL, NULL, "GPU Engine",
                                instances, &inst_size,
                                NULL, NULL,
                                PERF_DETAIL_WIZARD, 0);
    if (ret != ERROR_SUCCESS) {
        free(instances);
        return -1;
    }

    /* Collect matching instance paths */
    char **paths = NULL;
    int path_count = 0, path_cap = 0;

    char *p = instances;
    while (*p) {
        /* Check if this instance matches our LUID and is a 3D engine type */
        if (strstr(p, luid_prefix) && strstr(p, "engtype_3d")) {
            char path[512];
            snprintf(path, sizeof(path),
                     "\\GPU Engine(%s)\\Utilization Percentage", p);
            if (path_count >= path_cap) {
                path_cap = path_cap ? path_cap * 2 : 16;
                paths = (char**)realloc(paths, path_cap * sizeof(char*));
                if (!paths) { free(instances); return -1; }
            }
            paths[path_count] = _strdup(path);
            if (!paths[path_count]) { free(instances); return -1; }
            path_count++;
        }
        p += strlen(p) + 1;
    }
    free(instances);

    if (path_count == 0) {
        free(paths);
        return -1;
    }

    /* Create PDH query with all matching counters */
    PDH_HQUERY query = NULL;
    if (PdhOpenQueryH(NULL, 0, &query) != ERROR_SUCCESS || !query) {
        for (int i = 0; i < path_count; i++) free(paths[i]);
        free(paths);
        return -1;
    }

    PDH_HCOUNTER *counters = (PDH_HCOUNTER*)calloc(path_count, sizeof(PDH_HCOUNTER));
    if (!counters) {
        PdhCloseQuery(query);
        for (int i = 0; i < path_count; i++) free(paths[i]);
        free(paths);
        return -1;
    }

    for (int i = 0; i < path_count; i++) {
        PdhAddCounterA(query, paths[i], 0, &counters[i]);
    }

    /* Collect baseline, wait, collect again */
    PdhCollectQueryData(query);
    Sleep(200);
    PdhCollectQueryData(query);

    /* Read formatted values, take the max */
    int max_util = 0;
    for (int i = 0; i < path_count; i++) {
        PDH_FMT_COUNTERVALUE val;
        if (PdhGetFormattedCounterValue(counters[i],
                                         PDH_FMT_DOUBLE | PDH_FMT_NOCAP100,
                                         NULL, &val) == ERROR_SUCCESS) {
            int u = (int)(val.doubleValue + 0.5);
            if (u > max_util) max_util = u;
        }
    }

    PdhCloseQuery(query);
    free(counters);
    for (int i = 0; i < path_count; i++) free(paths[i]);
    free(paths);

    if (max_util > 100) max_util = 100;
    *util_pct = max_util;
    return 0;
}

#endif /* _WIN32 */