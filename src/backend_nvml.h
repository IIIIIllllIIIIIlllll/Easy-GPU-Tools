#ifndef BACKEND_NVML_H
#define BACKEND_NVML_H

/* ================================================================
 *  backend_nvml  --  NVIDIA Management Library dynamic backend
 *
 *  Loads nvml.dll (Windows) or libnvidia-ml.so (Linux) at runtime.
 *  If the library is not found, all calls degrade to no-ops.
 *
 *  The same .c source compiles on Windows and Linux -- only the
 *  loading path (LoadLibrary vs dlopen) differs.
 * ================================================================ */

#ifdef __cplusplus
extern "C" {
#endif

/* Initialize NVML, enumerate devices, cache PCI info.
 * Returns 0 on success, -1 if library not found or init fails. */
int nvml_init(void);

/* Shutdown NVML and unload library. Safe to call anytime. */
void nvml_shutdown(void);

/* Match a Vulkan NVIDIA device by PCI device ID.
 * Returns 0 and sets *index on success, -1 if not found. */
int nvml_find_by_device_id(unsigned int device_id, int *index);

/* GPU edge temperature in degrees Celsius. 0 = success. */
int nvml_get_temperature(int idx, int *celsius);

/* GPU and memory utilisation (0-100 %). 0 = success. */
int nvml_get_utilization(int idx, int *gpu_pct, int *mem_pct);

/* SM clock and memory clock in MHz. 0 = success. */
int nvml_get_clocks(int idx, int *sm_mhz, int *mem_mhz);

/* Current power usage and power limit in watts. 0 = success. */
int nvml_get_power(int idx, int *usage_w, int *limit_w);

/* Memory: used and total in megabytes. 0 = success. */
int nvml_get_memory(int idx, unsigned int *used_mb,
                    unsigned int *total_mb);

/* Fan speed in percent (may be N/A on passive-cooled GPUs).
 * 0 = success. */
int nvml_get_fan(int idx, int *percent);

/* Current performance state (P0 ~ P15, lower = faster).
 * 0 = success. */
int nvml_get_perf_state(int idx, int *pstate);

/* Whether ECC is currently enabled (1=yes, 0=no).
 * 0 = success. */
int nvml_get_ecc(int idx, int *enabled);

/* Driver version string (e.g. "580.159.03"). 0 = success. */
int nvml_get_driver_version(char *buf, size_t buf_size);

#ifdef __cplusplus
}
#endif

#endif /* BACKEND_NVML_H */
