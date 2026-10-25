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
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

# Build SPIRV-Tools in its own build tree and only the static library.
#
# Upstream always creates a SPIRV-Tools-shared target, and AGP builds every
# library/executable target it finds in the CMake project - which doubles the
# compile time and exhausted memory when the four ABIs were built in parallel.
# ExternalProject keeps SPIRV-Tools' targets out of AGP's target list and lets
# us ask for SPIRV-Tools-static only.
# Keep the SPIRV-Tools build tree outside the AGP-hashed .cxx directory. A
# CMake/AGP configuration change creates a new .cxx hash directory; without this
# cache the validator would be recompiled from scratch (several minutes per ABI).
# Bump the v1 suffix when SPIRV_TOOLS_CMAKE_ARGS change.
set(SPIRV_TOOLS_BUILD_DIR
    "${VKFIX_ROOT}.cxx-spirv-tools/v1/${ANDROID_ABI}-${CMAKE_BUILD_TYPE}")
set(SPIRV_TOOLS_STATIC_LIB "${SPIRV_TOOLS_BUILD_DIR}/source/libSPIRV-Tools.a")

include(ExternalProject)

set(SPIRV_TOOLS_CMAKE_ARGS
    -DCMAKE_BUILD_TYPE=${CMAKE_BUILD_TYPE}
    -DANDROID_ABI=${ANDROID_ABI}
    -DANDROID_STL=c++_static
    -DSPIRV_SKIP_TESTS=ON
    -DSPIRV_SKIP_EXECUTABLES=ON
    -DSPIRV_TOOLS_BUILD_STATIC=ON
    -DSPIRV_WERROR=OFF
    "-DSPIRV-Headers_SOURCE_DIR=${SPIRV_HEADERS_DIR}"
    # Size-oriented release build: the validator is only used for downgrade
    # checks, and function sections let the linker drop unreachable validators.
    "-DCMAKE_CXX_FLAGS_RELEASE=-Os -DNDEBUG -ffunction-sections -fdata-sections"
    "-DCMAKE_C_FLAGS_RELEASE=-Os -DNDEBUG -ffunction-sections -fdata-sections"
    "-DCMAKE_CXX_FLAGS_RELWITHDEBINFO=-Os -g -DNDEBUG -ffunction-sections -fdata-sections"
    "-DCMAKE_C_FLAGS_RELWITHDEBINFO=-Os -g -DNDEBUG -ffunction-sections -fdata-sections"
)
if(CMAKE_TOOLCHAIN_FILE)
    list(APPEND SPIRV_TOOLS_CMAKE_ARGS "-DCMAKE_TOOLCHAIN_FILE=${CMAKE_TOOLCHAIN_FILE}")
endif()
if(ANDROID_PLATFORM)
    list(APPEND SPIRV_TOOLS_CMAKE_ARGS "-DANDROID_PLATFORM=${ANDROID_PLATFORM}")
endif()
if(CMAKE_MAKE_PROGRAM)
    list(APPEND SPIRV_TOOLS_CMAKE_ARGS "-DCMAKE_MAKE_PROGRAM=${CMAKE_MAKE_PROGRAM}")
endif()

ExternalProject_Add(spirv_tools_external
    SOURCE_DIR "${SPIRV_TOOLS_DIR}"
    BINARY_DIR "${SPIRV_TOOLS_BUILD_DIR}"
    CMAKE_ARGS ${SPIRV_TOOLS_CMAKE_ARGS}
    BUILD_COMMAND ${CMAKE_COMMAND} --build <BINARY_DIR> --target SPIRV-Tools-static --parallel 4
    INSTALL_COMMAND ""
    UPDATE_COMMAND ""
    BUILD_BYPRODUCTS "${SPIRV_TOOLS_STATIC_LIB}"
)

add_library(SPIRV-Tools-static STATIC IMPORTED GLOBAL)
set_target_properties(SPIRV-Tools-static PROPERTIES
    IMPORTED_LOCATION "${SPIRV_TOOLS_STATIC_LIB}"
    INTERFACE_INCLUDE_DIRECTORIES "${SPIRV_TOOLS_DIR}/include;${SPIRV_HEADERS_DIR}/include"
)
add_dependencies(SPIRV-Tools-static spirv_tools_external)

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
    ${SPIRV_HEADERS_DIR}/include
)

find_library(LOG_LIB log)
find_library(ANDROID_LIB android)

target_link_libraries(vulkan_fix PRIVATE
    Vulkan::Headers
    SPIRV-Tools-static
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

# Device-run validation of the SPIR-V compute shader used by the
# drawIndirectCount expansion. Not packaged into the APK.
add_executable(indirect_expander_test
    tests/indirect_expander_test.cpp
)
target_include_directories(indirect_expander_test PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/../../../../Vulkan-Headers/include
)
target_link_libraries(indirect_expander_test PRIVATE
    SPIRV-Tools-static
    ${LOG_LIB}
    vulkan
)

# Cache reuse check: the SPIRV-Tools tree above is intentionally stable across AGP .cxx hashes.

# Opt-in: AGP explicitly builds executable targets even with EXCLUDE_FROM_ALL.
option(VULKAN_FIX_BUILD_REGRESSION_TESTS "Build mock-driver regression runner" OFF)
if(VULKAN_FIX_BUILD_REGRESSION_TESTS)
    add_executable(extension_dependencies_test EXCLUDE_FROM_ALL
        tests/extension_dependencies_test.cpp core/extension_dependencies.cpp core/native_instance_state.cpp)
    target_include_directories(extension_dependencies_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    target_link_libraries(extension_dependencies_test PRIVATE Vulkan::Headers)
    add_library(extension_dependencies_driver SHARED EXCLUDE_FROM_ALL tests/extension_dependencies_driver.cpp)
    target_link_libraries(extension_dependencies_driver PRIVATE Vulkan::Headers)
    target_link_options(extension_dependencies_driver PRIVATE "-Wl,-Bsymbolic-functions")
    add_executable(extension_dependencies_integration_test EXCLUDE_FROM_ALL tests/extension_dependencies_integration_test.cpp)
    target_link_libraries(extension_dependencies_integration_test PRIVATE Vulkan::Headers dl)
    add_executable(validation_driver_test EXCLUDE_FROM_ALL tests/validation_driver_test.cpp)
    target_link_libraries(validation_driver_test PRIVATE Vulkan::Headers dl)
    add_executable(push_descriptor_gpu_test EXCLUDE_FROM_ALL tests/push_descriptor_gpu_test.cpp)
    target_include_directories(push_descriptor_gpu_test BEFORE PRIVATE
        $<TARGET_PROPERTY:vulkan_fix,INCLUDE_DIRECTORIES>)
    target_link_libraries(push_descriptor_gpu_test PRIVATE vulkan_fix Vulkan::Headers SPIRV-Tools-static ${LOG_LIB} vulkan dl)
    add_executable(barrier_optimizer_test EXCLUDE_FROM_ALL
        tests/barrier_optimizer_test.cpp core/tbdr_barrier_optimizer.cpp)
    target_include_directories(barrier_optimizer_test BEFORE PRIVATE
        $<TARGET_PROPERTY:vulkan_fix,INCLUDE_DIRECTORIES>)
    target_link_libraries(barrier_optimizer_test PRIVATE Vulkan::Headers ${LOG_LIB})
    add_executable(timeline_scheduler_test EXCLUDE_FROM_ALL
        tests/timeline_scheduler_test.cpp core/timeline_scheduler.cpp core/input_pnext_copy.cpp)
    target_include_directories(timeline_scheduler_test BEFORE PRIVATE
        $<TARGET_PROPERTY:vulkan_fix,INCLUDE_DIRECTORIES>)
    target_link_libraries(timeline_scheduler_test PRIVATE Vulkan::Headers ${LOG_LIB})
    add_executable(timeline_gpu_test EXCLUDE_FROM_ALL tests/timeline_gpu_test.cpp)
    target_include_directories(timeline_gpu_test BEFORE PRIVATE
        $<TARGET_PROPERTY:vulkan_fix,INCLUDE_DIRECTORIES>)
    target_link_libraries(timeline_gpu_test PRIVATE vulkan_fix Vulkan::Headers ${LOG_LIB} vulkan dl)
    add_executable(subgroup_broadcast_test EXCLUDE_FROM_ALL
        tests/subgroup_broadcast_test.cpp shader/subgroup_broadcast_lowering.cpp shader/shader_rewriter.cpp)
    target_include_directories(subgroup_broadcast_test BEFORE PRIVATE
        $<TARGET_PROPERTY:vulkan_fix,INCLUDE_DIRECTORIES>)
    target_link_libraries(subgroup_broadcast_test PRIVATE Vulkan::Headers SPIRV-Tools-static ${LOG_LIB})
    add_executable(subgroup_broadcast_gpu_test EXCLUDE_FROM_ALL tests/subgroup_broadcast_gpu_test.cpp)
    target_include_directories(subgroup_broadcast_gpu_test BEFORE PRIVATE
        $<TARGET_PROPERTY:vulkan_fix,INCLUDE_DIRECTORIES>)
    target_link_libraries(subgroup_broadcast_gpu_test PRIVATE vulkan_fix Vulkan::Headers SPIRV-Tools-static ${LOG_LIB} vulkan dl)
    add_executable(promoted_properties_test EXCLUDE_FROM_ALL
        tests/promoted_properties_test.cpp core/promoted_properties.cpp core/native_instance_state.cpp)
    target_include_directories(promoted_properties_test BEFORE PRIVATE
        $<TARGET_PROPERTY:vulkan_fix,INCLUDE_DIRECTORIES>)
    target_link_libraries(promoted_properties_test PRIVATE Vulkan::Headers ${LOG_LIB})
    add_executable(core_properties_gpu_test EXCLUDE_FROM_ALL tests/core_properties_gpu_test.cpp)
    target_include_directories(core_properties_gpu_test BEFORE PRIVATE
        $<TARGET_PROPERTY:vulkan_fix,INCLUDE_DIRECTORIES>)
    target_link_libraries(core_properties_gpu_test PRIVATE vulkan_fix ${LOG_LIB} vulkan dl)
    add_executable(promoted_features_test EXCLUDE_FROM_ALL
        tests/promoted_features_test.cpp core/promoted_features.cpp
        core/subgroup_broadcast.cpp core/promoted_properties.cpp core/native_instance_state.cpp)
    target_include_directories(promoted_features_test BEFORE PRIVATE
        $<TARGET_PROPERTY:vulkan_fix,INCLUDE_DIRECTORIES>)
    target_link_libraries(promoted_features_test PRIVATE Vulkan::Headers ${LOG_LIB})
    add_executable(core_features_gpu_test EXCLUDE_FROM_ALL tests/core_features_gpu_test.cpp)
    target_include_directories(core_features_gpu_test BEFORE PRIVATE
        $<TARGET_PROPERTY:vulkan_fix,INCLUDE_DIRECTORIES>)
    target_link_libraries(core_features_gpu_test PRIVATE vulkan_fix ${LOG_LIB} vulkan dl)
    # Deterministic regression runner with a mock driver; no GPU is required.
    # Explicit target only, to avoid duplicating the native build during packaging.
    set(REGRESSION_CORE_SOURCES ${CORE_SOURCES})
    list(FILTER REGRESSION_CORE_SOURCES EXCLUDE REGEX "driver_loader\\.cpp$")
    add_executable(compatibility_regression_test EXCLUDE_FROM_ALL
        tests/compatibility_regression_test.cpp
        tests/timeline_regression_cases.cpp
        tests/push_descriptor_regression_cases.cpp
        ${REGRESSION_CORE_SOURCES}
        ${MODULE_SOURCES}
        ${SHADER_SOURCES}
    )
    target_include_directories(compatibility_regression_test BEFORE PRIVATE
        $<TARGET_PROPERTY:vulkan_fix,INCLUDE_DIRECTORIES>
    )
    target_link_libraries(compatibility_regression_test PRIVATE
        Vulkan::Headers SPIRV-Tools-static ${LOG_LIB} ${ANDROID_LIB} dl
    )
    add_executable(vertex_store_gpu_test EXCLUDE_FROM_ALL tests/vertex_store_gpu_test.cpp)
    target_include_directories(vertex_store_gpu_test BEFORE PRIVATE $<TARGET_PROPERTY:vulkan_fix,INCLUDE_DIRECTORIES>)
    target_link_libraries(vertex_store_gpu_test PRIVATE vulkan_fix SPIRV-Tools-static ${LOG_LIB} vulkan dl)
endif()
