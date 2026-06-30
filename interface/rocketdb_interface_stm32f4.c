/**
 * @file      rocketdb_interface_stm32f4.c
 * @brief     RocketDB HAL port for STM32F4 + W25Q128 via spi_flash module
 * @version   1.6.0
 * @author    XiaSheng
 * @date      2026-06-19
 *
 * Self-contained compilation unit: all spi_flash interaction lives here.
 * Other compilation units reference only rocketdb.h (via rocketdb_interface.h).
 *
 * Architecture:
 *   main.c → spi_flash_port_init() → spi_flash_create/init()
 *          → rocketdb_interface_init(dev)   // stores dev for get_ctx()
 *          → test file: part.flash_ctx = rocketdb_interface_get_ctx()
 *          → test file calls rdb_kvdb_init/format/set/get/delete
 *                                   ↓
 *                          rdb_flash_ops_t (this file)
 *                                   ↓
 *                   ops cast ctx→spi_flash_device_t*, call spi_flash_*()
 *
 * CRITICAL: The ops functions NEVER dereference spi_flash_device_t.
 * The pointer is always passed to spi_flash.c (the authoritative unit).
 * This eliminates ODR struct-layout mismatch across compilation units.
 */

#include "rocketdb_interface.h"
#include "spi_flash.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

/* ═══════════════════════════════════════════════════════════════════════════
 *  Opaque device pointer — stored as void* to avoid any struct access
 *  in this compilation unit.  The actual struct is only accessed inside
 *  spi_flash.c, which allocates it and owns its layout.
 * ═══════════════════════════════════════════════════════════════════════════ */
static void* g_flash_ctx = NULL;

/* ═══════════════════════════════════════════════════════════════════════════
 *  Helper: resolve the effective flash device pointer.
 *
 *  Prefers the per-call `ctx` (set via part.flash_ctx) and falls back
 *  to the globally-registered pointer.  This allows a single ops table
 *  to serve multiple partitions if needed.
 * ═══════════════════════════════════════════════════════════════════════════ */
static inline spi_flash_device_t* resolve_dev(void* ctx) {
    void* p = ctx ? ctx : g_flash_ctx;
    return (spi_flash_device_t*)p;
}

/* ═══════════════════════════════════════════════════════════════════════════
 *  Flash operation callbacks
 *
 *  These match rdb_flash_ops_t signatures.  The ctx parameter flows
 *  from rdb_partition_t.flash_ctx through RocketDB core into these
 *  callbacks untouched.
 *
 *  We cast ctx to spi_flash_device_t* and immediately pass it to the
 *  spi_flash_*() API.  This TU never reads or writes any field of the
 *  struct — spi_flash.c is the sole owner of its layout.
 * ═══════════════════════════════════════════════════════════════════════════ */

static int fl_read(void* ctx, uint32_t addr, uint8_t* buf, size_t len) {
    spi_flash_device_t* dev = resolve_dev(ctx);
    if (dev == NULL) {
        return -1;
    }
#ifdef RDB_DEBUG_LOG
    {
        static int trace_cnt = 0;
        if (trace_cnt < 3) {
            printf("[RDB OPS] fl_read ctx=%p dev=%p addr=0x%lX len=%u\r\n", ctx, (void*)dev, (unsigned long)addr,
                (unsigned)len);
            if (trace_cnt == 0) {
                const spi_flash_info_t* inf = spi_flash_get_info(dev);
                printf("[RDB OPS]   -> capacity=%lu sector=%lu\r\n", (unsigned long)(inf ? inf->capacity : 0),
                    (unsigned long)(inf ? inf->sector_size : 0));
            }
            trace_cnt++;
        }
    }
#endif
    return (int)spi_flash_read(dev, addr, buf, len);
}

static int fl_write(void* ctx, uint32_t addr, const uint8_t* buf, size_t len) {
    spi_flash_device_t* dev = resolve_dev(ctx);
    if (dev == NULL) {
        return -1;
    }
    return (int)spi_flash_write(dev, addr, buf, len);
}

static int fl_erase(void* ctx, uint32_t addr) {
    spi_flash_device_t* dev = resolve_dev(ctx);
    if (dev == NULL) {
        return -1;
    }
#ifdef RDB_DEBUG_LOG
    printf("[RDB OPS] fl_erase ctx=%p dev=%p addr=0x%lX\r\n", ctx, (void*)dev, (unsigned long)addr);
    {
        const spi_flash_info_t* inf = spi_flash_get_info(dev);
        printf("[RDB OPS]   -> capacity=%lu\r\n", (unsigned long)(inf ? inf->capacity : 0));
    }
#endif
    return (int)spi_flash_erase_sector_by_addr(dev, addr, NULL);
}

static void fl_lock(void* ctx) {
    /* spi_flash.c uses SPI_LOCK/SPI_UNLOCK internally around every
       flash operation.  Locking at this level would be redundant
       and risks deadlock. */
    (void)ctx;
}

static void fl_unlock(void* ctx) {
    (void)ctx;
}

static void fl_yield(void* ctx) {
    (void)ctx;
    /* Feed watchdog / yield to RTOS if needed in the future. */
}

/* ═══════════════════════════════════════════════════════════════════════════
 *  RocketDB HAL ops table
 * ═══════════════════════════════════════════════════════════════════════════ */
const rdb_flash_ops_t rocketdb_interface_ops = {
    .read   = fl_read,
    .write  = fl_write,
    .erase  = fl_erase,
    .lock   = fl_lock,
    .unlock = fl_unlock,
    .yield  = fl_yield,
};

/* ═══════════════════════════════════════════════════════════════════════════
 *  CRC-16 / MODBUS (polynomial 0xA001 reflected, seed 0xFFFF)
 *
 *  These are the external symbols RocketDB core links against.
 *  Defined directly here (no intermediate wrapper) to save ROM.
 * ═══════════════════════════════════════════════════════════════════════════ */

uint16_t rdb_crc16(const void* data, size_t len) {
    return rdb_crc16_cont(0xFFFFu, data, len);
}

uint16_t rdb_crc16_cont(uint16_t crc, const void* data, size_t len) {
    const uint8_t* p = (const uint8_t*)data;
    size_t         i;
    uint8_t        bit;

    if (p == NULL && len != 0u) {
        return crc;
    }

    for (i = 0u; i < len; ++i) {
        crc ^= p[i];
        for (bit = 0u; bit < 8u; ++bit) {
            if ((crc & 1u) != 0u) {
                crc = (uint16_t)((crc >> 1) ^ 0xA001u);
            } else {
                crc >>= 1;
            }
        }
    }
    return crc;
}

/* ═══════════════════════════════════════════════════════════════════════════
 *  Hash — FNV-1a folded to 16 bits
 * ═══════════════════════════════════════════════════════════════════════════ */

uint16_t rdb_hash16(const void* data, size_t len) {
    const uint8_t* p = (const uint8_t*)data;
    uint32_t       h = 2166136261u;
    size_t         i;

    if (p == NULL && len != 0u) {
        return 0u;
    }

    for (i = 0u; i < len; ++i) {
        h ^= p[i];
        h *= 16777619u;
    }
    return (uint16_t)(h ^ (h >> 16));
}

/* ═══════════════════════════════════════════════════════════════════════════
 *  Debug print — maps to UART printf via retarget
 * ═══════════════════════════════════════════════════════════════════════════ */

void rocketdb_interface_debug_print(const char* const fmt, ...) {
    va_list args;
    va_start(args, fmt);
    vprintf(fmt, args);
    va_end(args);
}

/* ═══════════════════════════════════════════════════════════════════════════
 *  Public API
 * ═══════════════════════════════════════════════════════════════════════════ */

/**
 * @brief Register a pre-initialised flash device.
 *
 * Must be called once from main.c after spi_flash_create() +
 * spi_flash_init().  Stores the device pointer so that
 * rocketdb_interface_get_ctx() can return it to test files.
 */
void rocketdb_interface_init(spi_flash_device_t* dev) {
    g_flash_ctx = dev;

    if (dev != NULL) {
        const spi_flash_info_t* info = spi_flash_get_info(dev);
        if (info != NULL) {
            printf("[RDB] Port init: dev=%s cap=%lu sector=%lu\r\n", info->name, (unsigned long)info->capacity,
                (unsigned long)info->sector_size);
        }
    }
}

/**
 * @brief Return the registered flash device pointer.
 *
 * Callers assign this to rdb_partition_t.flash_ctx so that the ops
 * callbacks receive the correct pointer through RocketDB's internal
 * fl_read/fl_write/fl_erase wrappers.
 */
void* rocketdb_interface_get_ctx(void) {
    return g_flash_ctx;
}

/* ═══════════════════════════════════════════════════════════════════════════
 *  Flash probe helper — used by rocketdb_interface.h declaration
 * ═══════════════════════════════════════════════════════════════════════════ */

spi_flash_device_t* rocketdb_interface_flash_probe(const void* port_ops) {
    spi_flash_device_t* dev;

    dev = spi_flash_create("W25Q128");
    if (dev == NULL) {
        return NULL;
    }

    if (spi_flash_init(dev, (const spi_flash_port_ops_t*)port_ops) != 0) {
        spi_flash_destroy(dev);
        return NULL;
    }

    return dev;
}
