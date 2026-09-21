cmake_minimum_required(VERSION 3.20)
project(vulkan_fix LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_C_STANDARD 11)

add_library(vulkan_fix SHARED
    core/driver_loader.cpp
    core/layer_manager.cpp
    modules/vertex_attribute_divisor.cpp
    modules/fill_mode_non_solid.cpp
    modules/push_descriptor.cpp
    modules/dynamic_rendering.cpp
    vulkan_entry.cpp
    egl_stubs.c
)

target_include_directories(vulkan_fix PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/core
    ${CMAKE_CURRENT_SOURCE_DIR}/modules
)

find_library(LOG_LIB log)

target_link_libraries(vulkan_fix PRIVATE
    ${LOG_LIB}
    dl
)

set_target_properties(vulkan_fix PROPERTIES
    C_VISIBILITY_PRESET default
    CXX_VISIBILITY_PRESET default
    VISIBILITY_INLINES_HIDDEN OFF
)
