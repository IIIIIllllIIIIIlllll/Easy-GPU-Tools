#include <vulkan/vulkan.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#ifdef _WIN32
#include <windows.h>
#endif

#include "gpu_info.h"
#include "sys_info.h"
#include "backend_adl.h"
#include "backend_sysfs.h"
#include "backend_nvml.h"
#include "backend_intel.h"
#include "backend_sysinfo.h"
#include "backend_vulkan.h"
#include "backend_winmem.h"

/* Redirect Vulkan API calls through dynamically loaded function pointers */
#define vkCreateInstance                vulkan_CreateInstance
#define vkDestroyInstance               vulkan_DestroyInstance
#define vkEnumeratePhysicalDevices      vulkan_EnumeratePhysicalDevices
#define vkGetPhysicalDeviceProperties   vulkan_GetPhysicalDeviceProperties
#define vkGetPhysicalDeviceProperties2  vulkan_GetPhysicalDeviceProperties2
#define vkGetPhysicalDeviceMemoryProperties vulkan_GetPhysicalDeviceMemoryProperties

/* ================================================================
 *  Utility helpers
 * ================================================================ */

static const char* vendor_name(uint32_t vid)
{
    switch (vid) {
    case 0x10DE: return "NVIDIA";
    case 0x1002: return "AMD";
    case 0x8086: return "Intel";
    case 0x1414: return "Microsoft";
    case 0x5143: return "Qualcomm";
    case 0x13B5: return "ARM";
    case 0x1AB8: return "Parallels";
    case 0x1010: return "ImgTec";
    case 0x10005: return "Mesa llvmpipe";
    default:     return "Unknown";
    }
}

static const char* device_type_str(VkPhysicalDeviceType type)
{
    switch (type) {
    case VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU: return "Integrated GPU";
    case VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU:   return "Discrete GPU";
    case VK_PHYSICAL_DEVICE_TYPE_VIRTUAL_GPU:    return "Virtual GPU";
    case VK_PHYSICAL_DEVICE_TYPE_CPU:            return "CPU";
    default:                                     return "Other";
    }
}

static void format_bytes(uint64_t bytes, char *buf, size_t buf_size)
{
    if (bytes >= 1073741824ULL) {
        snprintf(buf, buf_size, "%.2f GB", (double)bytes / 1073741824.0);
    } else if (bytes >= 1048576ULL) {
        snprintf(buf, buf_size, "%.2f MB", (double)bytes / 1048576.0);
    } else if (bytes >= 1024ULL) {
        snprintf(buf, buf_size, "%.2f KB", (double)bytes / 1024.0);
    } else {
        snprintf(buf, buf_size, "%llu B", (unsigned long long)bytes);
    }
}

static void format_api_ver(uint32_t ver, char *buf, size_t buf_size)
{
    snprintf(buf, buf_size, "%u.%u.%u",
             VK_VERSION_MAJOR(ver),
             VK_VERSION_MINOR(ver),
             VK_VERSION_PATCH(ver));
}

static void format_driver_ver(uint32_t ver, uint32_t vendor_id,
                               char *buf, size_t buf_size)
{
    switch (vendor_id) {
    case 0x10DE: /* NVIDIA: (major<<22)|(minor<<14)|(patch<<6)|build */
        snprintf(buf, buf_size, "%u.%u.%u.%u",
                 (ver >> 22) & 0x3FF,
                 (ver >> 14) & 0x0FF,
                 (ver >> 6)  & 0x0FF,
                 ver & 0x3F);
        break;
    case 0x8086: /* Intel Mesa: similar to NVIDIA but patch is 14 bits */
        snprintf(buf, buf_size, "%u.%u.%u",
                 VK_VERSION_MAJOR(ver),
                 VK_VERSION_MINOR(ver),
                 VK_VERSION_PATCH(ver));
        break;
    default:
        /* AMD / others: use standard VK_VERSION encoding */
        snprintf(buf, buf_size, "%u.%u.%u",
                 VK_VERSION_MAJOR(ver),
                 VK_VERSION_MINOR(ver),
                 VK_VERSION_PATCH(ver));
        break;
    }
}

static void format_uptime(uint64_t seconds, char *buf, size_t buf_size)
{
    int days    = (int)(seconds / 86400);
    int hours   = (int)((seconds % 86400) / 3600);
    int minutes = (int)((seconds % 3600) / 60);
    int secs    = (int)(seconds % 60);
    if (days > 0)
        snprintf(buf, buf_size, "%d day%s %d hour%s %d min%s",
                 days,   days   == 1 ? "" : "s",
                 hours,  hours  == 1 ? "" : "s",
                 minutes,minutes == 1 ? "" : "s");
    else if (hours > 0)
        snprintf(buf, buf_size, "%d hour%s %d min%s %d sec%s",
                 hours,   hours   == 1 ? "" : "s",
                 minutes, minutes == 1 ? "" : "s",
                 secs,    secs    == 1 ? "" : "s");
    else if (minutes > 0)
        snprintf(buf, buf_size, "%d min%s %d sec%s",
                 minutes, minutes == 1 ? "" : "s",
                 secs,    secs    == 1 ? "" : "s");
    else
        snprintf(buf, buf_size, "%d second%s", secs, secs == 1 ? "" : "s");
}

/* ================================================================
 *  GpuInfo data collection
 * ================================================================ */

static void gpu_init_info(GpuInfo *info)
{
    memset(info, 0, sizeof(GpuInfo));
    info->temperature_milli_c      = GPU_INFO_SENTINEL;
    info->utilization_gpu_pct      = GPU_INFO_SENTINEL;
    info->utilization_mem_pct      = GPU_INFO_SENTINEL;
    info->core_clock_mhz           = GPU_INFO_SENTINEL;
    info->mem_clock_mhz            = GPU_INFO_SENTINEL;
    info->power_milliwatts         = GPU_INFO_SENTINEL;
    info->power_limit_milliwatts   = GPU_INFO_SENTINEL;
    info->fan_speed_rpm            = GPU_INFO_SENTINEL;
    info->fan_speed_pct            = GPU_INFO_SENTINEL;
    info->perf_state               = GPU_INFO_SENTINEL;
    info->ecc_enabled              = GPU_INFO_SENTINEL;
}

static void gpu_collect_info(GpuInfo *info, VkPhysicalDevice device)
{
    VkPhysicalDeviceProperties2 props2 = {0};
    VkPhysicalDevicePCIBusInfoPropertiesEXT pci_props = {0};
    VkPhysicalDeviceIDProperties id_props = {0};
    VkPhysicalDeviceMemoryProperties mem;

    gpu_init_info(info);

    props2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2;
    props2.pNext = &pci_props;
    pci_props.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PCI_BUS_INFO_PROPERTIES_EXT;
    pci_props.pNext = &id_props;
    id_props.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES;
    vkGetPhysicalDeviceProperties2(device, &props2);
    vkGetPhysicalDeviceMemoryProperties(device, &mem);

    VkPhysicalDeviceProperties props = props2.properties;

    /* Extract Windows LUID for PDH performance-counter matching */
    uint32_t luid_low  = 0;
    uint32_t luid_high = 0;
    if (id_props.deviceLUIDValid) {
        luid_low  = ((uint32_t)id_props.deviceLUID[0])       |
                    ((uint32_t)id_props.deviceLUID[1] << 8)  |
                    ((uint32_t)id_props.deviceLUID[2] << 16) |
                    ((uint32_t)id_props.deviceLUID[3] << 24);
        luid_high = ((uint32_t)id_props.deviceLUID[4])       |
                    ((uint32_t)id_props.deviceLUID[5] << 8)  |
                    ((uint32_t)id_props.deviceLUID[6] << 16) |
                    ((uint32_t)id_props.deviceLUID[7] << 24);
    }

    /* identification */
    strncpy(info->device_name, props.deviceName, sizeof(info->device_name) - 1);
    info->vendor_id   = props.vendorID;
    info->device_id   = props.deviceID;
    info->device_type = props.deviceType;

    /* memory sums */
    {
        uint64_t dedicated = 0, shared = 0;
        uint32_t h;
        for (h = 0; h < mem.memoryHeapCount; h++) {
            info->heap_sizes[h] = mem.memoryHeaps[h].size;
            info->heap_flags[h] = mem.memoryHeaps[h].flags;
            if (mem.memoryHeaps[h].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT)
                dedicated += mem.memoryHeaps[h].size;
            else
                shared += mem.memoryHeaps[h].size;
        }
        info->heap_count           = mem.memoryHeapCount;
        info->dedicated_vram_bytes = dedicated;
        info->shared_ram_bytes     = shared;
    }

    /* vulkan versions */
    info->api_version    = props.apiVersion;
    info->driver_version = props.driverVersion;

    /* -- NVIDIA: NVML -------------------------------------------------- */
    if (props.vendorID == 0x10DE) {
        int nv_idx;
        if (nvml_find_by_pci(pci_props.pciDomain, pci_props.pciBus,
                             pci_props.pciDevice, pci_props.pciFunction,
                             &nv_idx) == 0) {
            strncpy(info->sensor_backend, "NVML", sizeof(info->sensor_backend) - 1);

            int temp;
            if (nvml_get_temperature(nv_idx, &temp) == 0)
                info->temperature_milli_c = temp * 1000;

            int gpu_pct, mem_pct;
            if (nvml_get_utilization(nv_idx, &gpu_pct, &mem_pct) == 0) {
                info->utilization_gpu_pct = gpu_pct;
                info->utilization_mem_pct = mem_pct;
            }

            int sm_mhz, mem_clk;
            if (nvml_get_clocks(nv_idx, &sm_mhz, &mem_clk) == 0) {
                info->core_clock_mhz = sm_mhz;
                if (mem_clk >= 0)
                    info->mem_clock_mhz = mem_clk;
            }

            int pwr_usage, pwr_limit;
            if (nvml_get_power(nv_idx, &pwr_usage, &pwr_limit) == 0) {
                if (pwr_usage >= 0)
                    info->power_milliwatts = pwr_usage * 1000;
                if (pwr_limit >= 0)
                    info->power_limit_milliwatts = pwr_limit * 1000;
            }

            int fan_pct;
            if (nvml_get_fan(nv_idx, &fan_pct) == 0)
                info->fan_speed_pct = fan_pct;

            int pstate;
            if (nvml_get_perf_state(nv_idx, &pstate) == 0)
                info->perf_state = pstate;

            int ecc;
            if (nvml_get_ecc(nv_idx, &ecc) == 0)
                info->ecc_enabled = ecc;

            unsigned int mem_used, mem_total;
            if (nvml_get_memory(nv_idx, &mem_used, &mem_total) == 0) {
                info->mem_used_bytes  = (uint64_t)mem_used  * 1048576ULL;
                info->mem_total_bytes = (uint64_t)mem_total * 1048576ULL;
            }

            char drv_ver[64];
            if (nvml_get_driver_version(drv_ver, sizeof(drv_ver)) == 0)
                strncpy(info->driver_version_str, drv_ver,
                        sizeof(info->driver_version_str) - 1);
        }
    }
    /* -- AMD: ADL (Windows) / sysfs (Linux) --------------------------- */
    else if (props.vendorID == 0x1002) {
#if defined(_WIN32)
        int adl_idx;
        /* Prefer PCI topology so two identical AMD GPUs don't collapse
         * onto adapter 0; fall back to vendor/device then name. */
        if (adl_find_by_pci_topology(pci_props.pciDomain, pci_props.pciBus,
                                     pci_props.pciDevice, pci_props.pciFunction,
                                     &adl_idx) != 0 &&
            adl_find_by_pci(props.vendorID, props.deviceID, &adl_idx) != 0 &&
            adl_find_by_name(props.deviceName, &adl_idx) != 0) {
            adl_idx = -1;
        }
        if (adl_idx >= 0) {
            strncpy(info->sensor_backend, "ADL", sizeof(info->sensor_backend) - 1);

            int t;
            if (adl_get_temperature(adl_idx, &t) == 0)
                info->temperature_milli_c = t;

            int rpm, pct;
            if (adl_get_fan_speed(adl_idx, &rpm, &pct) == 0) {
                info->fan_speed_rpm = rpm;
                info->fan_speed_pct = pct;
            } else if (adl_get_pmlog_fan(adl_idx, &rpm, &pct) == 0) {
                if (rpm > 0) info->fan_speed_rpm = rpm;
                if (pct > 0) info->fan_speed_pct = pct;
            }

            int eng, mem_clk, act, lvl;
            if (adl_get_activity(adl_idx, &eng, &mem_clk, &act, &lvl) == 0) {
                info->utilization_gpu_pct = act;
                info->core_clock_mhz      = eng / 100;
                info->mem_clock_mhz       = mem_clk / 100;
                info->perf_state          = lvl;
            } else if (id_props.deviceLUIDValid) {
                /* AMD iGPU/APU fallback: Windows PDH GPU Engine counters */
                int util;
                if (winmem_get_utilization(luid_low, luid_high, &util) == 0) {
                    info->utilization_gpu_pct = util;
                }
            }

            /* PMLog power -- works on iGPUs/APUs where Overdrive power doesn't */
            {
                int pwr;
                if (adl_get_pmlog_power(adl_idx, &pwr) == 0 && pwr > 0)
                    info->power_milliwatts = pwr;
            }

            uint64_t vram_used, vram_total;
            if (adl_get_memory(adl_idx, &vram_used, &vram_total) == 0) {
                info->mem_used_bytes  = vram_used;
                info->mem_total_bytes = vram_total;
            }

            /* memory: for AMD iGPUs ADL only reports the small
             * hardware-reserved segment; use the Vulkan-visible total.
             * Additionally, supplement with PDH Windows counters which
             * provide the true shared usage + total committed budget. */
            if (props.deviceType == VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU) {
                uint64_t vt = info->dedicated_vram_bytes + info->shared_ram_bytes;
                if (vt > info->mem_total_bytes)
                    info->mem_total_bytes = vt;

                if (id_props.deviceLUIDValid) {
                    uint64_t ded = 0, sha = 0, tot = 0;
                    if (winmem_get_memory(luid_low, luid_high,
                                          &ded, &sha, &tot) == 0) {
                        info->mem_used_bytes  = sha + ded;
                        info->mem_total_bytes = tot;
                    }
                }
            }
        }
#elif defined(__linux__)
        int s_idx;
        /* Prefer PCI topology so two identical AMD GPUs don't collapse
         * onto card0; fall back to vendor/device. */
        if (sysfs_find_by_pci_topology(pci_props.pciDomain, pci_props.pciBus,
                                       pci_props.pciDevice, pci_props.pciFunction,
                                       &s_idx) != 0 &&
            sysfs_find_by_vendor_device((int)props.vendorID,
                                        (int)props.deviceID, &s_idx) != 0) {
            s_idx = -1;
        }
        if (s_idx >= 0) {
            strncpy(info->sensor_backend, "sysfs", sizeof(info->sensor_backend) - 1);

            int temp;
            if (sysfs_get_temperature(s_idx, &temp) == 0)
                info->temperature_milli_c = temp;

            int util;
            if (sysfs_get_utilization(s_idx, &util) == 0)
                info->utilization_gpu_pct = util;

            int core_mhz, mm_mhz;
            if (sysfs_get_clocks(s_idx, &core_mhz, &mm_mhz) == 0) {
                info->core_clock_mhz = core_mhz;
                if (mm_mhz >= 0)
                    info->mem_clock_mhz = mm_mhz;
            }

            int power;
            if (sysfs_get_power(s_idx, &power) == 0)
                info->power_milliwatts = power;

            int rpm, pct;
            if (sysfs_get_fan(s_idx, &rpm, &pct) == 0) {
                info->fan_speed_rpm = rpm;
                if (pct >= 0)
                    info->fan_speed_pct = pct;
            }

            uint64_t vram_used_b, vram_total_b;
            if (sysfs_get_memory(s_idx, &vram_used_b, &vram_total_b) == 0) {
                info->mem_used_bytes  = vram_used_b;
                info->mem_total_bytes = vram_total_b;
            }

            /* memory: for iGPUs, use vram+gtt from sysfs (UMA total) */
            if (props.deviceType == VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU) {
                uint64_t vr = 0, gt = 0;
                if (sysfs_get_vram_total(s_idx, &vr) == 0 &&
                    sysfs_get_gtt_total(s_idx, &gt) == 0) {
                    uint64_t st = vr + gt;
                    if (st > info->dedicated_vram_bytes) {
                        uint64_t vt = info->dedicated_vram_bytes + info->shared_ram_bytes;
                        if (st > vt) st = vt;
                        info->dedicated_vram_bytes = st;
                        info->shared_ram_bytes = vt - st;

                        uint64_t gu = 0;
                        if (sysfs_get_gtt_used(s_idx, &gu) == 0)
                            info->mem_used_bytes += gu;
                        info->mem_total_bytes = st;
                    }
                }
            }
        }
#endif
    }
    /* -- Intel: IGCL (Windows) / sysfs (Linux) ----------------------- */
    else if (props.vendorID == 0x8086) {
#if defined(_WIN32)
        int intel_idx;
        /* Prefer PCI topology so two identical Intel GPUs don't collapse
         * onto adapter 0; fall back to vendor/device. */
        if (intel_find_by_pci_topology(pci_props.pciDomain, pci_props.pciBus,
                                       pci_props.pciDevice, pci_props.pciFunction,
                                       &intel_idx) != 0 &&
            intel_find_by_pci(props.vendorID, props.deviceID, &intel_idx) != 0) {
            intel_idx = -1;
        }
        if (intel_idx >= 0) {
            strncpy(info->sensor_backend, "IGCL", sizeof(info->sensor_backend) - 1);

            int t;
            if (intel_get_temperature(intel_idx, &t) == 0)
                info->temperature_milli_c = t;

            int gpu_pct, mem_pct;
            if (intel_get_utilization(intel_idx, &gpu_pct, &mem_pct) == 0) {
                info->utilization_gpu_pct = gpu_pct;
                if (mem_pct >= 0)
                    info->utilization_mem_pct = mem_pct;
            }

            int core_mhz, mem_clk;
            if (intel_get_clocks(intel_idx, &core_mhz, &mem_clk) == 0) {
                info->core_clock_mhz = core_mhz;
                if (mem_clk >= 0)
                    info->mem_clock_mhz = mem_clk;
            }

            int pwr_usage, pwr_limit;
            if (intel_get_power(intel_idx, &pwr_usage, &pwr_limit) == 0) {
                if (pwr_usage >= 0)
                    info->power_milliwatts = pwr_usage;
                if (pwr_limit >= 0)
                    info->power_limit_milliwatts = pwr_limit;
            }

            int rpm, pct;
            if (intel_get_fan(intel_idx, &rpm, &pct) == 0) {
                if (rpm >= 0)
                    info->fan_speed_rpm = rpm;
                if (pct >= 0)
                    info->fan_speed_pct = pct;
            }

            uint64_t mem_used, mem_total;
            if (intel_get_memory(intel_idx, &mem_used, &mem_total) == 0) {
                info->mem_used_bytes  = mem_used;
                info->mem_total_bytes = mem_total;
            }

            /* memory: for iGPUs IGCL may only report a small segment;
             * use the Vulkan-visible total like we do for AMD APUs. */
            if (props.deviceType == VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU) {
                uint64_t vt = info->dedicated_vram_bytes + info->shared_ram_bytes;
                if (vt > info->mem_total_bytes)
                    info->mem_total_bytes = vt;
            }
        }
#elif defined(__linux__)
        int s_idx;
        /* Prefer PCI topology so two identical Intel GPUs don't collapse
         * onto card0; fall back to vendor/device. */
        if (sysfs_find_by_pci_topology(pci_props.pciDomain, pci_props.pciBus,
                                       pci_props.pciDevice, pci_props.pciFunction,
                                       &s_idx) != 0 &&
            sysfs_find_by_vendor_device((int)props.vendorID,
                                        (int)props.deviceID, &s_idx) != 0) {
            s_idx = -1;
        }
        if (s_idx >= 0) {
            strncpy(info->sensor_backend, "sysfs", sizeof(info->sensor_backend) - 1);

            int temp;
            if (sysfs_get_temperature(s_idx, &temp) == 0)
                info->temperature_milli_c = temp;

            int util;
            if (sysfs_get_intel_utilization(s_idx, &util) == 0)
                info->utilization_gpu_pct = util;

            int core_mhz, mm_mhz;
            if (sysfs_get_intel_clocks(s_idx, &core_mhz, &mm_mhz) == 0) {
                info->core_clock_mhz = core_mhz;
                if (mm_mhz >= 0)
                    info->mem_clock_mhz = mm_mhz;
            }

            int power;
            if (sysfs_get_intel_power(s_idx, &power) == 0)
                info->power_milliwatts = power;

            int rpm, pct;
            if (sysfs_get_fan(s_idx, &rpm, &pct) == 0) {
                info->fan_speed_rpm = rpm;
                if (pct >= 0)
                    info->fan_speed_pct = pct;
            }

            uint64_t mem_used_b, mem_total_b;
            if (sysfs_get_intel_memory(s_idx, &mem_used_b, &mem_total_b) == 0) {
                info->mem_used_bytes  = mem_used_b;
                info->mem_total_bytes = mem_total_b;
            }

            /* memory: for iGPUs, use Vulkan-visible total (UMA). */
            if (props.deviceType == VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU) {
                uint64_t vt = info->dedicated_vram_bytes + info->shared_ram_bytes;
                if (vt > info->mem_total_bytes)
                    info->mem_total_bytes = vt;
            }
        }
#endif
    }
}

/* ================================================================
 *  JSON output
 * ================================================================ */

static void json_esc(const char *s)
{
    putchar('"');
    for (; *s; s++) {
        unsigned char c = (unsigned char)*s;
        switch (c) {
        case '"':  fputs("\\\"", stdout); break;
        case '\\': fputs("\\\\", stdout); break;
        case '\b': fputs("\\b",  stdout); break;
        case '\f': fputs("\\f",  stdout); break;
        case '\n': fputs("\\n",  stdout); break;
        case '\r': fputs("\\r",  stdout); break;
        case '\t': fputs("\\t",  stdout); break;
        default:
            if (c < 0x20) fprintf(stdout, "\\u%04x", c);
            else          putchar(c);
        }
    }
    putchar('"');
}

static void json_indent(int n)
{
    while (n--) fputs("  ", stdout);
}

static void json_key(int indent, const char *key)
{
    json_indent(indent);
    json_esc(key);
    fputs(": ", stdout);
}

static void json_int_val(int indent, const char *key, int val, int sentinel)
{
    json_key(indent, key);
    if (val == sentinel) fputs("null", stdout);
    else                 fprintf(stdout, "%d", val);
}

static void json_float1_val(int indent, const char *key, int millival, int sentinel)
{
    json_key(indent, key);
    if (millival == sentinel) fputs("null", stdout);
    else                      fprintf(stdout, "%.1f", (double)millival / 1000.0);
}

static void json_str_val(int indent, const char *key, const char *val)
{
    json_key(indent, key);
    json_esc(val);
}

static void json_sensors_null_block(void)
{
    json_str_val(4, "backend", "");         fputs(",\n", stdout);
    json_key(4, "temperature_celsius");     fputs("null,\n", stdout);
    json_key(4, "utilization_gpu_pct");     fputs("null,\n", stdout);
    json_key(4, "utilization_memory_pct");  fputs("null,\n", stdout);
    json_key(4, "core_clock_mhz");          fputs("null,\n", stdout);
    json_key(4, "memory_clock_mhz");        fputs("null,\n", stdout);
    json_key(4, "power_watts");             fputs("null,\n", stdout);
    json_key(4, "power_limit_watts");       fputs("null,\n", stdout);
    json_key(4, "fan_speed_rpm");           fputs("null,\n", stdout);
    json_key(4, "fan_speed_pct");           fputs("null,\n", stdout);
    json_key(4, "perf_state");              fputs("null,\n", stdout);
    json_key(4, "ecc");                     fputs("null,\n", stdout);
    json_key(4, "memory_used_bytes");       fputs("null,\n", stdout);
    json_key(4, "memory_total_bytes");      fputs("null,\n", stdout);
    json_key(4, "driver_version_str");      fputs("null\n", stdout);
}

static void gpu_json_write_devices(const GpuInfo *gpus, int count)
{
    int i, j;
    json_indent(1); fputs("\"devices\": [\n", stdout);

    for (i = 0; i < count; i++) {
        const GpuInfo *g = &gpus[i];
        char buf[64];

        json_indent(2); fputs("{\n", stdout);

        /* identification */
        json_str_val(3, "name", g->device_name);        fputs(",\n", stdout);
        json_str_val(3, "vendor", vendor_name(g->vendor_id)); fputs(",\n", stdout);
        json_key(3, "vendor_id"); fprintf(stdout, "%u,\n", (unsigned)g->vendor_id);
        json_key(3, "device_id"); fprintf(stdout, "%u,\n", (unsigned)g->device_id);
        json_str_val(3, "type", device_type_str(g->device_type)); fputs(",\n", stdout);

        /* memory */
        json_indent(3); fputs("\"memory\": {\n", stdout);
        json_key(4, "dedicated_vram_bytes");
        fprintf(stdout, "%llu,\n", (unsigned long long)g->dedicated_vram_bytes);
        json_key(4, "shared_ram_bytes");
        fprintf(stdout, "%llu,\n", (unsigned long long)g->shared_ram_bytes);
        json_indent(4); fputs("\"heaps\": [\n", stdout);
        for (j = 0; j < (int)g->heap_count; j++) {
            json_indent(5); fputs("{\n", stdout);
            json_key(6, "size_bytes");
            fprintf(stdout, "%llu,\n", (unsigned long long)g->heap_sizes[j]);
            json_str_val(6, "flags",
                (g->heap_flags[j] & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT)
                ? "device_local" : "host");
            fputc('\n', stdout);
            json_indent(5); fputc('}', stdout);
            if (j < (int)g->heap_count - 1) fputc(',', stdout);
            fputc('\n', stdout);
        }
        json_indent(4); fputs("]\n", stdout);
        json_indent(3); fputs("},\n", stdout);

        /* vulkan */
        json_indent(3); fputs("\"vulkan\": {\n", stdout);
        format_api_ver(g->api_version, buf, sizeof(buf));
        json_str_val(4, "api_version", buf); fputs(",\n", stdout);
        format_driver_ver(g->driver_version, g->vendor_id, buf, sizeof(buf));
        json_str_val(4, "driver_version", buf); fputs(",\n", stdout);
        json_key(4, "driver_version_raw");
        fprintf(stdout, "\"0x%08X\"\n", g->driver_version);
        json_indent(3); fputs("},\n", stdout);

        /* sensors */
        json_indent(3); fputs("\"sensors\": {\n", stdout);

        if (g->sensor_backend[0]) {
            json_str_val(4, "backend", g->sensor_backend); fputs(",\n", stdout);

            json_float1_val(4, "temperature_celsius", g->temperature_milli_c, GPU_INFO_SENTINEL);
            fputs(",\n", stdout);

            json_int_val(4, "utilization_gpu_pct", g->utilization_gpu_pct, GPU_INFO_SENTINEL);
            fputs(",\n", stdout);

            json_int_val(4, "utilization_memory_pct", g->utilization_mem_pct, GPU_INFO_SENTINEL);
            fputs(",\n", stdout);

            json_int_val(4, "core_clock_mhz", g->core_clock_mhz, GPU_INFO_SENTINEL);
            fputs(",\n", stdout);

            json_int_val(4, "memory_clock_mhz", g->mem_clock_mhz, GPU_INFO_SENTINEL);
            fputs(",\n", stdout);

            json_float1_val(4, "power_watts", g->power_milliwatts, GPU_INFO_SENTINEL);
            fputs(",\n", stdout);

            json_float1_val(4, "power_limit_watts", g->power_limit_milliwatts, GPU_INFO_SENTINEL);
            fputs(",\n", stdout);

            json_int_val(4, "fan_speed_rpm", g->fan_speed_rpm, GPU_INFO_SENTINEL);
            fputs(",\n", stdout);

            json_int_val(4, "fan_speed_pct", g->fan_speed_pct, GPU_INFO_SENTINEL);
            fputs(",\n", stdout);

            /* perf_state */
            json_key(4, "perf_state");
            if (g->perf_state == GPU_INFO_SENTINEL)
                fputs("null", stdout);
            else
                fprintf(stdout, "\"P%d\"", g->perf_state);
            fputs(",\n", stdout);

            /* ecc */
            json_key(4, "ecc");
            if (g->ecc_enabled == GPU_INFO_SENTINEL)
                fputs("null", stdout);
            else
                fputs(g->ecc_enabled ? "\"enabled\"" : "\"disabled\"", stdout);
            fputs(",\n", stdout);

            /* memory usage — keyed by mem_total_bytes > 0 */
            json_key(4, "memory_used_bytes");
            if (g->mem_total_bytes > 0)
                fprintf(stdout, "%llu,\n", (unsigned long long)g->mem_used_bytes);
            else
                fputs("null,\n", stdout);

            json_key(4, "memory_total_bytes");
            if (g->mem_total_bytes > 0)
                fprintf(stdout, "%llu,\n", (unsigned long long)g->mem_total_bytes);
            else
                fputs("null,\n", stdout);

            /* driver version string */
            if (g->driver_version_str[0]) {
                json_str_val(4, "driver_version_str", g->driver_version_str);
                fputc('\n', stdout);
            } else {
                json_key(4, "driver_version_str"); fputs("null\n", stdout);
            }
        } else {
            json_sensors_null_block();
        }

        json_indent(3); fputs("}\n", stdout);
        json_indent(2); fputc('}', stdout);
        if (i < count - 1) fputc(',', stdout);
        fputc('\n', stdout);
    }

    json_indent(1); fputs("],\n", stdout);
    json_indent(1); fprintf(stdout, "\"device_count\": %d", count);
}

static void gpu_print_json(const GpuInfo *gpus, int count)
{
    fputs("{\n", stdout);
    gpu_json_write_devices(gpus, count);
    fputs("\n}\n", stdout);
}

static void gpu_json_write_memory_devices(const GpuInfo *gpus, int count)
{
    int i, j;
    json_indent(1); fputs("\"devices\": [\n", stdout);

    for (i = 0; i < count; i++) {
        const GpuInfo *g = &gpus[i];

        json_indent(2); fputs("{\n", stdout);

        /* identification */
        json_str_val(3, "name", g->device_name);        fputs(",\n", stdout);
        json_str_val(3, "vendor", vendor_name(g->vendor_id)); fputs(",\n", stdout);
        json_key(3, "vendor_id"); fprintf(stdout, "%u,\n", (unsigned)g->vendor_id);
        json_key(3, "device_id"); fprintf(stdout, "%u,\n", (unsigned)g->device_id);
        json_str_val(3, "type", device_type_str(g->device_type)); fputs(",\n", stdout);

        /* memory */
        json_indent(3); fputs("\"memory\": {\n", stdout);
        json_key(4, "dedicated_vram_bytes");
        fprintf(stdout, "%llu,\n", (unsigned long long)g->dedicated_vram_bytes);
        json_key(4, "shared_ram_bytes");
        fprintf(stdout, "%llu,\n", (unsigned long long)g->shared_ram_bytes);
        json_indent(4); fputs("\"heaps\": [\n", stdout);
        for (j = 0; j < (int)g->heap_count; j++) {
            json_indent(5); fputs("{\n", stdout);
            json_key(6, "size_bytes");
            fprintf(stdout, "%llu,\n", (unsigned long long)g->heap_sizes[j]);
            json_str_val(6, "flags",
                (g->heap_flags[j] & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT)
                ? "device_local" : "host");
            fputc('\n', stdout);
            json_indent(5); fputc('}', stdout);
            if (j < (int)g->heap_count - 1) fputc(',', stdout);
            fputc('\n', stdout);
        }
        json_indent(4); fputs("]\n", stdout);
        json_indent(3); fputs("},\n", stdout);

        /* sensors — only memory fields */
        json_indent(3); fputs("\"sensors\": {\n", stdout);
        if (g->sensor_backend[0]) {
            json_str_val(4, "backend", g->sensor_backend); fputs(",\n", stdout);
            json_key(4, "memory_used_bytes");
            if (g->mem_total_bytes > 0)
                fprintf(stdout, "%llu,\n", (unsigned long long)g->mem_used_bytes);
            else
                fputs("null,\n", stdout);
            json_key(4, "memory_total_bytes");
            if (g->mem_total_bytes > 0)
                fprintf(stdout, "%llu\n", (unsigned long long)g->mem_total_bytes);
            else
                fputs("null\n", stdout);
        } else {
            json_str_val(4, "backend", ""); fputs(",\n", stdout);
            json_key(4, "memory_used_bytes"); fputs("null,\n", stdout);
            json_key(4, "memory_total_bytes"); fputs("null\n", stdout);
        }
        json_indent(3); fputs("}\n", stdout);

        json_indent(2); fputc('}', stdout);
        if (i < count - 1) fputc(',', stdout);
        fputc('\n', stdout);
    }

    json_indent(1); fputs("],\n", stdout);
    json_indent(1); fprintf(stdout, "\"device_count\": %d", count);
}

/* ================================================================
 *  System info text output
 * ================================================================ */

static void sys_print_text(const SysInfo *si)
{
    int i;
    char buf[128], used_buf[64], total_buf[64];

    printf("===== System Information =====\n\n");

    /* OS */
    printf("OS\n");
    if (si->os_name[0])
        printf("  Name      : %s\n", si->os_name);
    if (si->os_version[0])
        printf("  Version   : %s\n", si->os_version);
    if (si->os_arch[0])
        printf("  Arch      : %s\n", si->os_arch);
    printf("  Hostname  : %s\n", si->host_name);
    if (si->kernel[0])
        printf("  Kernel    : %s\n", si->kernel);
    if (si->uptime_seconds > 0) {
        format_uptime(si->uptime_seconds, buf, sizeof(buf));
        printf("  Uptime    : %s\n", buf);
    }
    putchar('\n');

    /* CPU */
    printf("CPU\n");
    if (si->cpu_name[0])
        printf("  Model     : %s\n", si->cpu_name);
    if (si->cpu_cores > 0 && si->cpu_threads > 0)
        printf("  Cores     : %d physical / %d logical\n",
               si->cpu_cores, si->cpu_threads);
    if (si->cpu_freq_max_mhz > 0)
        printf("  Max Freq  : %d MHz\n", si->cpu_freq_max_mhz);
    putchar('\n');

    /* Memory */
    printf("Memory\n");
    if (si->memory_total_bytes > 0) {
        format_bytes(si->memory_total_bytes, total_buf, sizeof(total_buf));
        format_bytes(si->memory_used_bytes,  used_buf,  sizeof(used_buf));
        {
            int pct = (int)(si->memory_used_bytes * 100ULL / si->memory_total_bytes);
            printf("  Physical  : %s / %s (%d%%)\n",
                   used_buf, total_buf, pct);
        }
        if (si->swap_total_bytes > 0) {
            format_bytes(si->swap_total_bytes, total_buf, sizeof(total_buf));
            format_bytes(si->swap_used_bytes,  used_buf,  sizeof(used_buf));
            {
                int pct = (int)(si->swap_used_bytes * 100ULL / si->swap_total_bytes);
                printf("  Swap      : %s / %s (%d%%)\n",
                       used_buf, total_buf, pct);
            }
        }
    }
    putchar('\n');

    /* Disk */
    if (si->disk_count > 0) {
        printf("Disk Drives\n");
        for (i = 0; i < si->disk_count; i++) {
            format_bytes(si->disks[i].total_bytes, total_buf, sizeof(total_buf));
            format_bytes(si->disks[i].used_bytes,  used_buf,  sizeof(used_buf));
            {
                int pct = si->disks[i].total_bytes > 0
                    ? (int)(si->disks[i].used_bytes * 100ULL / si->disks[i].total_bytes)
                    : 0;
                printf("  %-10s %s / %s (%d%%)",
                       si->disks[i].mount, used_buf, total_buf, pct);
                if (si->disks[i].filesystem[0])
                    printf("  %s", si->disks[i].filesystem);
                if (si->disks[i].is_external)
                    printf("  [External]");
                putchar('\n');
            }
        }
        putchar('\n');
    }
}

/* ================================================================
 *  System info JSON output
 * ================================================================ */

static void sys_json_write_system(const SysInfo *si)
{
    int i;
    char buf[128];

    json_indent(1); fputs("\"system\": {\n", stdout);

    /* OS */
    json_indent(2); fputs("\"os\": {\n", stdout);
    json_str_val(3, "name",    si->os_name);     fputs(",\n", stdout);
    json_str_val(3, "version", si->os_version);  fputs(",\n", stdout);
    json_str_val(3, "arch",    si->os_arch);     fputs(",\n", stdout);
    json_str_val(3, "hostname",si->host_name);   fputs(",\n", stdout);
    json_str_val(3, "kernel",  si->kernel);
    if (si->uptime_seconds > 0) {
        fputs(",\n", stdout);
        json_key(3, "uptime_seconds");
        fprintf(stdout, "%llu", (unsigned long long)si->uptime_seconds);
    }
    fputc('\n', stdout);
    json_indent(2); fputs("},\n", stdout);

    /* CPU */
    json_indent(2); fputs("\"cpu\": {\n", stdout);
    json_str_val(3, "name", si->cpu_name); fputs(",\n", stdout);
    json_key(3, "cores");   fprintf(stdout, "%d,\n", si->cpu_cores);
    json_key(3, "threads"); fprintf(stdout, "%d,\n", si->cpu_threads);
    json_key(3, "freq_max_mhz"); fprintf(stdout, "%d\n", si->cpu_freq_max_mhz);
    json_indent(2); fputs("},\n", stdout);

    /* Memory */
    json_indent(2); fputs("\"memory\": {\n", stdout);
    json_key(3, "total_bytes");
    fprintf(stdout, "%llu,\n", (unsigned long long)si->memory_total_bytes);
    json_key(3, "used_bytes");
    fprintf(stdout, "%llu,\n", (unsigned long long)si->memory_used_bytes);
    json_key(3, "swap_total_bytes");
    fprintf(stdout, "%llu,\n", (unsigned long long)si->swap_total_bytes);
    json_key(3, "swap_used_bytes");
    fprintf(stdout, "%llu\n", (unsigned long long)si->swap_used_bytes);
    json_indent(2); fputs("},\n", stdout);

    /* Disks */
    json_indent(2); fputs("\"disks\": [\n", stdout);
    for (i = 0; i < si->disk_count; i++) {
        json_indent(3); fputs("{\n", stdout);
        json_str_val(4, "name", si->disks[i].name); fputs(",\n", stdout);
        json_str_val(4, "mount", si->disks[i].mount); fputs(",\n", stdout);
        json_str_val(4, "filesystem", si->disks[i].filesystem); fputs(",\n", stdout);
        json_key(4, "total_bytes");
        fprintf(stdout, "%llu,\n", (unsigned long long)si->disks[i].total_bytes);
        json_key(4, "used_bytes");
        fprintf(stdout, "%llu,\n", (unsigned long long)si->disks[i].used_bytes);
        json_key(4, "is_external");
        fputs(si->disks[i].is_external ? "true\n" : "false\n", stdout);
        json_indent(3); fputc('}', stdout);
        if (i < si->disk_count - 1) fputc(',', stdout);
        fputc('\n', stdout);
    }
    json_indent(2); fputs("]\n", stdout);

    json_indent(1); fputc('}', stdout);  /* no trailing newline */
}

static void sys_print_json(const SysInfo *si)
{
    fputs("{\n", stdout);
    sys_json_write_system(si);
    fputs("\n}\n", stdout);
}

static void sys_json_write_memory_only(const SysInfo *si)
{
    json_indent(1); fputs("\"system\": {\n", stdout);
    json_indent(2); fputs("\"memory\": {\n", stdout);
    json_key(3, "total_bytes");
    fprintf(stdout, "%llu,\n", (unsigned long long)si->memory_total_bytes);
    json_key(3, "used_bytes");
    fprintf(stdout, "%llu,\n", (unsigned long long)si->memory_used_bytes);
    json_key(3, "swap_total_bytes");
    fprintf(stdout, "%llu,\n", (unsigned long long)si->swap_total_bytes);
    json_key(3, "swap_used_bytes");
    fprintf(stdout, "%llu\n", (unsigned long long)si->swap_used_bytes);
    json_indent(2); fputs("}\n", stdout);
    json_indent(1); fputc('}', stdout);
}

static void print_all_json(const SysInfo *si, const GpuInfo *gpus, int count)
{
    fputs("{\n", stdout);
    sys_json_write_system(si);
    fputs(",\n", stdout);
    gpu_json_write_devices(gpus, count);
    fputs("\n}\n", stdout);
}

static void print_gpu_json(const GpuInfo *gpus, int count)
{
    fputs("{\n", stdout);
    gpu_json_write_devices(gpus, count);
    fputs("\n}\n", stdout);
}

static void print_memory_json(const SysInfo *si, const GpuInfo *gpus, int count)
{
    fputs("{\n", stdout);
    sys_json_write_memory_only(si);
    if (count > 0) {
        fputs(",\n", stdout);
        gpu_json_write_memory_devices(gpus, count);
    }
    fputs("\n}\n", stdout);
}

static void sys_json_write_cpu_only(const SysInfo *si)
{
    json_indent(1); fputs("\"system\": {\n", stdout);
    json_indent(2); fputs("\"cpu\": {\n", stdout);
    json_str_val(3, "name", si->cpu_name); fputs(",\n", stdout);
    json_key(3, "cores");   fprintf(stdout, "%d,\n", si->cpu_cores);
    json_key(3, "threads"); fprintf(stdout, "%d,\n", si->cpu_threads);
    json_key(3, "freq_max_mhz"); fprintf(stdout, "%d\n", si->cpu_freq_max_mhz);
    json_indent(2); fputs("}\n", stdout);
    json_indent(1); fputc('}', stdout);
}

static void sys_json_write_ram_only(const SysInfo *si)
{
    json_indent(1); fputs("\"system\": {\n", stdout);
    json_indent(2); fputs("\"memory\": {\n", stdout);
    json_key(3, "total_bytes");
    fprintf(stdout, "%llu,\n", (unsigned long long)si->memory_total_bytes);
    json_key(3, "used_bytes");
    fprintf(stdout, "%llu,\n", (unsigned long long)si->memory_used_bytes);
    json_key(3, "swap_total_bytes");
    fprintf(stdout, "%llu,\n", (unsigned long long)si->swap_total_bytes);
    json_key(3, "swap_used_bytes");
    fprintf(stdout, "%llu\n", (unsigned long long)si->swap_used_bytes);
    json_indent(2); fputs("}\n", stdout);
    json_indent(1); fputc('}', stdout);
}

static void print_cpu_ram_json(const SysInfo *si, int cpu, int ram)
{
    int need_comma = 0;
    fputs("{\n", stdout);
    json_indent(1); fputs("\"system\": {\n", stdout);
    if (cpu) {
        json_indent(2); fputs("\"cpu\": {\n", stdout);
        json_str_val(3, "name", si->cpu_name); fputs(",\n", stdout);
        json_key(3, "cores");   fprintf(stdout, "%d,\n", si->cpu_cores);
        json_key(3, "threads"); fprintf(stdout, "%d,\n", si->cpu_threads);
        json_key(3, "freq_max_mhz"); fprintf(stdout, "%d\n", si->cpu_freq_max_mhz);
        json_indent(2); fputs("}", stdout);
        need_comma = 1;
    }
    if (ram) {
        if (need_comma) fputs(",\n", stdout);
        json_indent(2); fputs("\"memory\": {\n", stdout);
        json_key(3, "total_bytes");
        fprintf(stdout, "%llu,\n", (unsigned long long)si->memory_total_bytes);
        json_key(3, "used_bytes");
        fprintf(stdout, "%llu,\n", (unsigned long long)si->memory_used_bytes);
        json_key(3, "swap_total_bytes");
        fprintf(stdout, "%llu,\n", (unsigned long long)si->swap_total_bytes);
        json_key(3, "swap_used_bytes");
        fprintf(stdout, "%llu\n", (unsigned long long)si->swap_used_bytes);
        json_indent(2); fputs("}", stdout);
    }
    fputc('\n', stdout);
    json_indent(1); fputs("}\n", stdout);
    fputs("}\n", stdout);
}

/* ================================================================
 *  Signal handling
 * ================================================================ */

static volatile int g_exit_requested = 0;

#ifdef _WIN32
static BOOL WINAPI ctrl_handler(DWORD type)
{
    if (type == CTRL_C_EVENT || type == CTRL_BREAK_EVENT) {
        g_exit_requested = 1;
        return TRUE;
    }
    return FALSE;
}
#else
static void sigint_handler(int sig)
{
    (void)sig;
    g_exit_requested = 1;
}
#endif

/* ================================================================
 *  Help
 * ================================================================ */

static void print_help(void)
{
    printf("Available commands:\n");
    printf("  --json           Output in JSON format\n");
    printf("  --gpu            GPU information only\n");
    printf("  --memory         Memory information only\n");
    printf("  --cpu            CPU information only\n");
    printf("  --ram            RAM information only\n");
    printf("  --interactive    Force interactive mode (stdin non-TTY)\n");
    printf("  --exit           Exit the program\n");
    printf("  --quit           Exit the program\n");
    printf("  --help           Show this help\n");
    printf("\nExamples:\n");
    printf("  gpu-info> --json\n");
    printf("  gpu-info> --json --cpu\n");
    printf("  gpu-info> --json --gpu\n");
    printf("  gpu-info> --gpu\n");
}

/* ================================================================
 *  do_output — collect + print, callable repeatedly
 * ================================================================ */

static int do_output(int argc, char *argv[],
                     VkInstance instance,
                     VkPhysicalDevice *phys_devices,
                     uint32_t device_count)
{
    int json_mode = 0;
    int gpu_only = 0;
    int memory_only = 0;
    int cpu_mode = 0;
    int ram_mode = 0;
    int argi;
    for (argi = 0; argi < argc; argi++) {
        if (strcmp(argv[argi], "--json") == 0) {
            json_mode = 1;
        } else if (strcmp(argv[argi], "--gpu") == 0) {
            gpu_only = 1;
        } else if (strcmp(argv[argi], "--memory") == 0) {
            memory_only = 1;
        } else if (strcmp(argv[argi], "--cpu") == 0) {
            cpu_mode = 1;
        } else if (strcmp(argv[argi], "--ram") == 0) {
            ram_mode = 1;
        }
    }

    uint32_t i;

    /* -- collect system info -- */
    SysInfo si;
    memset(&si, 0, sizeof(si));
    if (cpu_mode || ram_mode) {
        if (cpu_mode) sys_collect_cpu_info(&si);
        if (ram_mode) sys_collect_ram_info(&si);
    } else if (!gpu_only) {
        sys_collect_info(&si);
    }

    /* -- collect GPU data for JSON output -- */
    GpuInfo *gpus = NULL;
    if (!cpu_mode && !ram_mode && json_mode && instance != VK_NULL_HANDLE) {
        gpus = (GpuInfo*)calloc((size_t)device_count, sizeof(GpuInfo));
        if (!gpus) {
            fprintf(stderr, "calloc failed\n");
            return 1;
        }
        for (i = 0; i < device_count; i++) {
            gpu_collect_info(&gpus[i], phys_devices[i]);
        }
    }

    /* -- GPU text output -- */
    if (!cpu_mode && !ram_mode && !json_mode && instance != VK_NULL_HANDLE) {
        uint32_t total = 0;

        printf("===== GPU Information (Vulkan + ADL/IGCL/sysfs + NVML) =====\n");
        printf("(Plain C -- cross-platform + dynamic info)\n");
        printf("=======================================================\n\n");

        for (i = 0; i < device_count; i++) {
            VkPhysicalDeviceProperties props;
            VkPhysicalDeviceMemoryProperties mem;

            vkGetPhysicalDeviceProperties(phys_devices[i], &props);
            vkGetPhysicalDeviceMemoryProperties(phys_devices[i], &mem);

            uint64_t dedicated_vram = 0;
            uint64_t shared_ram = 0;
            uint32_t h;
            for (h = 0; h < mem.memoryHeapCount; h++) {
                if (mem.memoryHeaps[h].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT) {
                    dedicated_vram += mem.memoryHeaps[h].size;
                } else {
                    shared_ram += mem.memoryHeaps[h].size;
                }
            }

#ifdef __linux__
            if (props.vendorID == 0x1002 &&
                props.deviceType == VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU) {
                int s_idx;
                if (sysfs_find_by_vendor_device((int)props.vendorID,
                                                (int)props.deviceID,
                                                &s_idx) == 0) {
                    uint64_t vr = 0, gt = 0;
                    if (sysfs_get_vram_total(s_idx, &vr) == 0 &&
                        sysfs_get_gtt_total(s_idx, &gt) == 0) {
                        uint64_t st = vr + gt;
                        if (st > dedicated_vram) {
                            uint64_t vt = dedicated_vram + shared_ram;
                            if (st > vt) st = vt;
                            dedicated_vram = st;
                            shared_ram = vt - st;
                        }
                    }
                }
            }
#endif

            char vram_buf[64], sram_buf[64];
            char api_buf[32], driver_buf[64];
            format_bytes(dedicated_vram, vram_buf, sizeof(vram_buf));
            format_bytes(shared_ram,     sram_buf, sizeof(sram_buf));
            format_api_ver(props.apiVersion, api_buf, sizeof(api_buf));
            format_driver_ver(props.driverVersion, props.vendorID,
                              driver_buf, sizeof(driver_buf));

            printf("Device %u: %s\n", i, props.deviceName);
            printf("  Vendor          : %s (0x%04X)\n",
                   vendor_name(props.vendorID), props.vendorID);
            printf("  Device ID       : 0x%04X\n", props.deviceID);
            printf("  Type            : %s\n",
                   device_type_str(props.deviceType));
            printf("  Dedicated VRAM  : %s\n", vram_buf);
            printf("  Shared RAM      : %s\n", sram_buf);
            printf("  Vulkan API      : %s\n", api_buf);
            printf("  Driver Version  : %s (raw: 0x%08X)\n",
                   driver_buf, props.driverVersion);

            printf("  Memory Heaps    : %u\n", mem.memoryHeapCount);
            for (h = 0; h < mem.memoryHeapCount; h++) {
                char heap_buf[64];
                format_bytes(mem.memoryHeaps[h].size, heap_buf, sizeof(heap_buf));
                const char* heap_type =
                    (mem.memoryHeaps[h].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT)
                    ? "Device Local (VRAM)"
                    : "Host     (System RAM)";
                printf("    Heap %u: %-10s  [%s]\n", h, heap_buf, heap_type);
            }

#ifdef _WIN32
            if (props.vendorID == 0x1002) {
                int adl_idx = -1;
                VkPhysicalDeviceProperties2 amd_props2 = {0};
                VkPhysicalDevicePCIBusInfoPropertiesEXT amd_pci = {0};
                VkPhysicalDeviceIDProperties amd_id = {0};
                amd_props2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2;
                amd_props2.pNext = &amd_pci;
                amd_pci.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PCI_BUS_INFO_PROPERTIES_EXT;
                amd_pci.pNext = &amd_id;
                amd_id.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES;
                vkGetPhysicalDeviceProperties2(phys_devices[i], &amd_props2);

                uint32_t amd_luid_low = 0, amd_luid_high = 0;
                if (amd_id.deviceLUIDValid) {
                    amd_luid_low  = ((uint32_t)amd_id.deviceLUID[0])       |
                                    ((uint32_t)amd_id.deviceLUID[1] << 8)  |
                                    ((uint32_t)amd_id.deviceLUID[2] << 16) |
                                    ((uint32_t)amd_id.deviceLUID[3] << 24);
                    amd_luid_high = ((uint32_t)amd_id.deviceLUID[4])       |
                                    ((uint32_t)amd_id.deviceLUID[5] << 8)  |
                                    ((uint32_t)amd_id.deviceLUID[6] << 16) |
                                    ((uint32_t)amd_id.deviceLUID[7] << 24);
                }

                printf("\n  --- AMD Dynamic Info (ADL) ---\n");
                /* Prefer PCI topology so two identical AMD GPUs don't
                 * collapse onto adapter 0; fall back to name. */
                if (adl_find_by_pci_topology(amd_pci.pciDomain, amd_pci.pciBus,
                                             amd_pci.pciDevice, amd_pci.pciFunction,
                                             &adl_idx) != 0 &&
                    adl_find_by_name(props.deviceName, &adl_idx) != 0) {
                    adl_idx = -1;
                }
                if (adl_idx >= 0) {
                    int t, rpm, pct, eng, mem_clk, act, lvl;

                    if (adl_get_temperature(adl_idx, &t) == 0)
                        printf("  GPU Temperature : %.1f C\n", t / 1000.0);
                    else
                        printf("  GPU Temperature : N/A\n");

                    if (adl_get_fan_speed(adl_idx, &rpm, &pct) == 0)
                        printf("  Fan Speed       : %d%% (%d RPM)\n", pct, rpm);
                    else if (adl_get_pmlog_fan(adl_idx, &rpm, &pct) == 0)
                        printf("  Fan Speed       : %d%% (%d RPM)\n", pct, rpm);
                    else
                        printf("  Fan Speed       : N/A\n");

                    if (adl_get_activity(adl_idx, &eng, &mem_clk, &act, &lvl) == 0) {
                        printf("  GPU Utilization : %d%%\n", act);
                        printf("  Engine Clock    : %d MHz\n", eng / 100);
                        printf("  Memory Clock    : %d MHz\n", mem_clk / 100);
                        if (lvl >= 0)
                            printf("  Perf Level      : P%d\n", lvl);
                        else
                            printf("  Perf Level      : N/A\n");
                    } else {
                        printf("  GPU Utilization : N/A\n");
                        printf("  Engine Clock    : N/A\n");
                        printf("  Memory Clock    : N/A\n");
                        printf("  Perf Level      : N/A\n");
                        if (amd_id.deviceLUIDValid) {
                            int u;
                            if (winmem_get_utilization(amd_luid_low, amd_luid_high, &u) == 0)
                                printf("  GPU Utilization : %d%% (PDH)\n", u);
                        }
                    }

                    /* PMLog power -- works on iGPUs/APUs */
                    {
                        int pwr;
                        if (adl_get_pmlog_power(adl_idx, &pwr) == 0 && pwr > 0)
                            printf("  GPU Power       : %.1f W\n", pwr / 1000.0);
                        else
                            printf("  GPU Power       : N/A\n");
                    }

                    {
                        uint64_t vram_used, vram_total;
                        int mem_ok = 0;
                        if (adl_get_memory(adl_idx, &vram_used, &vram_total) == 0) {
                            if (props.deviceType == VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU) {
                                uint64_t vt = dedicated_vram + shared_ram;
                                if (vt > vram_total)
                                    vram_total = vt;
                                if (amd_id.deviceLUIDValid) {
                                    uint64_t ded = 0, sha = 0, tot = 0;
                                    if (winmem_get_memory(amd_luid_low, amd_luid_high,
                                                          &ded, &sha, &tot) == 0) {
                                        vram_used  = sha + ded;
                                        vram_total = tot;
                                    }
                                }
                            }
                            mem_ok = 1;
                        } else if (amd_id.deviceLUIDValid &&
                                   props.deviceType == VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU) {
                            uint64_t ded = 0, sha = 0, tot = 0;
                            if (winmem_get_memory(amd_luid_low, amd_luid_high,
                                                  &ded, &sha, &tot) == 0) {
                                vram_used  = sha + ded;
                                vram_total = tot;
                                mem_ok = 1;
                            }
                        }
                        if (mem_ok)
                            printf("  Memory Usage    : %llu / %llu MB\n",
                                   (unsigned long long)(vram_used / (1024ULL * 1024ULL)),
                                   (unsigned long long)(vram_total / (1024ULL * 1024ULL)));
                        else
                            printf("  Memory Usage    : N/A\n");
                    }
                } else {
                    printf("  (could not match ADL adapter)\n");
                }
            }
#endif

#ifdef __linux__
            if (props.vendorID == 0x1002) {
                int s_idx = -1;
                VkPhysicalDeviceProperties2 amd_props2 = {0};
                VkPhysicalDevicePCIBusInfoPropertiesEXT amd_pci = {0};
                amd_props2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2;
                amd_props2.pNext = &amd_pci;
                amd_pci.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PCI_BUS_INFO_PROPERTIES_EXT;
                vkGetPhysicalDeviceProperties2(phys_devices[i], &amd_props2);

                printf("\n  --- AMD Dynamic Info (sysfs) ---\n");
                /* Prefer PCI topology so two identical AMD GPUs don't
                 * collapse onto card0; fall back to vendor/device. */
                if (sysfs_find_by_pci_topology(amd_pci.pciDomain, amd_pci.pciBus,
                                               amd_pci.pciDevice, amd_pci.pciFunction,
                                               &s_idx) != 0 &&
                    sysfs_find_by_vendor_device(
                        (int)props.vendorID, (int)props.deviceID, &s_idx) != 0) {
                    s_idx = -1;
                }
                if (s_idx >= 0) {
                    int temp, util, core_mhz, mem_mhz, power;

                    if (sysfs_get_temperature(s_idx, &temp) == 0)
                        printf("  GPU Temperature : %.1f C\n", temp / 1000.0);
                    else
                        printf("  GPU Temperature : N/A\n");

                    if (sysfs_get_utilization(s_idx, &util) == 0)
                        printf("  GPU Utilization : %d%%\n", util);
                    else
                        printf("  GPU Utilization : N/A\n");

                    if (sysfs_get_clocks(s_idx, &core_mhz, &mem_mhz) == 0) {
                        printf("  Engine Clock    : %d MHz\n", core_mhz);
                        if (mem_mhz >= 0)
                            printf("  Memory Clock    : %d MHz\n", mem_mhz);
                        else
                            printf("  Memory Clock    : N/A\n");
                    } else {
                        printf("  Engine Clock    : N/A\n");
                        printf("  Memory Clock    : N/A\n");
                    }

                    if (sysfs_get_power(s_idx, &power) == 0)
                        printf("  Power           : %.2f W\n", power / 1000.0);
                    else
                        printf("  Power           : N/A\n");

                    {
                        uint64_t vram_used_b, vram_total_b;
                        if (sysfs_get_memory(s_idx, &vram_used_b, &vram_total_b) == 0) {
                            uint64_t display_used  = vram_used_b;
                            uint64_t display_total = vram_total_b;
                            if (props.deviceType == VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU) {
                                uint64_t vr = 0, gt = 0;
                                if (sysfs_get_vram_total(s_idx, &vr) == 0 &&
                                    sysfs_get_gtt_total(s_idx, &gt) == 0) {
                                    uint64_t st = vr + gt;
                                    if (st > vram_total_b) {
                                        uint64_t vt = dedicated_vram + shared_ram;
                                        if (st > vt) st = vt;
                                        display_total = st;
                                        uint64_t gu = 0;
                                        if (sysfs_get_gtt_used(s_idx, &gu) == 0)
                                            display_used += gu;
                                    }
                                }
                            }
                            printf("  Memory Usage    : %llu / %llu MB\n",
                                   (unsigned long long)(display_used / (1024ULL * 1024ULL)),
                                   (unsigned long long)(display_total / (1024ULL * 1024ULL)));
                        } else
                            printf("  Memory Usage    : N/A\n");
                    }
                } else {
                    printf("  (could not match /sys/class/drm/ device)\n");
                }
            }
#endif

            if (props.vendorID == 0x10DE) {
                int nv_idx;
                VkPhysicalDeviceProperties2 nv_props2 = {0};
                VkPhysicalDevicePCIBusInfoPropertiesEXT nv_pci = {0};
                nv_props2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2;
                nv_props2.pNext = &nv_pci;
                nv_pci.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PCI_BUS_INFO_PROPERTIES_EXT;
                vkGetPhysicalDeviceProperties2(phys_devices[i], &nv_props2);

                printf("\n  --- NVIDIA Dynamic Info (NVML) ---\n");
                if (nvml_find_by_pci(nv_pci.pciDomain, nv_pci.pciBus,
                                     nv_pci.pciDevice, nv_pci.pciFunction,
                                     &nv_idx) == 0) {
                    int temp, gpu_pct, mem_pct, sm_mhz, mem_clk;
                    int power_usage, power_limit, fan_pct, pstate, ecc;
                    unsigned int mem_used, mem_total;
                    char drv_ver[64];

                    if (nvml_get_temperature(nv_idx, &temp) == 0)
                        printf("  GPU Temperature : %d C\n", temp);
                    else
                        printf("  GPU Temperature : N/A\n");

                    if (nvml_get_utilization(nv_idx, &gpu_pct, &mem_pct) == 0) {
                        printf("  GPU Utilization : %d%%\n", gpu_pct);
                        printf("  Memory Util.    : %d%%\n", mem_pct);
                    } else {
                        printf("  GPU Utilization : N/A\n");
                    }

                    if (nvml_get_clocks(nv_idx, &sm_mhz, &mem_clk) == 0) {
                        printf("  SM Clock        : %d MHz\n", sm_mhz);
                        if (mem_clk >= 0)
                            printf("  Memory Clock    : %d MHz\n", mem_clk);
                        else
                            printf("  Memory Clock    : N/A\n");
                    } else {
                        printf("  SM Clock        : N/A\n");
                        printf("  Memory Clock    : N/A\n");
                    }

                    if (nvml_get_memory(nv_idx, &mem_used, &mem_total) == 0)
                        printf("  Memory Usage    : %u / %u MB\n", mem_used, mem_total);
                    else
                        printf("  Memory Usage    : N/A\n");

                    if (nvml_get_power(nv_idx, &power_usage, &power_limit) == 0) {
                        if (power_usage >= 0 && power_limit >= 0)
                            printf("  Power           : %d W / %d W\n",
                                   power_usage, power_limit);
                        else if (power_usage >= 0)
                            printf("  Power           : %d W\n", power_usage);
                        else
                            printf("  Power           : N/A\n");
                    } else {
                        printf("  Power           : N/A\n");
                    }

                    if (nvml_get_fan(nv_idx, &fan_pct) == 0)
                        printf("  Fan Speed       : %d%%\n", fan_pct);
                    else
                        printf("  Fan Speed       : N/A\n");

                    if (nvml_get_perf_state(nv_idx, &pstate) == 0)
                        printf("  Perf State      : P%d\n", pstate);
                    else
                        printf("  Perf State      : N/A\n");

                    if (nvml_get_ecc(nv_idx, &ecc) == 0)
                        printf("  ECC             : %s\n", ecc ? "Enabled" : "Disabled");
                    else
                        printf("  ECC             : N/A\n");

                    if (nvml_get_driver_version(drv_ver, sizeof(drv_ver)) == 0)
                        printf("  Driver Version  : %s\n", drv_ver);
                    else
                        printf("  Driver Version  : N/A\n");
                } else {
                    printf("  (could not match NVML device)\n");
                }
            }

#ifdef _WIN32
            if (props.vendorID == 0x8086) {
                int intel_idx = -1;
                VkPhysicalDeviceProperties2 intel_props2 = {0};
                VkPhysicalDevicePCIBusInfoPropertiesEXT intel_pci = {0};
                intel_props2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2;
                intel_props2.pNext = &intel_pci;
                intel_pci.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PCI_BUS_INFO_PROPERTIES_EXT;
                vkGetPhysicalDeviceProperties2(phys_devices[i], &intel_props2);

                printf("\n  --- Intel Dynamic Info (IGCL) ---\n");
                if (intel_find_by_pci_topology(intel_pci.pciDomain, intel_pci.pciBus,
                                               intel_pci.pciDevice, intel_pci.pciFunction,
                                               &intel_idx) != 0 &&
                    intel_find_by_pci(props.vendorID, props.deviceID, &intel_idx) != 0) {
                    intel_idx = -1;
                }
                if (intel_idx >= 0) {
                    int t, gpu_pct, mem_pct, core_mhz, mem_clk;
                    int pwr_usage, pwr_limit, rpm, pct;
                    uint64_t mem_used, mem_total;

                    if (intel_get_temperature(intel_idx, &t) == 0)
                        printf("  GPU Temperature : %.1f C\n", t / 1000.0);
                    else
                        printf("  GPU Temperature : N/A\n");

                    if (intel_get_utilization(intel_idx, &gpu_pct, &mem_pct) == 0) {
                        printf("  GPU Utilization : %d%%\n", gpu_pct);
                        if (mem_pct >= 0)
                            printf("  Memory Util.    : %d%%\n", mem_pct);
                    } else {
                        printf("  GPU Utilization : N/A\n");
                    }

                    if (intel_get_clocks(intel_idx, &core_mhz, &mem_clk) == 0) {
                        printf("  GPU Clock       : %d MHz\n", core_mhz);
                        if (mem_clk >= 0)
                            printf("  Memory Clock    : %d MHz\n", mem_clk);
                        else
                            printf("  Memory Clock    : N/A\n");
                    } else {
                        printf("  GPU Clock       : N/A\n");
                        printf("  Memory Clock    : N/A\n");
                    }

                    if (intel_get_power(intel_idx, &pwr_usage, &pwr_limit) == 0) {
                        if (pwr_usage >= 0 && pwr_limit >= 0)
                            printf("  Power           : %.2f W / %.2f W\n",
                                   pwr_usage / 1000.0, pwr_limit / 1000.0);
                        else if (pwr_usage >= 0)
                            printf("  Power           : %.2f W\n", pwr_usage / 1000.0);
                        else
                            printf("  Power           : N/A\n");
                    } else {
                        printf("  Power           : N/A\n");
                    }

                    if (intel_get_fan(intel_idx, &rpm, &pct) == 0) {
                        if (rpm >= 0 && pct >= 0)
                            printf("  Fan Speed       : %d%% (%d RPM)\n", pct, rpm);
                        else if (pct >= 0)
                            printf("  Fan Speed       : %d%%\n", pct);
                        else if (rpm >= 0)
                            printf("  Fan Speed       : %d RPM\n", rpm);
                    } else {
                        printf("  Fan Speed       : N/A\n");
                    }

                    if (intel_get_memory(intel_idx, &mem_used, &mem_total) == 0) {
                        if (props.deviceType == VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU) {
                            uint64_t vt = dedicated_vram + shared_ram;
                            if (vt > mem_total)
                                mem_total = vt;
                        }
                        printf("  Memory Usage    : %llu / %llu MB\n",
                               (unsigned long long)(mem_used / (1024ULL * 1024ULL)),
                               (unsigned long long)(mem_total / (1024ULL * 1024ULL)));
                    } else {
                        printf("  Memory Usage    : N/A\n");
                    }
                } else {
                    printf("  (could not match IGCL adapter)\n");
                }
            }
#endif

#ifdef __linux__
            if (props.vendorID == 0x8086) {
                int s_idx = -1;
                VkPhysicalDeviceProperties2 intel_props2 = {0};
                VkPhysicalDevicePCIBusInfoPropertiesEXT intel_pci = {0};
                intel_props2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2;
                intel_props2.pNext = &intel_pci;
                intel_pci.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PCI_BUS_INFO_PROPERTIES_EXT;
                vkGetPhysicalDeviceProperties2(phys_devices[i], &intel_props2);

                printf("\n  --- Intel Dynamic Info (sysfs) ---\n");
                if (sysfs_find_by_pci_topology(intel_pci.pciDomain, intel_pci.pciBus,
                                               intel_pci.pciDevice, intel_pci.pciFunction,
                                               &s_idx) != 0 &&
                    sysfs_find_by_vendor_device(
                        (int)props.vendorID, (int)props.deviceID, &s_idx) != 0) {
                    s_idx = -1;
                }
                if (s_idx >= 0) {
                    int temp, util, core_mhz, mem_mhz, power;

                    if (sysfs_get_temperature(s_idx, &temp) == 0)
                        printf("  GPU Temperature : %.1f C\n", temp / 1000.0);
                    else
                        printf("  GPU Temperature : N/A\n");

                    if (sysfs_get_intel_utilization(s_idx, &util) == 0)
                        printf("  GPU Utilization : %d%%\n", util);
                    else
                        printf("  GPU Utilization : N/A\n");

                    if (sysfs_get_intel_clocks(s_idx, &core_mhz, &mem_mhz) == 0) {
                        printf("  GPU Clock       : %d MHz\n", core_mhz);
                        if (mem_mhz >= 0)
                            printf("  Memory Clock    : %d MHz\n", mem_mhz);
                        else
                            printf("  Memory Clock    : N/A\n");
                    } else {
                        printf("  GPU Clock       : N/A\n");
                        printf("  Memory Clock    : N/A\n");
                    }

                    if (sysfs_get_intel_power(s_idx, &power) == 0)
                        printf("  Power           : %.2f W\n", power / 1000.0);
                    else
                        printf("  Power           : N/A\n");

                    {
                        uint64_t mem_used_b, mem_total_b;
                        if (sysfs_get_intel_memory(s_idx, &mem_used_b, &mem_total_b) == 0) {
                            if (props.deviceType == VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU) {
                                uint64_t vt = dedicated_vram + shared_ram;
                                if (vt > mem_total_b)
                                    mem_total_b = vt;
                            }
                            printf("  Memory Usage    : %llu / %llu MB\n",
                                   (unsigned long long)(mem_used_b / (1024ULL * 1024ULL)),
                                   (unsigned long long)(mem_total_b / (1024ULL * 1024ULL)));
                        } else
                            printf("  Memory Usage    : N/A\n");
                    }
                } else {
                    printf("  (could not match /sys/class/drm/ device)\n");
                }
            }
#endif

            printf("\n");
            total++;
        }

        printf("---\n");
        printf("Total: %u GPU(s) detected.\n", total);
    }

    /* -- system info output -- */
    if (cpu_mode || ram_mode) {
        if (json_mode) {
            print_cpu_ram_json(&si, cpu_mode, ram_mode);
        } else {
            putchar('\n');
            sys_print_text(&si);
        }
    } else if (memory_only) {
        if (json_mode) {
            if (instance != VK_NULL_HANDLE)
                print_memory_json(&si, gpus, (int)device_count);
            else
                print_memory_json(&si, NULL, 0);
        } else {
            putchar('\n');
            sys_print_text(&si);
        }
    } else if (!gpu_only) {
        if (json_mode) {
            if (instance != VK_NULL_HANDLE)
                print_all_json(&si, gpus, (int)device_count);
            else
                sys_print_json(&si);
        } else {
            putchar('\n');
            sys_print_text(&si);
        }
    } else if (json_mode && instance != VK_NULL_HANDLE) {
        print_gpu_json(gpus, (int)device_count);
    }

    free(gpus);
    return 0;
}

/* ================================================================
 *  REPL loop
 * ================================================================ */

#define REPL_LINE_MAX 4096

static void repl_loop(VkInstance instance,
                      VkPhysicalDevice *phys_devices,
                      uint32_t device_count)
{
    char line[REPL_LINE_MAX];

    while (!g_exit_requested) {
        printf("gpu-info> ");
        fflush(stdout);

        if (!fgets(line, sizeof(line), stdin)) {
            break;
        }

        /* strip trailing newline */
        size_t len = strlen(line);
        while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r')) {
            line[--len] = '\0';
        }

        if (len == 0) continue;

        /* tokenize */
        char *tokenv[256];
        int tokc = 0;
        char *p = line;
        int in_token = 0;
        while (*p) {
            if (*p == ' ' || *p == '\t') {
                if (in_token) {
                    *p = '\0';
                    in_token = 0;
                }
            } else {
                if (!in_token) {
                    if (tokc < 256) tokenv[tokc++] = p;
                    in_token = 1;
                }
            }
            p++;
        }

        if (tokc == 0) continue;

        if (strcmp(tokenv[0], "--exit") == 0 ||
            strcmp(tokenv[0], "--quit") == 0) {
            break;
        }

        if (strcmp(tokenv[0], "--help") == 0) {
            print_help();
            continue;
        }

        do_output(tokc, tokenv, instance, phys_devices, device_count);
        fputc('\n', stdout);
    }
}

/* ================================================================
 *  Main
 * ================================================================ */

int main(int argc, char *argv[])
{
#ifdef _WIN32
    SetConsoleCtrlHandler(ctrl_handler, TRUE);
#else
    signal(SIGINT, sigint_handler);
#endif

    /* -- early exit for meta commands in batch mode -- */
    {
        int argi;
        for (argi = 1; argi < argc; argi++) {
            if (strcmp(argv[argi], "--exit") == 0 ||
                strcmp(argv[argi], "--quit") == 0) {
                return 0;
            }
            if (strcmp(argv[argi], "--help") == 0) {
                print_help();
                return 0;
            }
        }
    }

    VkResult vkres;
    VkInstance instance = VK_NULL_HANDLE;
    uint32_t device_count = 0;
    VkPhysicalDevice *phys_devices = NULL;
    uint32_t i;

    int interactive = (argc == 1);
    {
        int argi;
        for (argi = 1; argi < argc; argi++) {
            if (strcmp(argv[argi], "--interactive") == 0) interactive = 1;
        }
    }
    int gpu_only = 0;
    {
        int argi;
        for (argi = 1; argi < argc; argi++) {
            if (strcmp(argv[argi], "--gpu") == 0) gpu_only = 1;
        }
    }

    /* decide whether Vulkan / backends are needed */
    int need_vulkan = interactive;
    if (!interactive) {
        int only_cpu_ram = 1;
        int argi;
        for (argi = 1; argi < argc; argi++) {
            if (strcmp(argv[argi], "--cpu") != 0 &&
                strcmp(argv[argi], "--ram") != 0) {
                only_cpu_ram = 0;
                break;
            }
        }
        need_vulkan = !only_cpu_ram;
    }

    if (need_vulkan) {
        if (vulkan_backend_init() != 0) {
            if (gpu_only) {
                fprintf(stderr, "GPU-only mode requested but Vulkan loader is not available.\n");
                return 1;
            }
        }

        if (vulkan_backend_loaded()) {
            VkApplicationInfo app_info = {0};
            app_info.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
            app_info.pApplicationName = "gpu-info";
            app_info.applicationVersion = VK_MAKE_VERSION(1, 0, 0);
            app_info.pEngineName = "gpu-info";
            app_info.engineVersion = VK_MAKE_VERSION(1, 0, 0);
            app_info.apiVersion = VK_API_VERSION_1_1;

            VkInstanceCreateInfo instance_ci = {0};
            instance_ci.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
            instance_ci.pApplicationInfo = &app_info;

            vkres = vkCreateInstance(&instance_ci, NULL, &instance);
            if (vkres != VK_SUCCESS) {
                fprintf(stderr, "Warning: vkCreateInstance failed: %d\n", vkres);
                fprintf(stderr,
                        "Make sure a Vulkan driver is installed.\n"
                        "  Windows: install GPU vendor driver (NVIDIA/AMD/Intel)\n"
                        "  Linux:   apt install vulkan-tools / mesa-vulkan-drivers\n");
                if (gpu_only) {
                    fprintf(stderr, "GPU-only mode requested but no GPU is available.\n");
                    return 1;
                }
            }
        }
    }

    if (instance != VK_NULL_HANDLE) {
#ifdef _WIN32
        adl_init();
        intel_init();
        winmem_init();
#endif
#ifdef __linux__
        sysfs_init();
#endif
        nvml_init();

        vkres = vkEnumeratePhysicalDevices(instance, &device_count, NULL);
        if (vkres != VK_SUCCESS || device_count == 0) {
            fprintf(stderr, "Warning: No Vulkan-capable GPU found.\n");
            vkDestroyInstance(instance, NULL);
            instance = VK_NULL_HANDLE;
            if (gpu_only) {
                return 1;
            }
        }
    }

    if (instance != VK_NULL_HANDLE) {
        phys_devices = (VkPhysicalDevice*)malloc(sizeof(VkPhysicalDevice) * device_count);
        if (!phys_devices) {
            fprintf(stderr, "malloc failed\n");
            vkDestroyInstance(instance, NULL);
            return 1;
        }

        vkres = vkEnumeratePhysicalDevices(instance, &device_count, phys_devices);
        if (vkres != VK_SUCCESS) {
            fprintf(stderr, "vkEnumeratePhysicalDevices failed: %d\n", vkres);
            free(phys_devices);
            vkDestroyInstance(instance, NULL);
            return 1;
        }

        {
            uint32_t keep = 0;
            /* Dedup cache: PCI topology + identity for already-kept
             * devices.  The Vulkan loader may enumerate the same
             * physical GPU multiple times when multiple ICD manifest
             * files register it (common with Intel driver update
             * residues), so we collapse duplicates by PCI address.
             * Two identical discrete GPUs (e.g. 2x RTX 4090) have
             * different PCI addresses and are NOT merged. */
            struct {
                uint32_t domain, bus, device, function;
                int      pci_valid;
                uint32_t vendor_id, device_id;
                char     name[VK_MAX_PHYSICAL_DEVICE_NAME_SIZE];
            } seen[32];
            uint32_t s;

            for (i = 0; i < device_count; i++) {
                VkPhysicalDeviceProperties2 props2 = {0};
                VkPhysicalDevicePCIBusInfoPropertiesEXT pci_props = {0};
                VkPhysicalDeviceProperties props;
                int pci_valid, is_dup = 0;

                vkGetPhysicalDeviceProperties(phys_devices[i], &props);
                if (strstr(props.deviceName, "llvmpipe")) continue;

                /* Query PCI bus info for dedup (same extension already
                 * used in gpu_collect_info).  If the extension is
                 * unsupported the struct stays zeroed; a real GPU is
                 * never at 00:00.0 (that's the host bridge), so
                 * all-zero safely means "no PCI info available". */
                props2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2;
                props2.pNext = &pci_props;
                pci_props.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PCI_BUS_INFO_PROPERTIES_EXT;
                vkGetPhysicalDeviceProperties2(phys_devices[i], &props2);

                pci_valid = (pci_props.pciDomain | pci_props.pciBus |
                             pci_props.pciDevice | pci_props.pciFunction) != 0;

                for (s = 0; s < keep && s < 32; s++) {
                    if (pci_valid && seen[s].pci_valid) {
                        /* Both have PCI info: compare exact topology.
                         * This is what lets identical discrete GPUs
                         * survive dedup -- they differ in PCI slot. */
                        if (seen[s].domain   == pci_props.pciDomain &&
                            seen[s].bus      == pci_props.pciBus &&
                            seen[s].device   == pci_props.pciDevice &&
                            seen[s].function == pci_props.pciFunction) {
                            is_dup = 1;
                            break;
                        }
                    } else if (!pci_valid && !seen[s].pci_valid) {
                        /* Neither has PCI info: fall back to
                         * vendor+device+name identity comparison */
                        if (seen[s].vendor_id == props.vendorID &&
                            seen[s].device_id == props.deviceID &&
                            strcmp(seen[s].name, props.deviceName) == 0) {
                            is_dup = 1;
                            break;
                        }
                    }
                }

                if (is_dup) continue;

                if (keep != i) phys_devices[keep] = phys_devices[i];
                if (keep < 32) {
                    seen[keep].domain    = pci_props.pciDomain;
                    seen[keep].bus       = pci_props.pciBus;
                    seen[keep].device    = pci_props.pciDevice;
                    seen[keep].function  = pci_props.pciFunction;
                    seen[keep].pci_valid = pci_valid;
                    seen[keep].vendor_id = props.vendorID;
                    seen[keep].device_id = props.deviceID;
                    strncpy(seen[keep].name, props.deviceName,
                            sizeof(seen[keep].name) - 1);
                    seen[keep].name[sizeof(seen[keep].name) - 1] = '\0';
                }
                keep++;
            }
            device_count = keep;
        }
    }

    /* -- dispatch -- */
    if (interactive) {
        if (instance == VK_NULL_HANDLE) {
            printf("Interactive mode — no Vulkan GPU detected, system info only.\n");
        } else {
            printf("Interactive mode. Type --help for commands, --exit to quit.\n");
        }
        repl_loop(instance, phys_devices, device_count);
    } else {
        do_output(argc - 1, argv + 1, instance, phys_devices, device_count);
    }

    /* -- cleanup -- */
    free(phys_devices);
    nvml_shutdown();
#ifdef _WIN32
    winmem_shutdown();
    adl_shutdown();
    intel_shutdown();
#endif
#ifdef __linux__
    sysfs_shutdown();
#endif
    if (instance != VK_NULL_HANDLE)
        vkDestroyInstance(instance, NULL);
    vulkan_backend_shutdown();

    return 0;
}
