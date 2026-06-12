cmake_minimum_required(VERSION 3.15)
project(GpuInfo LANGUAGES C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

if(MSVC)
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()

# Vulkan headers only -- no link dependency, loaded dynamically at runtime
find_package(Vulkan REQUIRED)

add_executable(gpu-info
    src/main.c
    src/backend_vulkan.c
    src/backend_adl.c
    src/backend_sysfs.c
    src/backend_nvml.c
    src/backend_sysinfo.c
)

# Vulkan loaded dynamically via backend_vulkan.c -- no import library needed
target_include_directories(gpu-info PRIVATE ${Vulkan_INCLUDE_DIRS})

if(UNIX)
    target_link_libraries(gpu-info PRIVATE ${CMAKE_DL_LIBS})
endif()


