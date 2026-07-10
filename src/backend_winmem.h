#ifndef BACKEND_WINMEM_H
#define BACKEND_WINMEM_H

/* backend_winmem.c -- Windows-native GPU memory & utilization via PDH
 * (Performance Data Helper) performance counters.
 *
 * Reads "\GPU Adapter Memory(luid_*_phys_*)\*" and
 * "\GPU Engine(pid_*_luid_*_phys_*_eng_*)\Utilization Percentage".
 *
 * Used as a fallback/supplement for AMD iGPUs/APUs where ADL's
 * Overdrive APIs don't expose memory usage or utilization.
 *
 * LUID is obtained by the caller, typically from Vulkan's
 * VkPhysicalDeviceIDProperties::deviceLUID on Windows.
 */

#ifdef __cplusplus
extern "C" {
#endif

#ifdef _WIN32

int winmem_init(void);
void winmem_shutdown(void);

/* Get GPU adapter memory usage via PDH "GPU Adapter Memory" counters.
 * All values are in bytes.  Any pointer can be NULL to skip it.
 * Returns 0 on success (at least one value obtained), -1 on failure. */
int winmem_get_memory(uint32_t luid_low, uint32_t luid_high,
                      uint64_t *dedicated_used,
                      uint64_t *shared_used,
                      uint64_t *total_committed);

/* Get GPU utilization percentage via PDH "GPU Engine" counters.
 * Aggregates utilization across engine types (3D, compute, copy,
 * video) for the given LUID, taking the max across engines.
 * Returns 0 and sets *util_pct (0-100) on success, -1 on failure. */
int winmem_get_utilization(uint32_t luid_low, uint32_t luid_high,
                           int *util_pct);

#else  /* non-Windows stubs */

static inline int winmem_init(void) { return -1; }
static inline void winmem_shutdown(void) {}
static inline int winmem_get_memory(uint32_t lo, uint32_t hi,
                                     uint64_t *du, uint64_t *su, uint64_t *tc)
    { (void)lo; (void)hi; (void)du; (void)su; (void)tc; return -1; }
static inline int winmem_get_utilization(uint32_t lo, uint32_t hi, int *u)
    { (void)lo; (void)hi; (void)u; return -1; }

#endif /* _WIN32 */

#ifdef __cplusplus
}
#endif

#endif /* BACKEND_WINMEM_H */