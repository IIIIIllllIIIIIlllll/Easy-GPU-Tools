cmake_minimum_required(VERSION 3.15)
project(GpuInfo LANGUAGES C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

if(MSVC)
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()

find_package(Vulkan REQUIRED)

add_executable(gpu-info
    src/main.c
    src/backend_adl.c
    src/backend_sysfs.c
    src/backend_nvml.c
    src/backend_sysinfo.c
)

target_link_libraries(gpu-info PRIVATE Vulkan::Vulkan)

if(UNIX)
    target_link_libraries(gpu-info PRIVATE ${CMAKE_DL_LIBS})
endif()


