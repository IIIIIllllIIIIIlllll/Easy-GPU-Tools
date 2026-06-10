#ifndef BACKEND_SYSFS_H
#define BACKEND_SYSFS_H

/* ================================================================
 *  backend_sysfs  --  Linux sysfs GPU dynamic info
 *
 *  Reads temperature, frequency, utilisation and power directly
 *  from the sysfs cardN/device directory (AMDGPU / RADV).
 *
 *  Windows: everything compiles to no-op stubs.
 * ================================================================ */

#ifdef __cplusplus
extern "C" {
#endif

#ifdef __linux__

/* Scan /sys/class/drm/ and cache matching card paths.
 * Returns 0 on success, -1 if no cards found. */
int sysfs_init(void);

/* Free cached data. */
void sysfs_shutdown(void);

/* Match a Vulkan physical device (PCI VID + DID) to a DRM card.
 * Returns 0 and sets *gpu_index on success, -1 if not found. */
int sysfs_find_by_vendor_device(int vendor_id, int device_id,
                                int *gpu_index);

/* GPU edge temperature in millidegrees Celsius. 0 on success. */
int sysfs_get_temperature(int idx, int *milli_c);

/* GPU utilisation (0-100 %). 0 on success. */
int sysfs_get_utilization(int idx, int *percent);

/* Core / memory clocks in MHz. 0 on success. */
int sysfs_get_clocks(int idx, int *core_mhz, int *mem_mhz);

/* Power consumption in milliwatts. 0 on success. */
int sysfs_get_power(int idx, int *milliwatts);

/* Fan speed: RPM and percent. 0 on success. */
int sysfs_get_fan(int idx, int *rpm, int *percent);

#else  /* Windows / macOS -- everything is a no-op */

static inline int  sysfs_init(void) { return -1; }
static inline void sysfs_shutdown(void) {}
static inline int  sysfs_find_by_vendor_device(int v, int d, int *i)
    { (void)v; (void)d; (void)i; return -1; }
static inline int  sysfs_get_temperature(int i, int *t)
    { (void)i; (void)t; return -1; }
static inline int  sysfs_get_utilization(int i, int *p)
    { (void)i; (void)p; return -1; }
static inline int  sysfs_get_clocks(int i, int *c, int *m)
    { (void)i; (void)c; (void)m; return -1; }
static inline int  sysfs_get_power(int i, int *m)
    { (void)i; (void)m; return -1; }
static inline int  sysfs_get_fan(int i, int *r, int *p)
    { (void)i; (void)r; (void)p; return -1; }

#endif /* __linux__ */

#ifdef __cplusplus
}
#endif

#endif /* BACKEND_SYSFS_H */
