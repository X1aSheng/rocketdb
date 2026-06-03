# RocketDB Zephyr module CMake build
# SPDX-License-Identifier: MIT

zephyr_include_directories(../src)

zephyr_library()

if(CONFIG_ROCKETDB_KVDB)
  zephyr_library_sources(../src/rocketdb_kvdb.c)
endif()

if(CONFIG_ROCKETDB_TSDB)
  zephyr_library_sources(../src/rocketdb_tsdb.c)
endif()

# Zephyr HAL adapter
zephyr_library_sources(rocketdb_port.c)

# Map Kconfig symbols to RocketDB compile-time definitions
zephyr_compile_definitions(
  RDB_MAX_KEY_LEN=${CONFIG_ROCKETDB_MAX_KEY_LEN}
  RDB_MAX_VAL_LEN=${CONFIG_ROCKETDB_MAX_VAL_LEN}
  RDB_MAX_TS_DATA_LEN=${CONFIG_ROCKETDB_MAX_TS_DATA_LEN}
  RDB_STACK_BUF_SIZE=${CONFIG_ROCKETDB_STACK_BUF_SIZE}
  RDB_GC_GARBAGE_PCT=${CONFIG_ROCKETDB_GC_GARBAGE_PCT}
  RDB_GC_WEAR_THRESHOLD=${CONFIG_ROCKETDB_GC_WEAR_THRESHOLD}
  RDB_KV_CACHE_SIZE=${CONFIG_ROCKETDB_KV_CACHE_SIZE}
  RDB_BLOOM_BITS=${CONFIG_ROCKETDB_BLOOM_BITS}
  RDB_FLASH_PAGE_SIZE=${CONFIG_ROCKETDB_FLASH_PAGE_SIZE}
  RDB_GC_W_GARBAGE=${CONFIG_ROCKETDB_GC_W_GARBAGE}
  RDB_GC_W_WEAR=${CONFIG_ROCKETDB_GC_W_WEAR}
  RDB_GC_W_CAPACITY=${CONFIG_ROCKETDB_GC_W_CAPACITY}
)

if(CONFIG_ROCKETDB_SHELL AND EXISTS "${CMAKE_CURRENT_LIST_DIR}/rocketdb_shell.c")
  zephyr_library_sources(rocketdb_shell.c)
endif()
