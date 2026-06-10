#include <vulkan/vulkan.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "backend_adl.h"
#include "backend_sysfs.h"
#include "backend_nvml.h"

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

/* ================================================================
 *  Core
 * ================================================================ */

int main(void)
{
    VkResult vkres;
    VkInstance instance = VK_NULL_HANDLE;
    uint32_t device_count = 0;
    VkPhysicalDevice *phys_devices = NULL;
    uint32_t i;
    uint32_t total = 0;

    /* -- create instance -------------------------------------------------- */
    VkApplicationInfo app_info = {0};
    app_info.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    app_info.pApplicationName = "gpu-info";
    app_info.applicationVersion = VK_MAKE_VERSION(1, 0, 0);
    app_info.pEngineName = "gpu-info";
    app_info.engineVersion = VK_MAKE_VERSION(1, 0, 0);
    app_info.apiVersion = VK_API_VERSION_1_0;

    VkInstanceCreateInfo instance_ci = {0};
    instance_ci.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    instance_ci.pApplicationInfo = &app_info;

    vkres = vkCreateInstance(&instance_ci, NULL, &instance);
    if (vkres != VK_SUCCESS) {
        fprintf(stderr, "vkCreateInstance failed: %d\n", vkres);
        fprintf(stderr,
                "Make sure a Vulkan driver is installed.\n"
                "  Windows: install GPU vendor driver (NVIDIA/AMD/Intel)\n"
                "  Linux:   apt install vulkan-tools / mesa-vulkan-drivers\n");
        return 1;
    }

    /* -- init ADL (AMD dynamic info, Windows only) ------------------------ */
#ifdef _WIN32
    if (adl_init() != 0) {
        /* not an error -- just means no AMD GPU / driver, we still show
         * Vulkan info for whatever GPUs are present. */
    }
#endif

    /* -- init sysfs (AMD dynamic info, Linux only) ------------------------- */
#ifdef __linux__
    if (sysfs_init() != 0) {
        /* not an error -- AMDGPU kernel module may not be loaded. */
    }
#endif

    /* -- init NVML (NVIDIA dynamic info, cross-platform) ------------------- */
    if (nvml_init() != 0) {
        /* not an error -- just means no NVIDIA GPU / driver. */
    }

    /* -- enumerate physical devices --------------------------------------- */
    vkres = vkEnumeratePhysicalDevices(instance, &device_count, NULL);
    if (vkres != VK_SUCCESS || device_count == 0) {
        fprintf(stderr, "No Vulkan-capable GPU found.\n");
        vkDestroyInstance(instance, NULL);
        return 1;
    }

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

    /* -- display ---------------------------------------------------------- */
    printf("===== GPU Information (Vulkan + ADL/sysfs + NVML) =====\n");
    printf("(Plain C -- cross-platform + dynamic info)\n");
    printf("=======================================================\n\n");

    for (i = 0; i < device_count; i++) {
        VkPhysicalDeviceProperties props;
        VkPhysicalDeviceMemoryProperties mem;

        vkGetPhysicalDeviceProperties(phys_devices[i], &props);
        vkGetPhysicalDeviceMemoryProperties(phys_devices[i], &mem);

        /* calculate memory sums */
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

        /* memory heap breakdown */
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

        /* -- ADL dynamic info for AMD GPUs (Windows only) ------------------- */
#ifdef _WIN32
        if (props.vendorID == 0x1002) {
            int adl_idx;
            printf("\n  --- AMD Dynamic Info (ADL) ---\n");
            if (adl_find_by_name(props.deviceName, &adl_idx) == 0) {
                int t, rpm, pct, eng, mem_clk, act, lvl;
                int core_mhz, mem_mhz;

                /* temperature */
                if (adl_get_temperature(adl_idx, &t) == 0)
                    printf("  GPU Temperature : %.1f C\n", t / 1000.0);
                else
                    printf("  GPU Temperature : N/A\n");

                /* fan */
                if (adl_get_fan_speed(adl_idx, &rpm, &pct) == 0)
                    printf("  Fan Speed       : %d%% (%d RPM)\n", pct, rpm);
                else
                    printf("  Fan Speed       : N/A\n");

                /* clocks -- try Overdrive first, fallback to AdapterSpeed */
                if (adl_get_activity(adl_idx, &eng, &mem_clk, &act, &lvl) == 0) {
                    printf("  GPU Utilization : %d%%\n", act);
                    printf("  Engine Clock    : %d MHz\n", eng / 100);
                    printf("  Memory Clock    : %d MHz\n", mem_clk / 100);
                    printf("  Perf Level      : P%d\n", lvl);
                } else {
                    printf("  GPU Utilization : N/A\n");
                    if (adl_get_speed(adl_idx, &core_mhz, &mem_mhz) == 0) {
                        printf("  Engine Clock    : %d MHz\n", core_mhz);
                        printf("  Memory Clock    : %d MHz\n", mem_mhz);
                    } else {
                        printf("  Engine Clock    : N/A\n");
                        printf("  Memory Clock    : N/A\n");
                    }
                    printf("  Perf Level      : N/A\n");
                }
            } else {
                printf("  (could not match ADL adapter)\n");
            }
        }
#endif

        /* -- sysfs dynamic info for AMD GPUs (Linux only) ----------------- */
#ifdef __linux__
        if (props.vendorID == 0x1002) {
            int s_idx;
            printf("\n  --- AMD Dynamic Info (sysfs) ---\n");
            if (sysfs_find_by_vendor_device(
                    (int)props.vendorID, (int)props.deviceID, &s_idx) == 0) {
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
            } else {
                printf("  (could not match /sys/class/drm/ device)\n");
            }
        }
#endif

        /* -- NVML dynamic info for NVIDIA GPUs (cross-platform) ------------- */
        if (props.vendorID == 0x10DE) {
            int nv_idx;
            printf("\n  --- NVIDIA Dynamic Info (NVML) ---\n");
            if (nvml_find_by_device_id(props.deviceID, &nv_idx) == 0) {
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

        printf("\n");
        total++;
    }

    /* -- cleanup ---------------------------------------------------------- */
    free(phys_devices);
    nvml_shutdown();
#ifdef _WIN32
    adl_shutdown();
#endif
#ifdef __linux__
    sysfs_shutdown();
#endif
    vkDestroyInstance(instance, NULL);

    printf("---\n");
    printf("Total: %u GPU(s) detected.\n", total);

    return 0;
}
