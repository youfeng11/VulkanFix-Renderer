cmake_minimum_required(VERSION 3.20)
project(vulkan_fix LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_C_STANDARD 11)

if(NOT DEFINED PROJECT_VERSION_NAME)
    set(PROJECT_VERSION_NAME "1.0")
endif()
if(NOT DEFINED PROJECT_VERSION_CODE)
    find_package(Git QUIET)
    if(GIT_FOUND)
        execute_process(
            COMMAND ${GIT_EXECUTABLE} rev-list --count HEAD
            WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
            OUTPUT_VARIABLE GIT_COMMIT_COUNT
            OUTPUT_STRIP_TRAILING_WHITESPACE
            ERROR_QUIET
        )
        if(GIT_COMMIT_COUNT MATCHES "^[0-9]+$")
            set(PROJECT_VERSION_CODE "${GIT_COMMIT_COUNT}")
        else()
            set(PROJECT_VERSION_CODE "1")
        endif()
    else()
        set(PROJECT_VERSION_CODE "1")
    endif()
endif()

add_compile_definitions(
    PROJECT_VERSION_NAME="${PROJECT_VERSION_NAME}"
    PROJECT_VERSION_CODE="${PROJECT_VERSION_CODE}"
)

# Vulkan-Headers submodule
add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../../../../Vulkan-Headers ${CMAKE_CURRENT_BINARY_DIR}/Vulkan-Headers EXCLUDE_FROM_ALL)

# SPIRV-Tools: SPIR-V parsing/validation/rewriting for the shader downgrade pass.
# Both SPIRV-Tools and SPIRV-Headers are pinned submodules at the repository root.
set(VKFIX_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../../../../")
set(SPIRV_TOOLS_DIR "${VKFIX_ROOT}SPIRV-Tools")
set(SPIRV_HEADERS_DIR "${VKFIX_ROOT}SPIRV-Headers")

if(NOT EXISTS "${SPIRV_TOOLS_DIR}/CMakeLists.txt" OR NOT EXISTS "${SPIRV_HEADERS_DIR}/CMakeLists.txt")
    message(FATAL_ERROR
        "SPIRV-Tools / SPIRV-Headers submodules are missing.\n"
        "Run: git submodule update --init --recursive")
endif()

set(SPIRV-Headers_SOURCE_DIR "${SPIRV_HEADERS_DIR}" CACHE PATH "SPIRV-Headers for SPIRV-Tools" FORCE)
set(SPIRV_SKIP_TESTS ON CACHE BOOL "Skip SPIRV-Tools tests" FORCE)
set(SPIRV_SKIP_EXECUTABLES ON CACHE BOOL "Skip SPIRV-Tools executables" FORCE)
set(SPIRV_TOOLS_BUILD_STATIC ON CACHE BOOL "Build SPIRV-Tools as a static library" FORCE)
set(SPIRV_WERROR OFF CACHE BOOL "Do not treat SPIRV-Tools warnings as errors" FORCE)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
add_subdirectory(${SPIRV_TOOLS_DIR} ${CMAKE_CURRENT_BINARY_DIR}/SPIRV-Tools EXCLUDE_FROM_ALL)

if(TARGET SPIRV-Tools-static)
    set(VKFIX_SPIRV_TOOLS_TARGET SPIRV-Tools-static)
elseif(TARGET SPIRV-Tools)
    set(VKFIX_SPIRV_TOOLS_TARGET SPIRV-Tools)
else()
    message(FATAL_ERROR "SPIRV-Tools target not found after add_subdirectory")
endif()

# Automatically discover all core and module sources
file(GLOB_RECURSE CORE_SOURCES CONFIGURE_DEPENDS core/*.cpp)
file(GLOB_RECURSE MODULE_SOURCES CONFIGURE_DEPENDS modules/*.cpp)
file(GLOB_RECURSE SHADER_SOURCES CONFIGURE_DEPENDS shader/*.cpp)

add_library(vulkan_fix SHARED
    ${CORE_SOURCES}
    ${MODULE_SOURCES}
    ${SHADER_SOURCES}
    vulkan_entry.cpp
    egl_stubs.c
)

# Ensure Vulkan-Headers include directory comes BEFORE NDK system Vulkan headers
target_include_directories(vulkan_fix BEFORE PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../../../../Vulkan-Headers/include
)

target_include_directories(vulkan_fix PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/core
    ${CMAKE_CURRENT_SOURCE_DIR}/modules
    ${CMAKE_CURRENT_SOURCE_DIR}/modules/vulkan_1_0
    ${CMAKE_CURRENT_SOURCE_DIR}/modules/vulkan_1_1
    ${CMAKE_CURRENT_SOURCE_DIR}/modules/vulkan_1_2
    ${CMAKE_CURRENT_SOURCE_DIR}/modules/vulkan_1_3
    ${CMAKE_CURRENT_SOURCE_DIR}/modules/vulkan_1_4
)

find_library(LOG_LIB log)
find_library(ANDROID_LIB android)

target_link_libraries(vulkan_fix PRIVATE
    Vulkan::Headers
    ${VKFIX_SPIRV_TOOLS_TARGET}
    ${LOG_LIB}
    ${ANDROID_LIB}
    dl
)

set_target_properties(vulkan_fix PROPERTIES
    C_VISIBILITY_PRESET hidden
    CXX_VISIBILITY_PRESET hidden
    VISIBILITY_INLINES_HIDDEN ON
)

target_compile_options(vulkan_fix PRIVATE
    $<$<CONFIG:Release>:
        -O3
        -fomit-frame-pointer
        -ffunction-sections
        -fdata-sections
        -flto=thin
    >
)

target_link_options(vulkan_fix PRIVATE
    -Wl,-z,max-page-size=16384
    $<$<CONFIG:Release>:
        -Wl,--gc-sections
        -Wl,-Bsymbolic
        -flto=thin
    >
)

# Standalone Test Runner for Verification and RenderDoc Capture
add_executable(vulkan_fix_test
    tests/vulkan_fix_test.cpp
)

target_include_directories(vulkan_fix_test BEFORE PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../../../../Vulkan-Headers/include
)

target_link_libraries(vulkan_fix_test PRIVATE
    vulkan_fix
    ${LOG_LIB}
    dl
    vulkan
)

