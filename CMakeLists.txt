cmake_minimum_required(VERSION 3.10)

# Windows Clang builds need a resource compiler during compiler platform setup.
# Detect LLVM's llvm-rc before project() so users do not have to pass
# -DCMAKE_RC_COMPILER manually when clang.exe is outside PATH.
if(CMAKE_HOST_WIN32 AND NOT CMAKE_RC_COMPILER)
    set(_ROCKETDB_LLVM_HINTS)
    if(CMAKE_C_COMPILER)
        get_filename_component(_ROCKETDB_C_COMPILER_DIR "${CMAKE_C_COMPILER}" DIRECTORY)
        list(APPEND _ROCKETDB_LLVM_HINTS "${_ROCKETDB_C_COMPILER_DIR}")
    endif()
    list(APPEND _ROCKETDB_LLVM_HINTS
        "$ENV{ProgramFiles}/LLVM/bin"
        "$ENV{ProgramFiles\(x86\)}/LLVM/bin"
        "D:/Programs/LLVM/bin"
    )
    find_program(CMAKE_RC_COMPILER
        NAMES llvm-rc rc
        HINTS ${_ROCKETDB_LLVM_HINTS}
        DOC "Windows resource compiler"
    )
endif()

if(CMAKE_HOST_WIN32 AND CMAKE_C_COMPILER MATCHES "clang(\\.exe)?$")
    set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)
endif()

project(RocketDB C)

# =============================================================================
# RocketDB CMake Configuration
# Supports: Windows, Linux, macOS
# Build types: Release, Debug, RelWithDebInfo
# =============================================================================

set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

# -----------------------------------------------------------------------------
# Platform Detection
# -----------------------------------------------------------------------------

message(STATUS "=== RocketDB Cross-Platform Build ===")
message(STATUS "Platform: ${CMAKE_SYSTEM_NAME}")
message(STATUS "Processor: ${CMAKE_SYSTEM_PROCESSOR}")
message(STATUS "Compiler: ${CMAKE_C_COMPILER_ID}")
message(STATUS "Build type: ${CMAKE_BUILD_TYPE}")

# Platform-specific settings
if(WIN32)
    message(STATUS "Windows build detected")
    add_definitions(-D_CRT_SECURE_NO_WARNINGS)
    add_definitions(-D_POSIX_C_SOURCE)
    if(CMAKE_C_COMPILER_ID STREQUAL "Clang" AND CMAKE_C_COMPILER_FRONTEND_VARIANT STREQUAL "GNU")
        # Some Windows Clang/CMake combinations generate a freestanding
        # executable link rule (-nostartfiles -nostdlib). Use the Clang driver
        # default link rule so host-side examples and tests link the CRT.
        set(CMAKE_C_LINK_EXECUTABLE
            "<CMAKE_C_COMPILER> <FLAGS> <CMAKE_C_LINK_FLAGS> <LINK_FLAGS> <OBJECTS> -o <TARGET> <LINK_LIBRARIES>")
    endif()
elseif(APPLE)
    message(STATUS "macOS build detected")
    add_definitions(-D_POSIX_C_SOURCE=199309L)
    set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -fPIC")
elseif(UNIX AND NOT APPLE)
    message(STATUS "Linux build detected")
    add_definitions(-D_POSIX_C_SOURCE=199309L)
    set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -fPIC")
endif()

# -----------------------------------------------------------------------------
# Build Options
# -----------------------------------------------------------------------------

option(BUILD_EXAMPLES "Build example programs" ON)
option(BUILD_TESTS "Build test suite" ON)
option(BUILD_PERF "Build performance benchmark" OFF)
option(ENABLE_DEBUG_LOGGING "Enable debug logging" OFF)

if(BUILD_SHARED_LIBS)
    message(WARNING
        "BUILD_SHARED_LIBS is ignored. RocketDB builds static libraries because "
        "CRC/hash/HAL symbols are supplied by the final embedded application.")
endif()

if(ENABLE_DEBUG_LOGGING)
    add_definitions(-DRDB_DEBUG_LOG=1)
    message(STATUS "Debug logging enabled")
endif()

# -----------------------------------------------------------------------------
# Target Helpers
# -----------------------------------------------------------------------------

function(rocketdb_apply_warnings target_name)
    if(MSVC)
        target_compile_options(${target_name} PRIVATE
            /W4
            $<$<CONFIG:Debug>:/WX>
        )
    elseif(CMAKE_C_COMPILER_ID MATCHES "Clang|GNU")
        target_compile_options(${target_name} PRIVATE
            -Wall
            -Wextra
            $<$<CONFIG:Debug>:-Werror>
        )
    endif()
endfunction()

function(rocketdb_apply_optimization target_name)
    if(MSVC)
        target_compile_options(${target_name} PRIVATE
            $<$<CONFIG:Debug>:/Od>
            $<$<CONFIG:Release>:/O2>
            $<$<CONFIG:RelWithDebInfo>:/O2>
        )
    elseif(CMAKE_C_COMPILER_ID MATCHES "Clang|GNU")
        target_compile_options(${target_name} PRIVATE
            $<$<CONFIG:Debug>:-O0>
            $<$<CONFIG:Release>:-O3>
            $<$<CONFIG:RelWithDebInfo>:-O2>
        )
    endif()
endfunction()

function(rocketdb_configure_target target_name)
    target_include_directories(${target_name} PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)
    rocketdb_apply_warnings(${target_name})
    rocketdb_apply_optimization(${target_name})
endfunction()

# -----------------------------------------------------------------------------
# Library Targets
# -----------------------------------------------------------------------------

# Core KVDB library
add_library(rocketdb_kvdb STATIC
    src/rocketdb_kvdb.c
)
rocketdb_configure_target(rocketdb_kvdb)

# Core TSDB library
add_library(rocketdb_tsdb STATIC
    src/rocketdb_tsdb.c
)
rocketdb_configure_target(rocketdb_tsdb)

# Combined RocketDB library
add_library(rocketdb STATIC
    src/rocketdb_kvdb.c
    src/rocketdb_tsdb.c
)
rocketdb_configure_target(rocketdb)

# -----------------------------------------------------------------------------
# Example Programs
# -----------------------------------------------------------------------------

if(BUILD_EXAMPLES)
    message(STATUS "Building examples...")
    
    # KVDB Basic Example
    if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/examples/example_kvdb_basic.c")
        add_executable(example_kvdb_basic
            examples/example_kvdb_basic.c
        )
        target_link_libraries(example_kvdb_basic PRIVATE rocketdb)
        rocketdb_apply_warnings(example_kvdb_basic)
        rocketdb_apply_optimization(example_kvdb_basic)
    else()
        message(STATUS "Skipping example_kvdb_basic: examples/example_kvdb_basic.c not found")
    endif()
    
    # TSDB Sensor Example (if available)
    if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/examples/example_tsdb_sensor.c")
        add_executable(example_tsdb_sensor
            examples/example_tsdb_sensor.c
        )
        target_link_libraries(example_tsdb_sensor PRIVATE rocketdb)
        rocketdb_apply_warnings(example_tsdb_sensor)
        rocketdb_apply_optimization(example_tsdb_sensor)
    endif()
    
endif()

# -----------------------------------------------------------------------------
# Testing
# -----------------------------------------------------------------------------

if(BUILD_TESTS)
    enable_testing()
    message(STATUS "Testing enabled")

    add_library(rocketdb_sim_support STATIC
        test/sim/test_framework.c
        test/sim/sim_flash.c
        test/sim/sim_fault.c
        test/sim/sim_crypto.c
        test/sim/sim_dist.c
        test/sim/sim_trace.c
    )
    target_include_directories(rocketdb_sim_support PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/src
        ${CMAKE_CURRENT_SOURCE_DIR}/test/sim
    )
    rocketdb_apply_warnings(rocketdb_sim_support)
    rocketdb_apply_optimization(rocketdb_sim_support)
    if(UNIX AND NOT APPLE)
        target_link_libraries(rocketdb_sim_support PUBLIC m)
    elseif(APPLE)
        target_link_libraries(rocketdb_sim_support PUBLIC m)
    endif()

    set(ROCKETDB_TESTS
        test_kvdb_basic
        test_kvdb_stress
        test_tsdb_basic
        test_tsdb_stress
        test_integration
        test_fault_injection
        test_example
        test_kvdb_cache
    )

    foreach(test_name IN LISTS ROCKETDB_TESTS)
        add_executable(${test_name}
            test/sim/${test_name}.c
        )
        target_link_libraries(${test_name} PRIVATE rocketdb rocketdb_sim_support)
        target_include_directories(${test_name} PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}/src
            ${CMAKE_CURRENT_SOURCE_DIR}/test/sim
        )
        rocketdb_apply_warnings(${test_name})
        rocketdb_apply_optimization(${test_name})
        add_test(NAME ${test_name} COMMAND ${test_name})
        set_tests_properties(${test_name} PROPERTIES
            WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
        )
    endforeach()

    # test_kvdb_cache enables the KV key cache for cache-specific test coverage.
    target_compile_definitions(test_kvdb_cache PRIVATE
        RDB_KV_CACHE_SIZE=64
    )

    # Keep example smoke coverage when examples are present.
    if(BUILD_EXAMPLES AND TARGET example_kvdb_basic)
        add_test(
            NAME KVDB_Basic_Example
            COMMAND example_kvdb_basic
        )
    endif()

    if(BUILD_EXAMPLES AND TARGET example_tsdb_sensor)
        add_test(
            NAME TSDB_Sensor_Example
            COMMAND example_tsdb_sensor
        )
    endif()
endif()

# -----------------------------------------------------------------------------
# Performance Benchmark
# -----------------------------------------------------------------------------

if(BUILD_PERF)
    add_executable(rocketdb_perf
        test/perf/scenarios.c
        test/sim/sim_flash.c
        test/sim/sim_fault.c
        test/sim/sim_trace.c
        test/sim/sim_crypto.c
    )
    target_link_libraries(rocketdb_perf PRIVATE rocketdb)
    target_include_directories(rocketdb_perf PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
        ${CMAKE_CURRENT_SOURCE_DIR}/test/sim
        ${CMAKE_CURRENT_SOURCE_DIR}/test/perf
    )
    rocketdb_apply_warnings(rocketdb_perf)
    rocketdb_apply_optimization(rocketdb_perf)
endif()

# -----------------------------------------------------------------------------
# Installation
# -----------------------------------------------------------------------------

# Install headers
install(FILES src/rocketdb.h
    DESTINATION include
)

# Install libraries
install(TARGETS rocketdb
    LIBRARY DESTINATION lib
    ARCHIVE DESTINATION lib
    RUNTIME DESTINATION bin
)

# -----------------------------------------------------------------------------
# Summary
# -----------------------------------------------------------------------------

message(STATUS "")
message(STATUS "=== Build Configuration Summary ===")
message(STATUS "Build type: ${CMAKE_BUILD_TYPE}")
message(STATUS "Library type: static")
message(STATUS "Examples: ${BUILD_EXAMPLES}")
message(STATUS "Tests: ${BUILD_TESTS}")
message(STATUS "Performance benchmark: ${BUILD_PERF}")
message(STATUS "Debug logging: ${ENABLE_DEBUG_LOGGING}")
message(STATUS "========================================")
message(STATUS "")
message(STATUS "To build, run:")
message(STATUS "  mkdir build && cd build")
message(STATUS "  cmake ..")
message(STATUS "  cmake --build . --config Release")
message(STATUS "")
