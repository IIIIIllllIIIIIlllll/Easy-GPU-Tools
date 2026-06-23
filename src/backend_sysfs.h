#ifndef BACKEND_SYSFS_H
#define BACKEND_SYSFS_H

#include <stdint.h>

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

/* Match a Vulkan physical device by PCI bus topology
 * (domain:bus:device.function). Needed to distinguish multiple identical
 * AMD GPUs (same vendor+device, different PCI slot).
 * Returns 0 and sets *gpu_index on success, -1 if not found. */
int sysfs_find_by_pci_topology(uint32_t domain, uint32_t bus,
                               uint32_t device, uint32_t function,
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

/* Total VRAM / GTT memory (bytes) from mem_info_vram_total / mem_info_gtt_total. */
int sysfs_get_vram_total(int idx, uint64_t *bytes);
int sysfs_get_gtt_total(int idx, uint64_t *bytes);

/* Used GTT memory in bytes from mem_info_gtt_used (for UMA/iGPU total usage). */
int sysfs_get_gtt_used(int idx, uint64_t *bytes);

/* Used and total VRAM in bytes from mem_info_vram_used / mem_info_vram_total. */
int sysfs_get_memory(int idx, uint64_t *used_bytes, uint64_t *total_bytes);

#else  /* Windows / macOS -- everything is a no-op */

static inline int  sysfs_init(void) { return -1; }
static inline void sysfs_shutdown(void) {}
static inline int  sysfs_find_by_vendor_device(int v, int d, int *i)
    { (void)v; (void)d; (void)i; return -1; }
static inline int  sysfs_find_by_pci_topology(uint32_t dom, uint32_t b,
                                              uint32_t d, uint32_t f, int *i)
    { (void)dom; (void)b; (void)d; (void)f; (void)i; return -1; }
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
static inline int  sysfs_get_vram_total(int i, uint64_t *b)
    { (void)i; (void)b; return -1; }
static inline int  sysfs_get_gtt_total(int i, uint64_t *b)
    { (void)i; (void)b; return -1; }
static inline int  sysfs_get_gtt_used(int i, uint64_t *b)
    { (void)i; (void)b; return -1; }
static inline int  sysfs_get_memory(int i, uint64_t *used, uint64_t *total)
    { (void)i; (void)used; (void)total; return -1; }

#endif /* __linux__ */

#ifdef __cplusplus
}
#endif

#endif /* BACKEND_SYSFS_H */
