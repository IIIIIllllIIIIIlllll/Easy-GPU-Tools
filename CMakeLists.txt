cmake_minimum_required(VERSION 3.15)
project(GpuInfo LANGUAGES C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

if(MSVC)
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()

# Vulkan headers only -- no link dependency, loaded dynamically at runtime
find_package(Vulkan REQUIRED)

# Parse version from the latest git tag (e.g. v1.0.4 -> 1.0.4).
# Falls back to 1.0.0 when git is unavailable or no tag is found.
set(VER_MAJOR 1)
set(VER_MINOR 0)
set(VER_PATCH 0)
execute_process(
    COMMAND git describe --tags --abbrev=0
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
    OUTPUT_VARIABLE _git_tag
    OUTPUT_STRIP_TRAILING_WHITESPACE
    RESULT_VARIABLE _git_tag_rc
    ERROR_QUIET)
if(_git_tag_rc EQUAL 0 AND _git_tag MATCHES "^v([0-9]+)\\.([0-9]+)\\.([0-9]+)")
    set(VER_MAJOR ${CMAKE_MATCH_1})
    set(VER_MINOR ${CMAKE_MATCH_2})
    set(VER_PATCH ${CMAKE_MATCH_3})
endif()
message(STATUS "gpu-info version: ${VER_MAJOR}.${VER_MINOR}.${VER_PATCH}")

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

# Embed VERSIONINFO resource on Windows (reduces heuristic AV false positives).
# Linux/macOS skip the .rc entirely.
if(WIN32)
    configure_file(
        ${CMAKE_SOURCE_DIR}/src/gpu-info.rc.in
        ${CMAKE_BINARY_DIR}/gpu-info.rc
        @ONLY)
    target_sources(gpu-info PRIVATE ${CMAKE_BINARY_DIR}/gpu-info.rc)
endif()

if(UNIX)
    target_link_libraries(gpu-info PRIVATE ${CMAKE_DL_LIBS})
endif()


