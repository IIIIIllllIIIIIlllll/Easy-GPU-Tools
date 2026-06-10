#ifndef GPU_INFO_H
#define GPU_INFO_H

#include <vulkan/vulkan.h>
#include <stdint.h>

#define GPU_INFO_SENTINEL (-1)

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    /* identification */
    char     device_name[VK_MAX_PHYSICAL_DEVICE_NAME_SIZE];
    uint32_t vendor_id;
    uint32_t device_id;
    VkPhysicalDeviceType device_type;

    /* memory */
    uint64_t dedicated_vram_bytes;
    uint64_t shared_ram_bytes;
    uint32_t heap_count;
    uint64_t heap_sizes[VK_MAX_MEMORY_HEAPS];
    VkMemoryHeapFlags heap_flags[VK_MAX_MEMORY_HEAPS];

    /* vulkan versions (raw encoding) */
    uint32_t api_version;
    uint32_t driver_version;

    /* unified sensor data — GPU_INFO_SENTINEL or 0 means N/A */
    char     sensor_backend[16];
    int      temperature_milli_c;
    int      utilization_gpu_pct;
    int      utilization_mem_pct;
    int      core_clock_mhz;
    int      mem_clock_mhz;
    int      power_milliwatts;
    int      power_limit_milliwatts;
    int      fan_speed_rpm;
    int      fan_speed_pct;
    int      perf_state;
    int      ecc_enabled;
    uint64_t mem_used_bytes;      /* 0 when N/A */
    uint64_t mem_total_bytes;     /* 0 when N/A */
    char     driver_version_str[64];
} GpuInfo;

#ifdef __cplusplus
}
#endif

#endif /* GPU_INFO_H */
