cmake_minimum_required(VERSION 3.20)
project(vulkan_fix LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_C_STANDARD 11)

# Automatically discover all core and module sources
file(GLOB_RECURSE CORE_SOURCES core/*.cpp)
file(GLOB_RECURSE MODULE_SOURCES modules/*.cpp)

add_library(vulkan_fix SHARED
    ${CORE_SOURCES}
    ${MODULE_SOURCES}
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
