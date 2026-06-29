#ifndef BACKEND_INTEL_H
#define BACKEND_INTEL_H

#include <stdint.h>

/* ================================================================
 *  backend_intel  --  Intel Graphics Control Library (IGCL)
 *
 *  Loads ControlLib.dll (Windows) at runtime to query Intel Arc
 *  discrete GPUs and Iris Xe / UHD integrated graphics.
 *
 *  Linux: everything compiles to no-op stubs (Intel sysfs queries
 *  are handled by backend_sysfs).
 * ================================================================ */

#ifdef __cplusplus
extern "C" {
#endif

#ifdef _WIN32

/* Initialize IGCL (load ControlLib.dll, enumerate adapters).
 * Returns 0 on success, -1 if library not found or init fails. */
int intel_init(void);

/* Shutdown IGCL, free resources. Safe to call even if init failed. */
void intel_shutdown(void);

/* Find an IGCL adapter by PCI vendor/device ID (exact match).
 * Returns 0 and sets *index on success, -1 if not found. */
int intel_find_by_pci(uint32_t vendor_id, uint32_t device_id, int *index);

/* Find an IGCL adapter by PCI bus topology (domain:bus:device.function).
 * IGCL adapter_bdf has no domain field, so domain is accepted but
 * ignored; matching is on bus/device/function. Needed to distinguish
 * multiple identical Intel GPUs (same vendor+device, different PCI slot).
 * Returns 0 and sets *index on success, -1 if not found. */
int intel_find_by_pci_topology(uint32_t domain, uint32_t bus,
                               uint32_t device, uint32_t function,
                               int *index);

/* GPU temperature in millidegrees Celsius. 0 on success, -1 on failure. */
int intel_get_temperature(int idx, int *milli_c);

/* GPU utilisation (0-100 %). mem_pct may be N/A (-1) because IGCL has
 * no direct memory utilisation percentage. 0 on success, -1 on failure.
 * Uses two-sample delta of engine active-time counters; the first call
 * after init uses the pre-sampled baseline from intel_init. */
int intel_get_utilization(int idx, int *gpu_pct, int *mem_pct);

/* Core (GPU) and memory clocks in MHz. mem_mhz may be N/A (-1) on iGPUs.
 * 0 on success, -1 on failure. */
int intel_get_clocks(int idx, int *core_mhz, int *mem_mhz);

/* Current power usage and power limit in milliwatts.
 * usage uses two-sample delta of the energy counter (first call after
 * init uses the pre-sampled baseline). limit comes from power properties.
 * 0 on success, -1 on failure. */
int intel_get_power(int idx, int *usage_mw, int *limit_mw);

/* Memory: used and total in bytes. 0 on success, -1 on failure. */
int intel_get_memory(int idx, uint64_t *used_bytes, uint64_t *total_bytes);

/* Fan speed: RPM and percent. Only Arc discrete GPUs have fans;
 * iGPUs return -1. 0 on success, -1 on failure. */
int intel_get_fan(int idx, int *rpm, int *percent);

#else  /* Linux / non-Windows stubs */

static inline int  intel_init(void)          { return -1; }
static inline void intel_shutdown(void)      {}
static inline int  intel_find_by_pci(uint32_t v, uint32_t d, int *i)
    { (void)v; (void)d; (void)i; return -1; }
static inline int  intel_find_by_pci_topology(uint32_t dom, uint32_t b,
                                              uint32_t d, uint32_t f, int *i)
    { (void)dom; (void)b; (void)d; (void)f; (void)i; return -1; }
static inline int  intel_get_temperature(int i, int *t)
    { (void)i; (void)t; return -1; }
static inline int  intel_get_utilization(int i, int *g, int *m)
    { (void)i; (void)g; (void)m; return -1; }
static inline int  intel_get_clocks(int i, int *c, int *m)
    { (void)i; (void)c; (void)m; return -1; }
static inline int  intel_get_power(int i, int *u, int *l)
    { (void)i; (void)u; (void)l; return -1; }
static inline int  intel_get_memory(int i, uint64_t *u, uint64_t *t)
    { (void)i; (void)u; (void)t; return -1; }
static inline int  intel_get_fan(int i, int *r, int *p)
    { (void)i; (void)r; (void)p; return -1; }

#endif /* _WIN32 */

#ifdef __cplusplus
}
#endif

#endif /* BACKEND_INTEL_H */
