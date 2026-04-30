cmake_minimum_required(VERSION 3.10)
project(RocketDB C)

# ═══════════════════════════════════════════════════════════════════════════
# RocketDB CMake Configuration
# Supports: Windows, Linux, macOS
# Build types: Release, Debug, RelWithDebInfo
# ═══════════════════════════════════════════════════════════════════════════

set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_FLAGS_DEBUG "-O0 -g -Wall -Wextra -Werror")
set(CMAKE_C_FLAGS_RELEASE "-O3 -DNDEBUG -Wall -Wextra")
set(CMAKE_C_FLAGS_RELWITHDEBINFO "-O2 -g -Wall -Wextra")

# ───────────────────────────────────────────────────────────────────────────
# Platform Detection
# ───────────────────────────────────────────────────────────────────────────

message(STATUS "=== RocketDB Cross-Platform Build ===")
message(STATUS "Platform: ${CMAKE_SYSTEM_NAME}")
message(STATUS "Processor: ${CMAKE_SYSTEM_PROCESSOR}")
message(STATUS "Compiler: ${CMAKE_C_COMPILER_ID}")
message(STATUS "Build type: ${CMAKE_BUILD_TYPE}")

# Platform-specific settings
if(WIN32)
    message(STATUS "🪟 Windows build detected")
    add_definitions(-D_CRT_SECURE_NO_WARNINGS)
    add_definitions(-D_POSIX_C_SOURCE)
elseif(APPLE)
    message(STATUS "🍎 macOS build detected")
    add_definitions(-D_POSIX_C_SOURCE)
    set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -fPIC")
elseif(UNIX AND NOT APPLE)
    message(STATUS "🐧 Linux build detected")
    add_definitions(-D_POSIX_C_SOURCE)
    set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -fPIC")
endif()

# ───────────────────────────────────────────────────────────────────────────
# Build Options
# ───────────────────────────────────────────────────────────────────────────

option(BUILD_SHARED_LIBS "Build shared libraries" OFF)
option(BUILD_EXAMPLES "Build example programs" ON)
option(BUILD_TESTS "Build test suite" ON)
option(ENABLE_DEBUG_LOGGING "Enable debug logging" OFF)

if(ENABLE_DEBUG_LOGGING)
    add_definitions(-DRDB_DEBUG_LOG=1)
    message(STATUS "Debug logging enabled")
endif()

# ───────────────────────────────────────────────────────────────────────────
# Library Targets
# ───────────────────────────────────────────────────────────────────────────

# Core KVDB library
add_library(rocketdb_kvdb
    src/rocketdb_kvdb.c
)
target_include_directories(rocketdb_kvdb PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_compile_options(rocketdb_kvdb PRIVATE
    $<$<CONFIG:Debug>:-O0 -g -Wall -Wextra -Werror>
    $<$<CONFIG:Release>:-O3 -Wall -Wextra>
)

# Core TSDB library
add_library(rocketdb_tsdb
    src/rocketdb_tsdb.c
)
target_include_directories(rocketdb_tsdb PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_compile_options(rocketdb_tsdb PRIVATE
    $<$<CONFIG:Debug>:-O0 -g -Wall -Wextra -Werror>
    $<$<CONFIG:Release>:-O3 -Wall -Wextra>
)

# Combined RocketDB library
add_library(rocketdb
    src/rocketdb_kvdb.c
    src/rocketdb_tsdb.c
)
target_include_directories(rocketdb PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_compile_options(rocketdb PRIVATE
    $<$<CONFIG:Debug>:-O0 -g -Wall -Wextra -Werror>
    $<$<CONFIG:Release>:-O3 -Wall -Wextra>
)

# ───────────────────────────────────────────────────────────────────────────
# Example Programs
# ───────────────────────────────────────────────────────────────────────────

if(BUILD_EXAMPLES)
    message(STATUS "Building examples...")
    
    # KVDB Basic Example
    add_executable(example_kvdb_basic
        examples/example_kvdb_basic_v2.c
    )
    target_link_libraries(example_kvdb_basic PRIVATE rocketdb)
    target_compile_options(example_kvdb_basic PRIVATE
        $<$<CONFIG:Debug>:-O0 -g -Wall -Wextra>
        $<$<CONFIG:Release>:-O2 -Wall -Wextra>
    )
    
    # TSDB Sensor Example (if available)
    if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/examples/example_tsdb_sensor.c")
        add_executable(example_tsdb_sensor
            examples/example_tsdb_sensor.c
        )
        target_link_libraries(example_tsdb_sensor PRIVATE rocketdb)
        target_compile_options(example_tsdb_sensor PRIVATE
            $<$<CONFIG:Debug>:-O0 -g -Wall -Wextra>
            $<$<CONFIG:Release>:-O2 -Wall -Wextra>
        )
    endif()
    
endif()

# ───────────────────────────────────────────────────────────────────────────
# Testing
# ───────────────────────────────────────────────────────────────────────────

if(BUILD_TESTS)
    enable_testing()
    message(STATUS "Testing enabled")
    
    # Create a simple ctest that runs examples if they exist
    if(BUILD_EXAMPLES AND TARGET example_kvdb_basic)
        add_test(
            NAME KVDB_Basic_Example
            COMMAND example_kvdb_basic
        )
    endif()
endif()

# ───────────────────────────────────────────────────────────────────────────
# Installation
# ───────────────────────────────────────────────────────────────────────────

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

# ───────────────────────────────────────────────────────────────────────────
# Summary
# ───────────────────────────────────────────────────────────────────────────

message(STATUS "")
message(STATUS "=== Build Configuration Summary ===")
message(STATUS "Build type: ${CMAKE_BUILD_TYPE}")
message(STATUS "Shared libs: ${BUILD_SHARED_LIBS}")
message(STATUS "Examples: ${BUILD_EXAMPLES}")
message(STATUS "Tests: ${BUILD_TESTS}")
message(STATUS "Debug logging: ${ENABLE_DEBUG_LOGGING}")
message(STATUS "========================================")
message(STATUS "")
message(STATUS "To build, run:")
message(STATUS "  mkdir build && cd build")
message(STATUS "  cmake ..")
message(STATUS "  cmake --build . --config Release")
message(STATUS "")
