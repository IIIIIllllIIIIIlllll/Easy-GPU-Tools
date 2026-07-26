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
 *
 *  Notes on correctness:
 *  - Counter paths are built with English names and added through
 *    PdhAddEnglishCounterW, which works on any Windows locale
 *    (PdhAddCounterW would require locale-dependent names).
 *  - Object enumeration needs the *localized* object name; it is
 *    resolved from the English name via the Perflib\009 registry
 *    hive + PdhLookupPerfNameByIndexW.
 *  - Formatted counter values are only trusted when
 *    PDH_FMT_COUNTERVALUE.CStatus == ERROR_SUCCESS.
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
#include <wctype.h>

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
    if (g_init_ok > 0) return 0;
    if (g_init_ok < 0) return -1;
    g_init_ok = 1;
    return 0;
}

void winmem_shutdown(void)
{
    g_init_ok = 0;
}

/* ------------------------------------------------------------------
 *  Helpers
 * ------------------------------------------------------------------ */

/* case-insensitive substring search (wide chars) */
static int wcs_contains_nocase(const wchar_t *haystack, const wchar_t *needle)
{
    size_t n = wcslen(needle);
    if (!n) return 0;
    for (; *haystack; haystack++) {
        size_t i;
        for (i = 0; i < n; i++) {
            if (!haystack[i]) return 0;
            if (towlower(haystack[i]) != towlower(needle[i])) break;
        }
        if (i == n) return 1;
    }
    return 0;
}

/* Resolve the localized performance-object name for an English name.
 * PDH object/counter names are locale-dependent.  The English names are
 * listed in the Perflib\009 registry value "Counter" (a multi-sz of
 * alternating index / name strings); the index maps to the localized
 * name through PdhLookupPerfNameByIndexW.  Falls back to the English
 * name itself, which is correct on English systems. */
static void resolve_perf_object_name(const char *english,
                                     wchar_t *out, DWORD out_cch)
{
    HKEY hkey;
    out[0] = L'\0';

    if (RegOpenKeyExA(HKEY_LOCAL_MACHINE,
                      "SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion\\Perflib\\009",
                      0, KEY_READ, &hkey) == ERROR_SUCCESS) {
        DWORD type = 0, size = 0;
        if (RegQueryValueExA(hkey, "Counter", NULL, &type, NULL, &size)
                == ERROR_SUCCESS &&
            type == REG_MULTI_SZ && size >= 4) {
            char *buf = (char*)malloc(size);
            if (buf) {
                if (RegQueryValueExA(hkey, "Counter", NULL, &type,
                                     (LPBYTE)buf, &size) == ERROR_SUCCESS) {
                    const char *prev = NULL;   /* previous string = index */
                    const char *p = buf;
                    while (*p) {
                        if (prev && _stricmp(p, english) == 0) {
                            DWORD idx = (DWORD)strtoul(prev, NULL, 10);
                            DWORD cch = out_cch;
                            if (PdhLookupPerfNameByIndexW(NULL, idx, out, &cch)
                                    != ERROR_SUCCESS)
                                out[0] = L'\0';
                            break;
                        }
                        prev = p;
                        p += strlen(p) + 1;
                    }
                }
                free(buf);
            }
        }
        RegCloseKey(hkey);
    }

    if (!out[0])
        MultiByteToWideChar(CP_UTF8, 0, english, -1, out, (int)out_cch);
}

/* ------------------------------------------------------------------
 *  winmem_get_memory  --  read GPU Adapter Memory PDH counters
 *
 *  Builds the counter paths from the LUID and reads the counters in a
 *  single PDH query.  Values are in bytes.  Note that "Total Committed"
 *  is a dynamic committed figure, NOT the adapter's total capacity.
 * ------------------------------------------------------------------ */

int winmem_get_memory(uint32_t luid_low, uint32_t luid_high,
                      uint64_t *dedicated_used,
                      uint64_t *shared_used,
                      uint64_t *total_committed)
{
    if (g_init_ok <= 0) return -1;

    wchar_t instance[128];
    /* Instance name format: luid_0xHIGH_0xLOW_phys_0 */
    _snwprintf(instance, 128, L"luid_0x%08X_0x%08X_phys_0",
               luid_high, luid_low);
    instance[127] = L'\0';

    /* Build counter paths (English names; added via PdhAddEnglishCounterW) */
    wchar_t path_ded[256], path_sha[256], path_tot[256];
    _snwprintf(path_ded, 256, L"\\GPU Adapter Memory(%s)\\Dedicated Usage",
               instance);
    path_ded[255] = L'\0';
    _snwprintf(path_sha, 256, L"\\GPU Adapter Memory(%s)\\Shared Usage",
               instance);
    path_sha[255] = L'\0';
    _snwprintf(path_tot, 256, L"\\GPU Adapter Memory(%s)\\Total Committed",
               instance);
    path_tot[255] = L'\0';

    PDH_HQUERY query = NULL;
    if (PdhOpenQueryH(NULL, 0, &query) != ERROR_SUCCESS || !query)
        return -1;

    PDH_HCOUNTER c_ded = NULL, c_sha = NULL, c_tot = NULL;
    if (dedicated_used)
        if (PdhAddEnglishCounterW(query, path_ded, 0, &c_ded) != ERROR_SUCCESS)
            c_ded = NULL;
    if (shared_used)
        if (PdhAddEnglishCounterW(query, path_sha, 0, &c_sha) != ERROR_SUCCESS)
            c_sha = NULL;
    if (total_committed)
        if (PdhAddEnglishCounterW(query, path_tot, 0, &c_tot) != ERROR_SUCCESS)
            c_tot = NULL;

    if (!c_ded && !c_sha && !c_tot) {
        PdhCloseQuery(query);
        return -1;
    }

    /* Collect current values */
    PDH_STATUS ps = PdhCollectQueryData(query);
    if (ps != ERROR_SUCCESS) {
        /* First collect may fail; try once more after a brief wait */
        Sleep(50);
        ps = PdhCollectQueryData(query);
    }
    if (ps != ERROR_SUCCESS) {
        PdhCloseQuery(query);
        return -1;
    }

    int got = 0;
    PDH_FMT_COUNTERVALUE val;

    if (c_ded && dedicated_used) {
        if (PdhGetFormattedCounterValue(c_ded, PDH_FMT_LARGE | PDH_FMT_NOCAP100,
                                        NULL, &val) == ERROR_SUCCESS &&
            val.CStatus == ERROR_SUCCESS) {
            *dedicated_used = (uint64_t)val.largeValue;
            got = 1;
        }
    }
    if (c_sha && shared_used) {
        if (PdhGetFormattedCounterValue(c_sha, PDH_FMT_LARGE | PDH_FMT_NOCAP100,
                                        NULL, &val) == ERROR_SUCCESS &&
            val.CStatus == ERROR_SUCCESS) {
            *shared_used = (uint64_t)val.largeValue;
            got = 1;
        }
    }
    if (c_tot && total_committed) {
        if (PdhGetFormattedCounterValue(c_tot, PDH_FMT_LARGE | PDH_FMT_NOCAP100,
                                        NULL, &val) == ERROR_SUCCESS &&
            val.CStatus == ERROR_SUCCESS) {
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
 *  Enumerates all "\GPU Engine(...)\Utilization Percentage" instances
 *  belonging to the given LUID, collects data twice (baseline + delta),
 *  aggregates utilization per physical engine (summing across the PIDs
 *  that share it) and returns the busiest engine's value.  This matches
 *  Task Manager's overall "GPU" view.  All engine types count (3D,
 *  Compute, Copy, VideoDecode, VideoEncode, ...).
 *
 *  The "Utilization Percentage" counter is a timer-based counter:
 *  it reports the fraction of time the engine was busy during the
 *  sampling interval.
 * ------------------------------------------------------------------ */

#define WINMEM_MAX_ENGINES 128

int winmem_get_utilization(uint32_t luid_low, uint32_t luid_high,
                           int *util_pct)
{
    if (g_init_ok <= 0) return -1;

    /* Build the LUID prefix to match instances */
    wchar_t luid_prefix[64];
    _snwprintf(luid_prefix, 64, L"luid_0x%08X_0x%08X", luid_high, luid_low);
    luid_prefix[63] = L'\0';

    /* The "GPU Engine" object name is locale-dependent; resolve it. */
    wchar_t obj_name[128];
    resolve_perf_object_name("GPU Engine", obj_name, 128);

    /* Enumerate all instances of the object.  The instance list is the
     * 6th/7th parameter pair -- the 4th/5th pair is the counter list;
     * mixing them up enumerates counter names instead of instances. */
    DWORD counter_size = 0, instance_size = 0;
    PDH_STATUS ret = PdhEnumObjectItemsHW(NULL, NULL, obj_name,
                                          NULL, &counter_size,
                                          NULL, &instance_size,
                                          PERF_DETAIL_WIZARD, 0);
    if (ret != PDH_MORE_DATA || instance_size == 0)
        return -1;

    wchar_t *counter_buf = (wchar_t*)calloc(counter_size ? counter_size : 1,
                                            sizeof(wchar_t));
    wchar_t *instances   = (wchar_t*)calloc(instance_size, sizeof(wchar_t));
    if (!counter_buf || !instances) {
        free(counter_buf);
        free(instances);
        return -1;
    }

    ret = PdhEnumObjectItemsHW(NULL, NULL, obj_name,
                               counter_buf, &counter_size,
                               instances, &instance_size,
                               PERF_DETAIL_WIZARD, 0);
    free(counter_buf);
    if (ret != ERROR_SUCCESS) {
        free(instances);
        return -1;
    }

    /* Collect matching instance paths.
     * Instance format:
     *   pid_<n>_luid_0xHIGH_0xLOW_phys_<n>_eng_<n>_engtype_<TYPE>
     * Matching is case-insensitive: real engine-type names use mixed
     * case ("engtype_3D", "engtype_Copy", ...). */
    wchar_t **paths = NULL;
    int     *eng_of_path = NULL;
    int path_count = 0, path_cap = 0;

    wchar_t *p = instances;
    while (*p) {
        if (wcs_contains_nocase(p, luid_prefix)) {
            const wchar_t *ep = wcsstr(p, L"_eng_");
            if (ep) {
                long eidx = wcstol(ep + 5, NULL, 10);
                wchar_t path[512];
                _snwprintf(path, 512,
                           L"\\GPU Engine(%s)\\Utilization Percentage", p);
                path[511] = L'\0';
                if (path_count >= path_cap) {
                    int new_cap = path_cap ? path_cap * 2 : 16;
                    wchar_t **np = (wchar_t**)realloc(paths,
                                                      new_cap * sizeof(wchar_t*));
                    int *ne = (int*)realloc(eng_of_path,
                                            new_cap * sizeof(int));
                    if (!np || !ne) {
                        /* adopt whichever realloc succeeded (it freed the
                         * old block); the other pointer is still valid */
                        if (np) paths = np;
                        if (ne) eng_of_path = ne;
                        goto fail;
                    }
                    paths = np;
                    eng_of_path = ne;
                    path_cap = new_cap;
                }
                paths[path_count] = _wcsdup(path);
                if (!paths[path_count])
                    goto fail;
                eng_of_path[path_count] = (int)eidx;
                path_count++;
            }
        }
        p += wcslen(p) + 1;
    }
    free(instances);
    instances = NULL;

    if (path_count == 0)
        goto fail;

    /* Create PDH query with all matching counters */
    PDH_HQUERY query = NULL;
    if (PdhOpenQueryH(NULL, 0, &query) != ERROR_SUCCESS || !query)
        goto fail;

    PDH_HCOUNTER *counters =
        (PDH_HCOUNTER*)calloc((size_t)path_count, sizeof(PDH_HCOUNTER));
    if (!counters) {
        PdhCloseQuery(query);
        goto fail;
    }

    int added = 0;
    for (int i = 0; i < path_count; i++) {
        if (PdhAddEnglishCounterW(query, paths[i], 0, &counters[i])
                == ERROR_SUCCESS)
            added++;
    }
    if (!added) {
        PdhCloseQuery(query);
        free(counters);
        goto fail;
    }

    /* Collect baseline, wait, collect again */
    PdhCollectQueryData(query);
    Sleep(200);
    if (PdhCollectQueryData(query) != ERROR_SUCCESS) {
        PdhCloseQuery(query);
        free(counters);
        goto fail;
    }

    /* Sum utilization per physical engine across processes */
    double eng_sum[WINMEM_MAX_ENGINES];
    int    eng_id[WINMEM_MAX_ENGINES];
    int    eng_count = 0;
    int    any_read = 0;

    for (int i = 0; i < path_count; i++) {
        PDH_FMT_COUNTERVALUE val;
        if (PdhGetFormattedCounterValue(counters[i],
                                        PDH_FMT_DOUBLE | PDH_FMT_NOCAP100,
                                        NULL, &val) == ERROR_SUCCESS &&
            val.CStatus == ERROR_SUCCESS) {
            int e;
            any_read = 1;
            for (e = 0; e < eng_count; e++)
                if (eng_id[e] == eng_of_path[i])
                    break;
            if (e == eng_count) {
                if (eng_count >= WINMEM_MAX_ENGINES)
                    continue;
                eng_id[eng_count] = eng_of_path[i];
                eng_sum[eng_count] = 0.0;
                eng_count++;
            }
            eng_sum[e] += val.doubleValue;
        }
    }

    PdhCloseQuery(query);
    free(counters);
    for (int i = 0; i < path_count; i++)
        free(paths[i]);
    free(paths);
    free(eng_of_path);

    if (!any_read || eng_count == 0)
        return -1;

    /* The adapter's utilization is its busiest engine */
    double max_util = 0.0;
    for (int e = 0; e < eng_count; e++)
        if (eng_sum[e] > max_util)
            max_util = eng_sum[e];

    if (max_util > 100.0) max_util = 100.0;
    if (max_util < 0.0)   max_util = 0.0;
    *util_pct = (int)(max_util + 0.5);
    return 0;

fail:
    free(instances);
    if (paths) {
        for (int i = 0; i < path_count; i++)
            free(paths[i]);
        free(paths);
    }
    free(eng_of_path);
    return -1;
}

#endif /* _WIN32 */
