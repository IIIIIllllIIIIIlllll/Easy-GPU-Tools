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
)

target_link_libraries(gpu-info PRIVATE Vulkan::Vulkan)

if(UNIX)
    target_link_libraries(gpu-info PRIVATE ${CMAKE_DL_LIBS})
endif()

if(WIN32)
    add_custom_command(TARGET gpu-info POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "$ENV{SystemRoot}/System32/vulkan-1.dll"
            "$<TARGET_FILE_DIR:gpu-info>"
        COMMENT "Bundling vulkan-1.dll into output directory"
    )
    if(EXISTS "$ENV{SystemRoot}/System32/nvml.dll")
        add_custom_command(TARGET gpu-info POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "$ENV{SystemRoot}/System32/nvml.dll"
                "$<TARGET_FILE_DIR:gpu-info>"
            COMMENT "Bundling nvml.dll into output directory"
        )
    endif()
endif()
