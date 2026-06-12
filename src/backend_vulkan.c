/* ================================================================
 *  backend_vulkan.c  --  Vulkan loader dynamic backend
 *
 *  Loads vulkan-1.dll (Windows) or libvulkan.so.1 (Linux) at
 *  runtime, resolves the 6 Vulkan entry points used by gpu-info,
 *  and exposes them as global function pointers.
 *
 *  Entirely linker-free: no import library needed.
 * ================================================================ */

#include <stdlib.h>

#ifdef _WIN32
#include <windows.h>
static HMODULE g_vulkan_lib = NULL;
#define LOAD_LIB()   LoadLibraryA("vulkan-1.dll")
#define GET_FN(l, n) GetProcAddress((HMODULE)(l), n)
#define FREE_LIB(l)  FreeLibrary((HMODULE)(l))
#else
#include <dlfcn.h>
static void *g_vulkan_lib = NULL;
#define LOAD_LIB()   dlopen("libvulkan.so.1", RTLD_NOW)
#define GET_FN(l, n) dlsym((l), n)
#define FREE_LIB(l)  dlclose((l))
#endif

#include "backend_vulkan.h"

static int g_loaded = 0;

PFN_vkCreateInstance                vulkan_CreateInstance = NULL;
PFN_vkDestroyInstance               vulkan_DestroyInstance = NULL;
PFN_vkEnumeratePhysicalDevices      vulkan_EnumeratePhysicalDevices = NULL;
PFN_vkGetPhysicalDeviceProperties   vulkan_GetPhysicalDeviceProperties = NULL;
PFN_vkGetPhysicalDeviceProperties2  vulkan_GetPhysicalDeviceProperties2 = NULL;
PFN_vkGetPhysicalDeviceMemoryProperties vulkan_GetPhysicalDeviceMemoryProperties = NULL;

int vulkan_backend_init(void)
{
    if (g_loaded)  return 0;

    g_vulkan_lib = LOAD_LIB();
#ifndef _WIN32
    if (!g_vulkan_lib)
        g_vulkan_lib = dlopen("libvulkan.so", RTLD_NOW);
#endif
    if (!g_vulkan_lib) return -1;

    vulkan_CreateInstance = (PFN_vkCreateInstance)GET_FN(g_vulkan_lib, "vkCreateInstance");
    vulkan_DestroyInstance = (PFN_vkDestroyInstance)GET_FN(g_vulkan_lib, "vkDestroyInstance");
    vulkan_EnumeratePhysicalDevices = (PFN_vkEnumeratePhysicalDevices)GET_FN(g_vulkan_lib, "vkEnumeratePhysicalDevices");
    vulkan_GetPhysicalDeviceProperties = (PFN_vkGetPhysicalDeviceProperties)GET_FN(g_vulkan_lib, "vkGetPhysicalDeviceProperties");
    vulkan_GetPhysicalDeviceProperties2 = (PFN_vkGetPhysicalDeviceProperties2)GET_FN(g_vulkan_lib, "vkGetPhysicalDeviceProperties2");
    vulkan_GetPhysicalDeviceMemoryProperties = (PFN_vkGetPhysicalDeviceMemoryProperties)GET_FN(g_vulkan_lib, "vkGetPhysicalDeviceMemoryProperties");

    if (!vulkan_CreateInstance || !vulkan_DestroyInstance ||
        !vulkan_EnumeratePhysicalDevices || !vulkan_GetPhysicalDeviceProperties ||
        !vulkan_GetPhysicalDeviceProperties2 || !vulkan_GetPhysicalDeviceMemoryProperties) {
        FREE_LIB(g_vulkan_lib);
        g_vulkan_lib = NULL;
        return -1;
    }

    g_loaded = 1;
    return 0;
}

void vulkan_backend_shutdown(void)
{
    if (g_vulkan_lib) {
        FREE_LIB(g_vulkan_lib);
        g_vulkan_lib = NULL;
    }
    vulkan_CreateInstance = NULL;
    vulkan_DestroyInstance = NULL;
    vulkan_EnumeratePhysicalDevices = NULL;
    vulkan_GetPhysicalDeviceProperties = NULL;
    vulkan_GetPhysicalDeviceProperties2 = NULL;
    vulkan_GetPhysicalDeviceMemoryProperties = NULL;
    g_loaded = 0;
}

int vulkan_backend_loaded(void)
{
    return g_loaded;
}
