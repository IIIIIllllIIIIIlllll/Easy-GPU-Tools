#ifndef BACKEND_ADLX_H
#define BACKEND_ADLX_H

#ifdef __cplusplus
extern "C" {
#endif

#ifdef _WIN32

/* Initialize ADLX (load amdadlx64.dll at runtime, enumerate GPUs).
 * Returns 0 on success, negative on failure.
 * Safe to call repeatedly; a failed init is sticky until shutdown. */
int adlx_init(void);

/* Shutdown ADLX and release all interfaces. Safe to call anytime. */
void adlx_shutdown(void);

/* Non-zero when ADLX initialized successfully and at least one GPU
 * was enumerated. */
int adlx_available(void);

/* Find an ADLX GPU by PCI bus/device/function (when the driver exposes
 * the mapping). Returns 0 and sets *idx on success, -1 if not found. */
int adlx_find_by_pci_topology(uint32_t domain, uint32_t bus,
                              uint32_t device, uint32_t function,
                              int *idx);

/* Find an ADLX GPU by PCI vendor/device ID (exact match).
 * Returns 0 and sets *idx on success, -1 if not found. */
int adlx_find_by_pci(uint32_t vendor_id, uint32_t device_id, int *idx);

/* All getters below return 0 when the value was obtained, -1 when the
 * metric is unsupported or the query failed.  Outputs are only written
 * on success. */

/* Temperature in millidegrees Celsius. */
int adlx_get_temperature(int idx, int *temp_milli_c);

/* Fan speed; percent may stay -1 when the driver only reports RPM. */
int adlx_get_fan_speed(int idx, int *rpm, int *percent);

/* GPU utilization percent (0-100). */
int adlx_get_utilization(int idx, int *pct);

/* Core and VRAM clocks in MHz; either output may stay -1 when only
 * one clock is supported. */
int adlx_get_clocks(int idx, int *core_mhz, int *mem_mhz);

/* ASIC/board power in milliwatts. */
int adlx_get_power(int idx, int *power_milliwatts);

/* VRAM usage in bytes; either output may stay 0 when unsupported.
 * Total comes from the driver (differs from the Vulkan-visible size
 * on APUs). */
int adlx_get_memory(int idx, uint64_t *used_bytes, uint64_t *total_bytes);

#else  /* non-Windows stubs */

static inline int adlx_init(void)   { return -1; }
static inline void adlx_shutdown(void) {}
static inline int adlx_available(void) { return 0; }
static inline int adlx_find_by_pci_topology(uint32_t dom, uint32_t b,
                                            uint32_t d, uint32_t f, int *i)
    { (void)dom; (void)b; (void)d; (void)f; (void)i; return -1; }
static inline int adlx_find_by_pci(uint32_t v, uint32_t d, int *i)
    { (void)v; (void)d; (void)i; return -1; }
static inline int adlx_get_temperature(int idx, int *t)
    { (void)idx; (void)t; return -1; }
static inline int adlx_get_fan_speed(int idx, int *r, int *p)
    { (void)idx; (void)r; (void)p; return -1; }
static inline int adlx_get_utilization(int idx, int *p)
    { (void)idx; (void)p; return -1; }
static inline int adlx_get_clocks(int idx, int *c, int *m)
    { (void)idx; (void)c; (void)m; return -1; }
static inline int adlx_get_power(int idx, int *w)
    { (void)idx; (void)w; return -1; }
static inline int adlx_get_memory(int idx, uint64_t *u, uint64_t *t)
    { (void)idx; (void)u; (void)t; return -1; }

#endif /* _WIN32 */

#ifdef __cplusplus
}
#endif

#endif /* BACKEND_ADLX_H */
