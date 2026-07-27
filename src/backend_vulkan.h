/* ================================================================
 *  backend_vulkan.h  --  Vulkan loader dynamic backend
 *
 *  Loads vulkan-1.dll (Windows) or libvulkan.so.1 (Linux) at
 *  runtime and exposes the handful of Vulkan entry points used
 *  by gpu-info.  Entirely linker-free: no import library needed.
 *
 *  In client code, #define the Vulkan API names to these pointers
 *  to avoid changing existing call sites:
 *
 *    #define vkCreateInstance            vulkan_CreateInstance
 *    #define vkDestroyInstance           vulkan_DestroyInstance
 *    #define vkEnumeratePhysicalDevices  vulkan_EnumeratePhysicalDevices
 *    #define vkGetPhysicalDeviceProperties   vulkan_GetPhysicalDeviceProperties
 *    #define vkGetPhysicalDeviceProperties2  vulkan_GetPhysicalDeviceProperties2
 *    #define vkGetPhysicalDeviceMemoryProperties vulkan_GetPhysicalDeviceMemoryProperties
 * ================================================================ */

#ifndef BACKEND_VULKAN_H
#define BACKEND_VULKAN_H

#include <vulkan/vulkan.h>

/* Function pointer globals — populated by vulkan_backend_init() */
extern PFN_vkCreateInstance                vulkan_CreateInstance;
extern PFN_vkDestroyInstance               vulkan_DestroyInstance;
extern PFN_vkEnumeratePhysicalDevices      vulkan_EnumeratePhysicalDevices;
extern PFN_vkGetPhysicalDeviceProperties   vulkan_GetPhysicalDeviceProperties;
/* May be NULL: Vulkan 1.0-era loaders neither export the core (1.1)
 * vkGetPhysicalDeviceProperties2 symbol nor support a 1.1 instance.
 * Call vulkan_backend_resolve_properties2() after instance creation to
 * fill it via VK_KHR_get_physical_device_properties2 when available. */
extern PFN_vkGetPhysicalDeviceProperties2  vulkan_GetPhysicalDeviceProperties2;
extern PFN_vkGetPhysicalDeviceMemoryProperties vulkan_GetPhysicalDeviceMemoryProperties;
extern PFN_vkGetInstanceProcAddr           vulkan_GetInstanceProcAddr;
extern PFN_vkEnumerateInstanceExtensionProperties vulkan_EnumerateInstanceExtensionProperties;

/* Load the Vulkan loader shared library. Returns 0 on success, -1 on failure. */
int  vulkan_backend_init(void);

/* Resolve vkGetPhysicalDeviceProperties2 for a created instance: tries the
 * core (1.1) name first, then the VK_KHR_get_physical_device_properties2
 * alias used on Vulkan 1.0 instances. */
void vulkan_backend_resolve_properties2(VkInstance instance);

/* Unload the library and null-out all pointers. Safe to call anytime. */
void vulkan_backend_shutdown(void);

/* Returns 1 if the backend was successfully loaded. */
int  vulkan_backend_loaded(void);

#endif
