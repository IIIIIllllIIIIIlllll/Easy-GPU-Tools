#ifndef BACKEND_ADL_H
#define BACKEND_ADL_H

#ifdef __cplusplus
extern "C" {
#endif

#ifdef _WIN32

/* Initialize ADL (load atiadlxx.dll, create context).
 * Returns 0 on success, negative on failure. */
int adl_init(void);

/* Shutdown ADL, free resources. Safe to call even if init failed. */
void adl_shutdown(void);

/* Find an ADL adapter by GPU name (case-insensitive substring match).
 * Returns 0 and sets *adl_index on success, -1 if not found. */
int adl_find_by_name(const char *name, int *adl_index);

/* ADL_Overdrive5_Temperature_Get.
 * Returns temperature in millidegrees Celsius. 0 on success, -1 on failure. */
int adl_get_temperature(int adapter_index, int *temp_milli_c);

/* ADL_Overdrive5_FanSpeed_Get.
 * Returns fan RPM and percentage. 0 on success, -1 on failure. */
int adl_get_fan_speed(int adapter_index, int *rpm, int *percent);

/* ADL_Overdrive5_CurrentActivity_Get.
 * engine_clock:    in 10 kHz (divide by 100 -> MHz)
 * memory_clock:    in 10 kHz (divide by 100 -> MHz)
 * activity_percent: GPU utilization (0-100)
 * perf_level:       current performance level / P-State (0=low, higher=perf)
 * Returns 0 on success, -1 on failure. */
int adl_get_activity(int adapter_index,
                     int *engine_clock_10khz,
                     int *memory_clock_10khz,
                     int *activity_percent,
                     int *perf_level);

/* ADL2_Adapter_Speed_Get.
 * Returns core clock (MHz) and memory clock (MHz).
 * Works even on APU integrated graphics.
 * Returns 0 on success, -1 on failure. */
int adl_get_speed(int adapter_index, int *core_mhz, int *mem_mhz);

#else  /* Linux / non-Windows stubs */

static inline int adl_init(void)           { return -1; }
static inline void adl_shutdown(void)      {}
static inline int adl_find_by_name(const char *n, int *i)
    { (void)n; (void)i; return -1; }
static inline int adl_get_temperature(int idx, int *t)
    { (void)idx; (void)t; return -1; }
static inline int adl_get_fan_speed(int idx, int *r, int *p)
    { (void)idx; (void)r; (void)p; return -1; }
static inline int adl_get_activity(int idx, int *e, int *m, int *a, int *l)
    { (void)idx; (void)e; (void)m; (void)a; (void)l; return -1; }
static inline int adl_get_speed(int idx, int *c, int *m)
    { (void)idx; (void)c; (void)m; return -1; }

#endif /* _WIN32 */

#ifdef __cplusplus
}
#endif

#endif /* BACKEND_ADL_H */
