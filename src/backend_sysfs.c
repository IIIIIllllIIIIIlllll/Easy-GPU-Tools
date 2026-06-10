/* ================================================================
 *  backend_sysfs.c  --  Linux /sys/class/drm/ GPU monitor
 *
 *  Enumerates DRM cards, matches them to Vulkan devices by
 *  PCI vendor / device ID, then queries temperature, utilisation,
 *  clocks, power and fan through standard sysfs nodes.
 *
 *  Requires the amdgpu kernel driver (CONFIG_DRM_AMDGPU).
 * ================================================================ */

#ifdef __linux__

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <dirent.h>
#include <sys/types.h>

#include "backend_sysfs.h"

/* ------------------------------------------------------------------
 *  Internal structures & constants
 * ------------------------------------------------------------------ */

#define MAX_GPUS   16
#define PATH_MAX_L 256

typedef struct {
    int  vendor_id;
    int  device_id;
    char card_path[PATH_MAX_L];   /* /sys/class/drm/cardN         */
    char hwmon_path[PATH_MAX_L];  /* .../device/hwmon/hwmonX     */
    char dev_path[PATH_MAX_L];    /* .../device                   */
    int  valid;
} SysfsGpu;

static SysfsGpu g_gpus[MAX_GPUS];
static int      g_num_gpus = 0;
static int      g_init_ok  = 0;

/* ------------------------------------------------------------------
 *  Helpers
 * ------------------------------------------------------------------ */

/* Read an integer from a sysfs file.  Returns 0 on success. */
static int read_int(const char *path, int *out)
{
    FILE *f = fopen(path, "r");
    if (!f) return -1;
    if (fscanf(f, "%d", out) != 1) { fclose(f); return -1; }
    fclose(f);
    return 0;
}

/* Read an unsigned long long (for freq/power — can exceed 2^31). */
static int read_ull(const char *path, unsigned long long *out)
{
    FILE *f = fopen(path, "r");
    if (!f) return -1;
    if (fscanf(f, "%llu", out) != 1) { fclose(f); return -1; }
    fclose(f);
    return 0;
}

/* Read hex PCI ID "0x1002" style string, store as int. */
static int read_pci_id(const char *path, int *out)
{
    unsigned int val;
    FILE *f = fopen(path, "r");
    if (!f) return -1;
    if (fscanf(f, "0x%x", &val) != 1) { fclose(f); return -1; }
    fclose(f);
    *out = (int)val;
    return 0;
}

/* Build path sprintf helper */
static void make_path(char *dst, size_t sz, const char *dir, const char *file)
{
    snprintf(dst, sz, "%s/%s", dir, file);
}

/* Find the first hwmon directory under <dev_path>/hwmon/. */
static int find_hwmon(const char *dev_path, char *out, size_t out_sz)
{
    char hwmon_dir[PATH_MAX_L + 64];
    DIR *d;
    struct dirent *de;

    snprintf(hwmon_dir, sizeof(hwmon_dir), "%s/hwmon", dev_path);
    d = opendir(hwmon_dir);
    if (!d) return -1;

    while ((de = readdir(d)) != NULL) {
        if (strncmp(de->d_name, "hwmon", 5) == 0) {
            snprintf(out, out_sz, "%s/%s", hwmon_dir, de->d_name);
            closedir(d);
            return 0;
        }
    }
    closedir(d);
    return -1;
}

/* ------------------------------------------------------------------
 *  sysfs_init  --  scan /sys/class/drm/ for render-capable cards
 * ------------------------------------------------------------------ */

int sysfs_init(void)
{
    DIR *drm;
    struct dirent *de;
    int i;

    if (g_init_ok) return 0;

    drm = opendir("/sys/class/drm");
    if (!drm) { g_init_ok = -1; return -1; }

    g_num_gpus = 0;
    memset(g_gpus, 0, sizeof(g_gpus));

    while ((de = readdir(drm)) != NULL) {
        char dev_path[PATH_MAX_L + 64];
        char vendor_path[PATH_MAX_L + 64];
        char test_path[PATH_MAX_L + 64];
        int vendor_id, device_id;

        /* only process cardN (skip renderD*, version, etc.) */
        if (strncmp(de->d_name, "card", 4) != 0) continue;

        snprintf(dev_path, sizeof(dev_path),
                 "/sys/class/drm/%s/device", de->d_name);

        /* check that vendor file exists */
        snprintf(vendor_path, sizeof(vendor_path),
                 "%s/vendor", dev_path);
        FILE *f = fopen(vendor_path, "r");
        if (!f) continue;   /* not a PCI device, skip */
        fclose(f);

        if (read_pci_id(vendor_path, &vendor_id) != 0) continue;

        snprintf(test_path, sizeof(test_path), "%s/device", dev_path);
        if (read_pci_id(test_path, &device_id) != 0) continue;

        if (g_num_gpus >= MAX_GPUS) break;

        i = g_num_gpus;
        g_gpus[i].vendor_id = vendor_id;
        g_gpus[i].device_id = device_id;
        snprintf(g_gpus[i].card_path, PATH_MAX_L,
                 "/sys/class/drm/%s", de->d_name);
        snprintf(g_gpus[i].dev_path, PATH_MAX_L, "%s", dev_path);
        g_gpus[i].hwmon_path[0] = '\0';

        /* locate hwmon */
        find_hwmon(dev_path, g_gpus[i].hwmon_path,
                   sizeof(g_gpus[i].hwmon_path));

        g_gpus[i].valid = 1;
        g_num_gpus++;
    }
    closedir(drm);

    if (g_num_gpus == 0) { g_init_ok = -1; return -1; }

    g_init_ok = 1;
    return 0;
}

void sysfs_shutdown(void)
{
    memset(g_gpus, 0, sizeof(g_gpus));
    g_num_gpus = 0;
    g_init_ok  = 0;
}

/* ------------------------------------------------------------------
 *  Matching
 * ------------------------------------------------------------------ */

int sysfs_find_by_vendor_device(int vendor_id, int device_id, int *gpu_index)
{
    int i;
    if (g_init_ok <= 0) return -1;
    for (i = 0; i < g_num_gpus; i++) {
        if (!g_gpus[i].valid) continue;
        if (g_gpus[i].vendor_id == vendor_id &&
            g_gpus[i].device_id == device_id) {
            *gpu_index = i;
            return 0;
        }
    }
    return -1;
}

/* ------------------------------------------------------------------
 *  sysfs query helpers -- read a file under dev_path or hwmon_path
 * ------------------------------------------------------------------ */

#define TRY_READ(dst, path)  (read_int(path, dst) == 0)

int sysfs_get_temperature(int idx, int *milli_c)
{
    char p[PATH_MAX_L + 64];
    if (idx < 0 || idx >= g_num_gpus || !g_gpus[idx].valid)
        return -1;

    if (g_gpus[idx].hwmon_path[0]) {
        make_path(p, sizeof(p), g_gpus[idx].hwmon_path, "temp1_input");
        if (TRY_READ(milli_c, p)) return 0;
    }
    return -1;
}

int sysfs_get_utilization(int idx, int *percent)
{
    char p[PATH_MAX_L + 64];
    if (idx < 0 || idx >= g_num_gpus || !g_gpus[idx].valid)
        return -1;

    make_path(p, sizeof(p), g_gpus[idx].dev_path, "gpu_busy_percent");
    return TRY_READ(percent, p) ? 0 : -1;
}

int sysfs_get_clocks(int idx, int *core_mhz, int *mem_mhz)
{
    char p[PATH_MAX_L + 64];
    unsigned long long val;
    int ok = 0;

    if (idx < 0 || idx >= g_num_gpus || !g_gpus[idx].valid)
        return -1;

    if (g_gpus[idx].hwmon_path[0]) {
        make_path(p, sizeof(p), g_gpus[idx].hwmon_path, "freq1_input");
        if (read_ull(p, &val) == 0) {
            *core_mhz = (int)(val / 1000000ULL); /* Hz -> MHz */
            ok = 1;
        }
        make_path(p, sizeof(p), g_gpus[idx].hwmon_path, "freq2_input");
        if (read_ull(p, &val) == 0) {
            *mem_mhz = (int)(val / 1000000ULL);
        } else {
            *mem_mhz = -1;
        }
    }
    return ok ? 0 : -1;
}

int sysfs_get_power(int idx, int *milliwatts)
{
    char p[PATH_MAX_L + 64];
    unsigned long long val;

    if (idx < 0 || idx >= g_num_gpus || !g_gpus[idx].valid)
        return -1;

    if (g_gpus[idx].hwmon_path[0]) {
        make_path(p, sizeof(p), g_gpus[idx].hwmon_path, "power1_average");
        if (read_ull(p, &val) == 0) {
            *milliwatts = (int)(val / 1000ULL); /* uW -> mW */
            return 0;
        }
    }
    return -1;
}

int sysfs_get_fan(int idx, int *rpm, int *percent)
{
    char p[PATH_MAX_L + 64];

    if (idx < 0 || idx >= g_num_gpus || !g_gpus[idx].valid)
        return -1;

    if (g_gpus[idx].hwmon_path[0]) {
        make_path(p, sizeof(p), g_gpus[idx].hwmon_path, "fan1_input");
        if (TRY_READ(rpm, p)) {
            make_path(p, sizeof(p), g_gpus[idx].hwmon_path, "pwm1");
            read_int(p, percent); /* optional, may fail */
            return 0;
        }
    }
    return -1;
}

#endif /* __linux__ */
