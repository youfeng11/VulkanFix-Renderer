cmake_minimum_required(VERSION 3.20)
project(vulkan_fix LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_C_STANDARD 11)

if(NOT DEFINED PROJECT_VERSION_NAME)
    set(PROJECT_VERSION_NAME "1.0")
endif()
if(NOT DEFINED PROJECT_VERSION_CODE)
    set(PROJECT_VERSION_CODE "1")
endif()

add_compile_definitions(
    PROJECT_VERSION_NAME="${PROJECT_VERSION_NAME}"
    PROJECT_VERSION_CODE="${PROJECT_VERSION_CODE}"
)

# Automatically discover all core and module sources
file(GLOB_RECURSE CORE_SOURCES CONFIGURE_DEPENDS core/*.cpp)
file(GLOB_RECURSE MODULE_SOURCES CONFIGURE_DEPENDS modules/*.cpp)

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
