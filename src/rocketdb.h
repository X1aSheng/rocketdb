/*****************************************************************************
 * rocketdb.h — RocketDB Public Header
 *
 * Zero-allocation dual-mode Flash storage engine for embedded systems.
 *
 * KVDB: Log-structured key-value store with four-phase scored GC,
 *       static wear leveling, and power-loss consistent two-phase writes.
 *
 * TSDB: Ring-buffer time-series store with epoch management,
 *       monotonic timestamps, and automatic sector rotation.
 *
 * Architecture:
 *   ┌──────────────────────────────────────────┐
 *   │            Application Code              │
 *   ├──────────────────────────────────────────┤
 *   │  rdb_kvdb_*()  API  │  rdb_tsdb_*() API  │  ← This header
 *   ├──────────────────────────────────────────┤
 *   │  rocketdb_kvdb.c    │  rocketdb_tsdb.c   │  ← Engine impl
 *   ├──────────────────────────────────────────┤
 *   │     rdb_flash_ops_t (user-provided)      │  ← HAL layer
 *   ├──────────────────────────────────────────┤
 *   │         Physical NOR Flash               │
 *   └──────────────────────────────────────────┘
 *
 * Usage:
 *   Application code only needs to #include "rocketdb.h".
 *   No additional headers are required.
 *
 * External dependencies (user must implement):
 *   - rdb_crc16()      — CRC-16 computation
 *   - rdb_crc16_cont() — CRC-16 continuation (streaming)
 *   - rdb_hash16()     — 16-bit key hash for KVDB fast-reject
 *   - rdb_flash_ops_t  — Flash read/write/erase callbacks
 *
 * Thread safety:
 *   All public API functions acquire the flash lock (if provided)
 *   before accessing flash.  The lock/unlock callbacks in
 *   rdb_flash_ops_t should map to a mutex or critical section.
 *   If NULL, single-threaded operation is assumed.
 *
 * Memory allocation:
 *   RocketDB performs ZERO dynamic memory allocation.  All RAM
 *   buffers are caller-provided:
 *     - KVDB: rdb_kv_sector_meta_t[] via rdb_kvdb_meta_size()
 *     - TSDB: uint32_t[]             via rdb_tsdb_ec_size()
 *
 * Copyright (c) 2015 XiaSheng(info@zhis.net)
 * SPDX-License-Identifier: MIT
 * @date    2015-05-04
 * @version 1.1.0
 * 
 *****************************************************************************/
#ifndef ROCKETDB_H
#define ROCKETDB_H

#include <stddef.h>
#include <stdint.h>

/* Portable compile-time assertion helper. */
#ifndef RDB_STATIC_ASSERT
#if defined(__STDC_VERSION__) && (__STDC_VERSION__ >= 201112L)
#define RDB_STATIC_ASSERT(cond, msg) _Static_assert(cond, msg)
#else
#define RDB_STATIC_ASSERT_JOIN_(a, b) a##b
#define RDB_STATIC_ASSERT_JOIN(a, b) RDB_STATIC_ASSERT_JOIN_(a, b)
#define RDB_STATIC_ASSERT(cond, msg) \
    typedef char RDB_STATIC_ASSERT_JOIN(rdb_static_assert_, __LINE__)[(cond) ? 1 : -1]
#endif
#endif


#ifdef __cplusplus
extern "C" {
#endif

/* ═══════════════════════════════════════════════════════════════════════════
 *  §1  Compile-time configuration
 *
 *  All parameters have sensible defaults.  Override by defining the
 *  macro BEFORE including this header (e.g. in a project-wide config
 *  header or compiler -D flag).
 *
 *  ┌─────────────────────────┬─────────┬──────────────────────────────────┐
 *  │ Macro                   │ Default │ Description                      │
 *  ├─────────────────────────┼─────────┼──────────────────────────────────┤
 *  │ RDB_MAX_KEY_LEN         │   32    │ Max key length (1..32 bytes)     │
 *  │ RDB_MAX_VAL_LEN         │  4095   │ Max value length per record      │
 *  │ RDB_MAX_TS_DATA_LEN     │    0    │ Max TSDB data len (0=physical)   │
 *  │ RDB_STACK_BUF_SIZE      │   64    │ Stack buffer for merged writes   │
 *  │ RDB_GC_GARBAGE_PCT      │   20    │ GC Phase 2 garbage% threshold    │
 *  │ RDB_GC_WEAR_THRESHOLD   │   100   │ GC Phase 4 wear-level trigger    │
 *  │ RDB_MIN_SECTOR_SIZE     │  4096   │ Minimum sector size (bytes)      │
 *  │ RDB_KV_MIN_SECTORS      │    3    │ Minimum sectors for KVDB         │
 *  │ RDB_TS_MIN_SECTORS      │    2    │ Minimum sectors for TSDB         │
 *  │ RDB_MAX_SECTORS         │  255    │ Maximum sectors per partition    │
 *  └─────────────────────────┴─────────┴──────────────────────────────────┘
 * ═══════════════════════════════════════════════════════════════════════════ */

/** @brief Maximum key length in bytes.  Valid range: 1..32. */
#ifndef RDB_MAX_KEY_LEN
#define RDB_MAX_KEY_LEN 32u
#endif

/** @brief Maximum value length in bytes per KV record. */
#ifndef RDB_MAX_VAL_LEN
#define RDB_MAX_VAL_LEN 4095u
#endif

/**
 * @brief Maximum data payload length for TSDB records.
 *
 * Set to 0 to use the physical limit (sector_size − headers).
 * Set to a positive value to cap record size regardless of sector size.
 */
#ifndef RDB_MAX_TS_DATA_LEN
#define RDB_MAX_TS_DATA_LEN 0u
#endif

/**
 * @brief Stack buffer size for merged single-write operations.
 *
 * Records smaller than this are assembled in a stack buffer and
 * written in one flash operation, reducing write latency and wear.
 * Must be ≥ max(KV_REC_SZ, TS_REC_SZ) = 16 bytes.
 */
#ifndef RDB_STACK_BUF_SIZE
#define RDB_STACK_BUF_SIZE 64u
#endif

/**
 * @brief Flash physical page size in bytes, or 0 for no page-boundary
 *        restriction.
 *
 * When > 0, the engine ensures that no single write operation crosses
 * a page_size boundary in the flash address space.  This allows HAL
 * drivers (e.g. W25QXX) to use the maximum Page Program length without
 * needing to split at page boundaries themselves.
 *
 * W25QXX-series parts use 256-byte pages:
 *   #define RDB_FLASH_PAGE_SIZE 256u
 *
 * Must be a power of 2 when > 0.
 */
#ifndef RDB_FLASH_PAGE_SIZE
#define RDB_FLASH_PAGE_SIZE 0u
#endif

/**
 * @brief Clamp `*io_chunk` to stay within the current flash page.
 *
 * Must be called inside the write loop with `wpos` being the current
 * write address.  No-op when RDB_FLASH_PAGE_SIZE == 0.
 */
#if RDB_FLASH_PAGE_SIZE > 0
#define RDB_PAGE_CLAMP(io_chunk, wpos) \
    do { \
        uint32_t _po = (uint32_t)((wpos) & ((uint32_t)RDB_FLASH_PAGE_SIZE - 1u)); \
        if (_po > 0u) { \
            uint32_t _pr = (uint32_t)RDB_FLASH_PAGE_SIZE - _po; \
            if ((io_chunk) > _pr) (io_chunk) = _pr; \
        } \
    } while (0)
#else
#define RDB_PAGE_CLAMP(io_chunk, wpos) ((void)0)
#endif

/**
 * @brief KVDB key-to-address cache size (number of slots).
 *
 * A direct-mapped cache with linear probing that stores key fingerprints
 * mapped to absolute flash addresses.  Eliminates full-table scans on
 * repeated get()/set() calls for frequently-accessed keys.
 *
 * Set to 0 to disable the cache entirely (zero RAM cost).
 * Recommended: 64 slots (1,024 bytes) for a typical embedded workload.
 */
#ifndef RDB_KV_CACHE_SIZE
#define RDB_KV_CACHE_SIZE 0u
#endif

/**
 * @brief Per-sector bloom filter width in bits.
 *
 * A 256-bit (32-byte) hash bucket bitmap that tracks which keys *may*
 * exist in each sector.  Before scanning a sector for a key the engine
 * checks the bloom filter and can skip the entire sector when the key
 * is definitively absent, avoiding costly flash reads.
 *
 * False-positive rate with 256 bits, 2 hash functions, and 80 keys per
 * sector is approximately 27%.  Set to 0 to disable.
 *
 * Must be a power of 2 when > 0.
 */
#ifndef RDB_BLOOM_BITS
#define RDB_BLOOM_BITS 0u  /* 0 = disabled; W25QXX-class workloads: 256 */
#endif
#if RDB_BLOOM_BITS > 0
#define RDB_BLOOM_BYTES (RDB_BLOOM_BITS / 8u)
/** @brief Set two hash bits in a per-sector bloom bitmap for key hash @p h. */
#define RDB_BLOOM_SET(bm, h) \
    do { \
        (bm)[(h) & 0x1Fu] |= (uint8_t)(1u << (((h) >> 5) & 7u)); \
        (bm)[((h) >> 8) & 0x1Fu] |= (uint8_t)(1u << ((((h) >> 8) >> 5) & 7u)); \
    } while (0)
/** @brief Test whether key hash @p h *may* exist in the sector.  0 = absent. */
#define RDB_BLOOM_MAYBE(bm, h) \
    ((int)(((bm)[(h) & 0x1Fu] & (uint8_t)(1u << (((h) >> 5) & 7u))) != 0u && \
           ((bm)[((h) >> 8) & 0x1Fu] & (uint8_t)(1u << ((((h) >> 8) >> 5) & 7u))) != 0u))
#else
#define RDB_BLOOM_BYTES 0u
#define RDB_BLOOM_SET(bm, h)       ((void)0)
#define RDB_BLOOM_MAYBE(bm, h)     1  /* Always "may exist" when disabled */
#endif

/** @brief Key fingerprint slot for the KV cache.
 *
 * Each slot stores a key fingerprint (hash + length + 8-byte prefix) and
 * the absolute flash address of the VALID record.  The fingerprint format
 * is identical to the one used by rdb_dedup_set_t for collision detection. */
typedef struct {
    uint16_t hash;       /**< Full 16-bit key hash (FNV-1a folded)     */
    uint8_t  klen;       /**< Key length in bytes (0 = empty slot)     */
    uint8_t  prefix[8];  /**< First 8 key bytes for disambiguation     */
    uint32_t addr;       /**< Absolute flash address of VALID record   */
} rdb_kv_cache_slot_t;

/** @brief KVDB key-to-address cache (embedded in rdb_kvdb_t).
 *
 * The total RAM cost is RDB_KV_CACHE_SIZE * 16 bytes.  With the
 * default of 0, the cache is a zero-element flexible array and costs
 * nothing.  Use at least 64 slots for production workloads. */
typedef struct {
#if RDB_KV_CACHE_SIZE > 0
    rdb_kv_cache_slot_t slots[RDB_KV_CACHE_SIZE];
#else
    uint8_t disabled;
#endif
} rdb_kv_cache_t;

/**
 * @brief GC Phase 2 scored selection garbage percentage threshold.
 *
 * Sectors with garbage% below this threshold are not considered as
 * GC candidates in the normal (non-emergency) scoring pass.
 * Lower values make GC more aggressive; higher values defer GC longer.
 */
#ifndef RDB_GC_GARBAGE_PCT
#define RDB_GC_GARBAGE_PCT 20u
#endif

/**
 * @brief GC Phase 4 static wear-leveling trigger threshold.
 *
 * When the difference between the maximum and minimum erase counts
 * across all sectors exceeds this value, Phase 4 wear leveling
 * activates to redistribute wear by recycling the least-worn sector.
 */
#ifndef RDB_GC_WEAR_THRESHOLD
#define RDB_GC_WEAR_THRESHOLD 100u
#endif

/**
 * @brief GC Phase 2 scored-selection weight for the garbage percentage
 *        term in the composite score formula:
 *          score = gpct × W_GARBAGE + wpct × W_WEAR + cpct × W_CAPACITY
 *
 * Higher values favour reclaiming sectors with the most garbage (throughput).
 * Lower values relative to W_WEAR favour even wear distribution (lifespan).
 */
#ifndef RDB_GC_W_GARBAGE
#define RDB_GC_W_GARBAGE 7u
#endif

/**
 * @brief GC Phase 2 weight for the wear percentage term.
 *
 * Higher values favour recycling sectors with lower erase counts,
 * distributing wear more evenly across the flash array.
 */
#ifndef RDB_GC_W_WEAR
#define RDB_GC_W_WEAR 3u
#endif

/**
 * @brief GC Phase 2 weight for the capacity utilisation term.
 *
 * Higher values favour sectors with mostly dead space (high reclaim
 * efficiency per byte migrated).
 */
#ifndef RDB_GC_W_CAPACITY
#define RDB_GC_W_CAPACITY 1u
#endif

/** @brief Minimum supported sector (erase block) size in bytes.
 *         Must be a power of 2. */
#ifndef RDB_MIN_SECTOR_SIZE
#define RDB_MIN_SECTOR_SIZE 4096u
#endif

/** @brief Minimum number of sectors required for a KVDB partition.
 *         At least 3 sectors are needed: 1 active + 1 GC reserve + 1 data. */
#ifndef RDB_KV_MIN_SECTORS
#define RDB_KV_MIN_SECTORS 3u
#endif

/** @brief Minimum number of sectors required for a TSDB partition.
 *         At least 2 sectors for meaningful ring-buffer operation. */
#ifndef RDB_TS_MIN_SECTORS
#define RDB_TS_MIN_SECTORS 2u
#endif

/** @brief Maximum number of sectors per partition.
 *         Limited to 255 because sector indices are stored as uint8_t. */
#ifndef RDB_MAX_SECTORS
#define RDB_MAX_SECTORS 255u
#endif

/* Compile-time validation */
RDB_STATIC_ASSERT(RDB_MAX_KEY_LEN >= 1u && RDB_MAX_KEY_LEN <= 32u,
    "RDB_MAX_KEY_LEN must be 1..32");
RDB_STATIC_ASSERT(RDB_STACK_BUF_SIZE >= 32u,
    "RDB_STACK_BUF_SIZE must be >= 32 (record header + minimum payload)");

/* ═══════════════════════════════════════════════════════════════════════════
 *  §2  Error codes
 *
 *  All public API functions return rdb_err_t.
 *  RDB_OK (0) indicates success; negative values indicate errors.
 *
 *  ┌──────────────────────────┬──────┬────────────────────────────────────┐
 *  │ Code                     │ Value│ Meaning                            │
 *  ├──────────────────────────┼──────┼────────────────────────────────────┤
 *  │ RDB_OK                   │   0  │ Success                            │
 *  │ RDB_ERR_PARAM            │  -1  │ Invalid parameter                  │
 *  │ RDB_ERR_FLASH            │  -2  │ Flash I/O error                    │
 *  │ RDB_ERR_NO_SPACE         │  -3  │ No space (deprecated, use FULL)    │
 *  │ RDB_ERR_NOT_FOUND        │  -4  │ Key/record not found               │
 *  │ RDB_ERR_TOO_LARGE        │  -5  │ Key/value/data exceeds limit       │
 *  │ RDB_ERR_CRC              │  -6  │ Data CRC verification failed       │
 *  │ RDB_ERR_CORRUPT          │  -7  │ Unrecoverable data corruption      │
 *  │ RDB_ERR_NOT_INIT         │  -8  │ Database not initialised           │
 *  │ RDB_ERR_GC_FAIL          │  -9  │ GC failed to reclaim space         │
 *  │ RDB_ERR_ITER_END         │ -10  │ Iterator exhausted                 │
 *  │ RDB_ERR_FULL             │ -11  │ Logical capacity exhausted         │
 *  │ RDB_ERR_BUSY             │ -12  │ Concurrent modification detected   │
 *  │ RDB_ERR_TIME_EXHAUSTED   │ -13  │ Timestamp space exhausted          │
 *  └──────────────────────────┴──────┴────────────────────────────────────┘
 * ═══════════════════════════════════════════════════════════════════════════ */

typedef enum {
    RDB_OK = 0,                  /**< Success                            */
    RDB_ERR_PARAM = -1,          /**< Invalid parameter                  */
    RDB_ERR_FLASH = -2,          /**< Flash read/write/erase failed      */
    RDB_ERR_NO_SPACE = -3,       /**< No space available (legacy)        */
    RDB_ERR_NOT_FOUND = -4,      /**< Key or record not found            */
    RDB_ERR_TOO_LARGE = -5,      /**< Data exceeds configured limit      */
    RDB_ERR_CRC = -6,            /**< CRC verification failed            */
    RDB_ERR_CORRUPT = -7,        /**< Unrecoverable flash corruption     */
    RDB_ERR_NOT_INIT = -8,       /**< Database not initialised           */
    RDB_ERR_GC_FAIL = -9,        /**< Garbage collection failed          */
    RDB_ERR_ITER_END = -10,      /**< No more records to iterate         */
    RDB_ERR_FULL = -11,          /**< Logical capacity fully used        */
    RDB_ERR_BUSY = -12,          /**< DB modified during iteration       */
    RDB_ERR_TIME_EXHAUSTED = -13 /**< Timestamp range exhausted          */
} rdb_err_t;

/* ═══════════════════════════════════════════════════════════════════════════
 *  §3  Record state machine (NOR-safe 1→0 transitions)
 *
 *  NOR flash can only clear bits (1→0) without erasing.  The record
 *  state field uses this property for atomic state transitions:
 *
 *    WRITING (0xFF)  →  VALID (0xFE)  →  DEAD (0xFC)
 *       │                                    ↑
 *       └────────────────────────────────────┘
 *                   (direct on error)
 *
 *  Bit pattern analysis:
 *    0xFF = 1111_1111  (erased, write in progress)
 *    0xFE = 1111_1110  (bit 0 cleared → committed)
 *    0xFC = 1111_1100  (bit 1 cleared → invalidated)
 *
 *  Each transition clears exactly one bit, which is always safe on
 *  NOR flash without an erase cycle.
 * ═══════════════════════════════════════════════════════════════════════════ */

#define RDB_STATE_WRITING   0xFFu /**< Write in progress (uncommitted)   */
#define RDB_STATE_VALID     0xFEu /**< Committed and active              */
#define RDB_STATE_DEAD      0xFCu /**< Invalidated (reclaimable by GC)   */

/* ═══════════════════════════════════════════════════════════════════════════
 *  §4  Sentinel values
 *
 *  Special marker values used throughout the codebase to represent
 *  invalid, uninitialised, or boundary conditions.
 * ═══════════════════════════════════════════════════════════════════════════ */

#define RDB_ADDR_INVALID    0xFFFFFFFFu /**< Invalid flash address        */
#define RDB_TIME_INVALID    0xFFFFFFFFu /**< Invalid/uninitialised time    */
#define RDB_TIME_MAX        0xFFFFFFFEu /**< Maximum representable time    */
#define RDB_SEQ_INVALID     0xFFFFFFFFu /**< Invalid sequence number (≠wrap-to-0) */

/* ═══════════════════════════════════════════════════════════════════════════
 *  §5  Utility macros
 * ═══════════════════════════════════════════════════════════════════════════ */

/**
 * @brief Round `v` up to the nearest multiple of alignment `a`.
 * @note  `a` MUST be a power of 2.
 */
#define RDB_ALIGN_UP(v, a)  (((uint32_t)(v) + (uint32_t)(a) - 1u) & ~((uint32_t)(a) - 1u))

/** @brief Return the smaller of two values. */
#define RDB_MIN(a, b)       ((a) < (b) ? (a) : (b))

/** @brief Return the larger of two values. */
#define RDB_MAX(a, b)       ((a) > (b) ? (a) : (b))

/**
 * @brief Compute the GC reserve sector count for a given total.
 *
 * For partitions with ≥16 sectors, reserve 2 sectors for GC headroom.
 * For smaller partitions, reserve 1 sector.
 */
#define RDB_GC_RESERVE(n)   ((uint8_t)(((n) >= 16u) ? 2u : 1u))

/**
 * @brief Wrap-safe 32-bit sequence number comparison.
 *
 * Returns true if sequence `a` is logically greater than `b`,
 * handling uint32_t wrap-around via signed difference.
 */
#define RDB_SEQ_GT(a, b)    ((int32_t)((a) - (b)) > 0)

/**
 * @brief Wrap-safe 16-bit sequence number comparison.
 *
 * Used by TSDB for sector sequence ordering.
 */
#define RDB_SEQ16_GT(a, b)  ((int16_t)((uint16_t)(a) - (uint16_t)(b)) > 0)

/** @brief Boolean true for RocketDB return values. */
#define RDB_TRUE            1

/** @brief Boolean false for RocketDB return values. */
#define RDB_FALSE           0

/** @brief Iterator callback return: continue scanning. */
#define RDB_ITER_CONTINUE   0

/** @brief Iterator callback return: stop scanning. */
#define RDB_ITER_STOP       1

/* ═══════════════════════════════════════════════════════════════════════════
 *  §6  Magic numbers and version
 *
 *  Each database type uses a unique 32-bit magic number in sector
 *  headers for identification and corruption detection.  Record
 *  headers use 8-bit magic values for space efficiency.
 *
 *  ┌──────────────────────┬────────────┬──────────────┐
 *  │ Constant             │ Value      │ ASCII        │
 *  ├──────────────────────┼────────────┼──────────────┤
 *  │ RDB_KV_SECTOR_MAGIC  │ 0x4B564442 │ "KVDB"       │
 *  │ RDB_TS_SECTOR_MAGIC  │ 0x54534442 │ "TSDB"       │
 *  │ RDB_KV_RECORD_MAGIC  │ 0xA5       │ —           │
 *  │ RDB_TS_RECORD_MAGIC  │ 0xB6       │ —           │
 *  └──────────────────────┴────────────┴──────────────┘
 * ═══════════════════════════════════════════════════════════════════════════ */

#define RDB_KV_SECTOR_MAGIC 0x4B564442u /**< KVDB sector header magic     */
#define RDB_KV_RECORD_MAGIC 0xA5u       /**< KVDB record header magic     */
#define RDB_KV_VERSION      0x0001u     /**< KVDB on-flash format version */

#define RDB_TS_SECTOR_MAGIC 0x54534442u /**< TSDB sector header magic     */
#define RDB_TS_RECORD_MAGIC 0xB6u       /**< TSDB record header magic     */

/* ═══════════════════════════════════════════════════════════════════════════
 *  §7  Flash abstraction layer (HAL)
 *
 *  The user must provide an rdb_flash_ops_t function table implementing
 *  read, write, and erase operations for the target flash device.
 *
 *  Required callbacks:
 *    read  — Read `len` bytes from absolute address `addr` into `buf`.
 *            Return 0 on success, non-zero on failure.
 *    write — Write `len` bytes from `buf` to absolute address `addr`.
 *            Return 0 on success, non-zero on failure.
 *            Must respect write granularity (no sub-granularity writes).
 *    erase — Erase the sector containing absolute address `addr`.
 *            Return 0 on success, non-zero on failure.
 *
 *  Optional callbacks (set to NULL if not needed):
 *    lock   — Acquire mutual exclusion (mutex / critical section).
 *    unlock — Release mutual exclusion.
 *    yield  — Yield CPU during long operations (GC, erase).
 *             Useful for cooperative RTOS or watchdog feeding.
 * ═══════════════════════════════════════════════════════════════════════════ */

/**
 * @brief Flash operation function table.
 *
 * Every callback receives a @c void* ctx as its first argument.
 * The context pointer is stored in @c rdb_partition_t.flash_ctx
 * and enables a single ops table to be shared across multiple
 * flash devices or partitions.  Set flash_ctx to NULL for
 * bare-metal single-instance use (backward compatible).
 */
typedef struct {
    int  (*read)  (void* ctx, uint32_t addr, uint8_t* buf, size_t len); /**< Read    */
    int  (*write) (void* ctx, uint32_t addr, const uint8_t* buf, size_t len); /**< Write */
    int  (*erase) (void* ctx, uint32_t addr);                           /**< Erase   */
    void (*lock)  (void* ctx);          /**< Acquire flash lock (NULL = no-op)         */
    void (*unlock)(void* ctx);          /**< Release flash lock (NULL = no-op)         */
    void (*yield) (void* ctx);          /**< Yield CPU (NULL = no-op)                  */
} rdb_flash_ops_t;

/**
 * @brief Flash partition descriptor.
 *
 * Describes a contiguous region of flash assigned to one database
 * instance.  Multiple KVDB and/or TSDB instances can coexist on the
 * same flash chip by using non-overlapping partitions.
 *
 * @note  The partition must be aligned to sector boundaries.
 *        total_size must be an exact multiple of sector_size.
 *        sector_size must be a power of 2 and ≥ RDB_MIN_SECTOR_SIZE.
 */
typedef struct {
    const char*           name;      /**< Human-readable name (debug)   */
    uint32_t              base_addr; /**< Absolute flash start address  */
    uint32_t              total_size;/**< Total partition size (bytes)  */
    uint32_t              sector_size;/**< Erase block size (bytes)     */
    uint8_t               write_gran;/**< Write granularity exponent:
                                          0→1B, 1→2B, 2→4B, 3→8B      */
    const rdb_flash_ops_t* ops;      /**< Flash operation callbacks     */
    void*                 flash_ctx; /**< Opaque pointer passed to every
                                          ops callback (NULL = no ctx)  */
} rdb_partition_t;

/* ═══════════════════════════════════════════════════════════════════════════
 *  §8  On-flash data structures
 *
 *  All structures are packed to 1-byte alignment for deterministic
 *  flash layout.  uint32_t fields are placed at naturally-aligned
 *  offsets (multiples of 4) for efficient access on ARM Cortex-M.
 *
 *  Padding bytes are initialised to 0xFF (erased state) and reserved
 *  for future use.
 *
 *  ┌───────────────────────────────────────────────────────────────┐
 *  │  KVDB Sector Layout                                           │
 *  │                                                               │
 *  │  ┌─────────────────────────┐ offset 0                         │
 *  │  │ rdb_kv_sector_hdr_t     │ 16 bytes                         │
 *  │  ├─────────────────────────┤ data_start (aligned to gran)     │
 *  │  │ Record 0                │ rdb_kv_record_hdr_t + key + val  │
 *  │  │ Record 1                │                                  │
 *  │  │ ...                     │                                  │
 *  │  │ (erased space = 0xFF)   │ ← write frontier                │
 *  │  └─────────────────────────┘ sector_size                      │
 *  │                                                               │
 *  │  KVDB Record Layout                                           │
 *  │  ┌──────────┬──────────────┬──────────────┐                   │
 *  │  │ hdr 16B  │ key (padded) │ val (padded) │                   │
 *  │  └──────────┴──────────────┴──────────────┘                   │
 *  └───────────────────────────────────────────────────────────────┘
 *
 *  ┌───────────────────────────────────────────────────────────────┐
 *  │  TSDB Sector Layout                                           │
 *  │                                                               │
 *  │  ┌─────────────────────────┐ offset 0                         │
 *  │  │ rdb_ts_sector_hdr_t     │ 20 bytes                         │
 *  │  ├─────────────────────────┤ data_start (aligned to gran)     │
 *  │  │ Record 0                │ rdb_ts_record_hdr_t + data       │
 *  │  │ Record 1                │                                  │
 *  │  │ ...                     │                                  │
 *  │  │ (erased space = 0xFF)   │ ← write frontier                │
 *  │  └─────────────────────────┘ sector_size                      │
 *  │                                                               │
 *  │  TSDB Record Layout                                           │
 *  │  ┌──────────┬───────────────┐                                 │
 *  │  │ hdr 12B  │ data (padded) │                                 │
 *  │  └──────────┴───────────────┘                                 │
 *  └───────────────────────────────────────────────────────────────┘
 * ═══════════════════════════════════════════════════════════════════════════ */

#pragma pack(push, 1)

/**
 * @brief KVDB sector header — 16 bytes.
 *
 * Written once when a sector is initialised (after erase).
 * The hdr_crc covers the first 6 bytes (magic + version) for
 * fast corruption detection during boot scan.
 *
 *  Offset  Field        Size  Description
 *  ──────  ───────────  ────  ─────────────────────────────────
 *    0     magic        4     RDB_KV_SECTOR_MAGIC (0x4B564442)
 *    4     version      2     On-flash format version
 *    6     hdr_crc      2     CRC16 of bytes [0..5]
 *    8     erase_cnt    4     Cumulative erase count
 *   12     create_seq   4     Monotonic sector creation sequence
 */
typedef struct {
    uint32_t magic;      /**< @brief Sector identification magic        */
    uint16_t version;    /**< @brief On-flash format version            */
    uint16_t hdr_crc;    /**< @brief CRC16(bytes[0..5])                 */
    uint32_t erase_cnt;  /**< @brief Cumulative erase cycle count       */
    uint32_t create_seq; /**< @brief Monotonic creation sequence number */
} rdb_kv_sector_hdr_t;

/**
 * @brief KVDB record header — 16 bytes.
 *
 * Precedes every key-value record on flash.  The state field enables
 * atomic two-phase commit via NOR-safe 1→0 bit transitions.
 *
 *  Offset  Field      Size  Description
 *  ──────  ─────────  ────  ──────────────────────────────────
 *    0     magic      1     RDB_KV_RECORD_MAGIC (0xA5)
 *    1     state      1     Record state (WRITING/VALID/DEAD)
 *    2     key_len    1     Key length in bytes (unpadded)
 *    3     _pad0      1     Reserved (0xFF)
 *    4     val_len    2     Value length in bytes (unpadded)
 *    6     key_hash   2     16-bit hash for fast key rejection
 *    8     seq        4     Monotonic write sequence number
 *   12     data_crc   2     CRC16(key_bytes ‖ val_bytes)
 *   14     _pad1      2     Reserved (0xFFFF)
 */
typedef struct {
    uint8_t  magic;    /**< @brief Record identification magic        */
    uint8_t  state;    /**< @brief Record state machine value         */
    uint8_t  key_len;  /**< @brief Key length in bytes (1..32)        */
    uint8_t  _pad0;    /**< @brief Reserved, must be 0xFF             */
    uint16_t val_len;  /**< @brief Value length in bytes              */
    uint16_t key_hash; /**< @brief 16-bit hash for fast key matching  */
    uint32_t seq;      /**< @brief Monotonic write sequence number    */
    uint16_t data_crc; /**< @brief CRC16 of raw key + value bytes    */
    uint16_t _pad1;    /**< @brief Reserved, must be 0xFFFF           */
} rdb_kv_record_hdr_t;

/**
 * @brief TSDB sector header — 20 bytes.
 *
 * Written when a sector is initialised.  The count, end_off, and
 * hdr_crc fields remain 0xFFFF until the sector is sealed.
 *
 * Sealing protocol (three-step, power-loss safe):
 *   Step 1: Write count   (offset 14) — records in this sector
 *   Step 2: Write end_off (offset 16) — byte offset of write frontier
 *   Step 3: Write hdr_crc (offset 18) — commits the seal
 *
 * If power is lost during sealing, the CRC will not match, and the
 * sector is classified as ACTIVE (degraded) rather than CORRUPT.
 *
 *  Offset  Field      Size  Description
 *  ──────  ─────────  ────  ──────────────────────────────────
 *    0     magic      4     RDB_TS_SECTOR_MAGIC (0x54534442)
 *    4     erase_cnt  4     Cumulative erase count
 *    8     time_base  4     Base timestamp for delta encoding
 *   12     seq        2     Sector sequence for ring ordering
 *   14     count      2     Record count (0xFFFF = unsealed)
 *   16     end_off    2     Write frontier (0xFFFF = unsealed)
 *   18     hdr_crc    2     CRC16(bytes[0..17]), commits seal
 */
typedef struct {
    uint32_t magic;     /**< @brief Sector identification magic        */
    uint32_t erase_cnt; /**< @brief Cumulative erase cycle count       */
    uint32_t time_base; /**< @brief Base timestamp (delta encoding)    */
    uint16_t seq;       /**< @brief Ring-buffer sequence number        */
    uint16_t count;     /**< @brief Record count (0xFFFF = unsealed)   */
    uint16_t end_off;   /**< @brief Write frontier (0xFFFF = unsealed) */
    uint16_t hdr_crc;   /**< @brief CRC16(bytes[0..17])                */
} rdb_ts_sector_hdr_t;

/**
 * @brief TSDB record header — 12 bytes.
 *
 * Precedes every time-series data record.  Timestamps are stored as
 * a delta from the sector's time_base to minimise storage overhead.
 *
 *  Offset  Field       Size  Description
 *  ──────  ──────────  ────  ─────────────────────────────────
 *    0     magic       1     RDB_TS_RECORD_MAGIC (0xB6)
 *    1     state       1     Record state (WRITING/VALID/DEAD)
 *    2     data_len    2     Data payload length (unpadded)
 *    4     time_delta  4     time − sector.time_base
 *    8     data_crc    2     CRC16(data_bytes)
 *   10     _pad        2     Reserved (0xFFFF)
 */
typedef struct {
    uint8_t  magic;      /**< @brief Record identification magic        */
    uint8_t  state;      /**< @brief Record state machine value         */
    uint16_t data_len;   /**< @brief Data payload length in bytes       */
    uint32_t time_delta; /**< @brief Timestamp delta from time_base     */
    uint16_t data_crc;   /**< @brief CRC16 of data payload bytes        */
    uint16_t _pad;       /**< @brief Reserved, must be 0xFFFF           */
} rdb_ts_record_hdr_t;

#pragma pack(pop)

/* Compile-time size verification */
_Static_assert(sizeof(rdb_kv_sector_hdr_t) == 16u, "KV sector hdr must be 16B");
_Static_assert(sizeof(rdb_kv_record_hdr_t) == 16u, "KV record hdr must be 16B");
_Static_assert(sizeof(rdb_ts_sector_hdr_t) == 20u, "TS sector hdr must be 20B");
_Static_assert(sizeof(rdb_ts_record_hdr_t) == 12u, "TS record hdr must be 12B");
_Static_assert(offsetof(rdb_kv_sector_hdr_t, hdr_crc) == 6u, "KV sector CRC covers bytes [0..5]");
_Static_assert(offsetof(rdb_ts_sector_hdr_t, hdr_crc) == 18u, "TS sector CRC covers bytes [0..17]");

/* ═══════════════════════════════════════════════════════════════════════════
 *  §9  KVDB RAM structures
 *
 *  These structures live in caller-provided RAM and maintain the
 *  runtime state of the KVDB engine.  They are populated during
 *  rdb_kvdb_init() and updated on every write/delete/GC operation.
 * ═══════════════════════════════════════════════════════════════════════════ */

/**
 * @brief Sector status classification.
 *
 * Used by both KVDB sector metadata and GC to determine sector state.
 */
typedef enum {
    RDB_SEC_ERASED = 0, /**< Sector is fully erased (available)        */
    RDB_SEC_ACTIVE = 1, /**< Sector has valid header, accepting writes */
    RDB_SEC_SEALED = 2, /**< Sector is full, read-only until GC        */
    RDB_SEC_CORRUPT = 3 /**< Sector header is invalid/unreadable       */
} rdb_sec_status_t;

/**
 * @brief Per-sector metadata cached in RAM.
 *
 * One instance per sector, stored in the meta_buf array provided
 * to rdb_kvdb_init().  Updated incrementally on writes and GC.
 */
typedef struct {
    uint32_t create_seq;    /**< Sector creation sequence (from header)   */
    uint32_t erase_cnt;     /**< Cumulative erase count (from header)     */
    uint32_t garbage_bytes; /**< Total bytes occupied by DEAD records     */
    uint32_t write_off;     /**< Next writable offset within the sector   */
    uint8_t  status;        /**< Sector status (rdb_sec_status_t)         */
    uint8_t  _pad[3];       /**< Padding for alignment                    */
} rdb_kv_sector_meta_t;

/**
 * @brief KVDB runtime statistics.
 *
 * Accumulative counters for monitoring and diagnostics.
 * Can be retrieved with rdb_kvdb_get_stats() and reset with
 * rdb_kvdb_reset_stats().
 */
typedef struct {
    uint32_t read_ops;           /**< Successful get() calls             */
    uint32_t write_ops;          /**< Successful set() calls             */
    uint32_t delete_ops;         /**< Successful delete() calls          */
    uint32_t gc_runs;            /**< Number of GC executions            */
    uint32_t gc_reclaimed_bytes; /**< Total bytes freed by GC            */
    uint32_t gc_migrated_recs;   /**< Records copied during GC           */
    uint32_t flash_errors;       /**< Flash I/O error count              */
    uint32_t crc_errors;         /**< Data CRC mismatch count            */
    uint32_t corrupt_sectors;    /**< Sectors detected as corrupt        */
} rdb_kv_stats_t;

/**
 * @brief KVDB database handle.
 *
 * The primary control structure for a KVDB instance.  Must be
 * allocated by the caller (stack or static) and passed to all
 * KVDB API functions.
 *
 * Initialised by rdb_kvdb_init() or rdb_kvdb_format().
 * Must not be modified by the application after initialisation.
 */
typedef struct {
    const rdb_partition_t* part;        /**< Partition descriptor          */
    rdb_kv_sector_meta_t*  sectors;     /**< Per-sector metadata array     */
    uint8_t                sector_cnt;  /**< Total number of sectors in partition      */
    uint8_t                gc_reserve;  /**< Number of sectors reserved for GC         */
    uint8_t                active_sec;  /**< Index of the current active (write) sector*/
    uint8_t                initialized; /**< 1 after successful init, 0 otherwise      */
    uint32_t               write_seq;   /**< Global monotonic write sequence counter    */
    uint32_t               live_bytes;  /**< Total bytes occupied by VALID records      */
    uint32_t               write_off;   /**< Current write offset in active sector      */
    uint32_t               iter_gen;    /**< Iterator generation (detects modification) */
    rdb_kv_cache_t         cache;       /**< Key-to-address cache (disabled if size=0)  */
    rdb_kv_stats_t         stats;       /**< Runtime statistics                         */
#if RDB_BLOOM_BITS > 0
    uint8_t*               blooms;      /**< Per-sector bloom bitmaps, RDB_BLOOM_BYTES×sector_cnt */
#endif
} rdb_kvdb_t;

/**
 * @brief KVDB key-value iterator.
 *
 * Provides forward-only iteration over all live key-value pairs.
 * Initialised by rdb_kv_iter_init(), advanced by rdb_kv_iter_next().
 *
 * The iterator detects concurrent database modifications via the
 * generation counter.  If the database is modified between init
 * and next, RDB_ERR_BUSY is returned.
 *
 * Usage:
 *   rdb_kv_iter_t it;
 *   rdb_kv_iter_init(&it, db);
 *   while (rdb_kv_iter_next(&it, key, sizeof(key),
 *                            val, sizeof(val),
 *                            &klen, &vlen) == RDB_OK) {
 *       // process key[0..klen-1], val[0..vlen-1]
 *   }
 */
typedef struct {
    rdb_kvdb_t* db;     /**< Parent database handle                       */
    uint32_t    gen;    /**< Snapshot of db->iter_gen at init time         */
    uint8_t     sector; /**< Current sector index being iterated           */
    uint8_t     _pad[3];/**< Padding for alignment                         */
    uint32_t    offset; /**< Current byte offset within the sector         */
} rdb_kv_iter_t;

/* ═══════════════════════════════════════════════════════════════════════════
 *  §10  TSDB RAM structures
 *
 *  Runtime state for the TSDB ring-buffer engine.
 * ═══════════════════════════════════════════════════════════════════════════ */

/**
 * @brief TSDB query/iteration callback function type.
 *
 * Invoked for each matching record during rdb_tsdb_query() or
 * rdb_tsdb_query_ex().
 *
 * @param ts    Absolute timestamp of the record.
 * @param data  Pointer to the data payload, or NULL if:
 *              - Data CRC verification failed (crc_errors incremented)
 *              - Data is too large for the available read buffer
 *              - Flash read error occurred
 * @param len   Data payload length in bytes.
 * @param arg   User-provided context pointer from the query call.
 * @return      RDB_ITER_CONTINUE (0) to keep iterating,
 *              RDB_ITER_STOP (1) to abort the query.
 */
typedef int (*rdb_ts_cb_t)(uint32_t ts, const void* data,
    uint16_t len, void* arg);

/**
 * @brief TSDB runtime statistics.
 *
 * Accumulative counters for monitoring and diagnostics.
 * Can be retrieved with rdb_tsdb_get_stats() and reset with
 * rdb_tsdb_reset_stats().
 */
typedef struct {
    uint32_t write_ops;        /**< Successful append() calls            */
    uint32_t read_ops;         /**< Records read during queries          */
    uint32_t sector_rotations; /**< Number of head sector rotations      */
    uint32_t records_lost;     /**< Records overwritten by ring wrap     */
    uint32_t flash_errors;     /**< Flash I/O error count                */
    uint32_t crc_errors;       /**< Data CRC mismatch count              */
    uint32_t data_gaps;        /**< Sequence gaps detected during init   */
} rdb_ts_stats_t;

/**
 * @brief TSDB database handle.
 *
 * The primary control structure for a TSDB instance.  Must be
 * allocated by the caller (stack or static) and passed to all
 * TSDB API functions.
 *
 * Ring-buffer model:
 *   ┌─────┬─────┬─────┬─────┬─────┐
 *   │ S0  │ S1  │ S2  │ S3  │ S4  │
 *   │SEAL │SEAL │SEAL │ ACT │     │
 *   └─────┴─────┴─────┴─────┴─────┘
 *     tail ──────────→ head
 *
 *   tail_sec: oldest sector with valid data (read start)
 *   head_sec: current sector accepting new appends (write end)
 *
 *   When head_sec is full, it is sealed and the next sector becomes
 *   the new head.  If the next sector == tail_sec, the tail advances
 *   (oldest data is discarded).
 *
 * Initialised by rdb_tsdb_init() or rdb_tsdb_format().
 * Must not be modified by the application after initialisation.
 */
typedef struct {
    const rdb_partition_t* part;           /**< Partition descriptor              */
    uint32_t*              erase_cnts;     /**< Per-sector erase count array (may be NULL)*/
    uint32_t               sector_size;    /**< Cached sector size from partition        */
    uint16_t               max_data_len;   /**< Maximum data payload per record          */
    uint8_t                sector_cnt;     /**< Total number of sectors in partition      */
    uint8_t                initialized;    /**< 1 after successful init, 0 otherwise      */
    uint8_t                head_sec;       /**< Index of the current head (write) sector  */
    uint8_t                tail_sec;       /**< Index of the oldest (tail) sector         */
    uint32_t               head_seq;       /**< Sequence number of the head sector        */
    uint32_t               head_off;       /**< Write frontier offset in head sector (supports ≤4GB sectors) */
    uint16_t               head_count;     /**< Record count in current head sector       */
    uint16_t               _pad;           /**< Padding for alignment                      */
    uint32_t               head_time_base; /**< time_base of the head sector            */
    uint32_t               last_time;      /**< Timestamp of the most recent record        */
    uint32_t               total_count;    /**< Total VALID records across all sectors     */
    rdb_ts_stats_t         stats;          /**< Runtime statistics                         */
} rdb_tsdb_t;

/* ═══════════════════════════════════════════════════════════════════════════
 *  §11  External primitives (user must implement)
 *
 *  RocketDB requires three external functions for CRC and hashing.
 *  These must be provided by the application (e.g. in a separate
 *  rdb_port.c file).
 *
 *  Suggested implementations:
 *    - CRC-16/MODBUS (polynomial 0xA001 reflected, init 0xFFFF)
 *    - FNV-1a folded to 16 bits for hash
 * ═══════════════════════════════════════════════════════════════════════════ */

/**
 * @brief Compute CRC-16 over a data block.
 *
 * When called with data=NULL, len=0, returns the initial CRC seed.
 * This allows the caller to obtain the seed for streaming CRC
 * computation via rdb_crc16_cont().
 *
 * @param data  Pointer to data block (NULL for seed retrieval).
 * @param len   Length of data in bytes (0 for seed retrieval).
 * @return      Computed CRC-16 value.
 */
extern uint16_t rdb_crc16(const void* data, size_t len);

/**
 * @brief Continue a CRC-16 computation with additional data.
 *
 * Used for streaming CRC over non-contiguous data (e.g. key + value
 * stored separately on flash).
 *
 * @param crc   Previous CRC value (from rdb_crc16 or prior _cont call).
 * @param data  Pointer to the next data block.
 * @param len   Length of data in bytes.
 * @return      Updated CRC-16 value.
 */
extern uint16_t rdb_crc16_cont(uint16_t crc, const void* data, size_t len);

/**
 * @brief Compute a 16-bit hash of a key.
 *
 * Used by KVDB for fast key rejection during scans.  Two records
 * with different hashes are guaranteed to have different keys,
 * avoiding expensive flash reads for key comparison.
 *
 * The hash function should have good distribution to minimise
 * false positives (hash collision → unnecessary flash read).
 *
 * @param data  Pointer to key data.
 * @param len   Key length in bytes.
 * @return      16-bit hash value.
 */
extern uint16_t rdb_hash16(const void* data, size_t len);

/* ═══════════════════════════════════════════════════════════════════════════
 *  §12  Utility functions
 * ═══════════════════════════════════════════════════════════════════════════ */

/**
 * @brief Get the library version as a packed integer.
 * @return  Version in 0x00MMNNPP format (major.minor.patch).
 *          Example: v1.1.0 → 0x010100.
 */
uint32_t rdb_version(void);

/**
 * @brief Compute the required meta-buffer size for KVDB.
 *
 * The caller must allocate at least this many bytes and pass
 * the buffer as `meta_buf` to rdb_kvdb_init().
 *
 * @param sector_cnt  Number of sectors in the KVDB partition.
 * @return            Required buffer size in bytes.
 */
size_t rdb_kvdb_meta_size(uint8_t sector_cnt);

/**
 * @brief Compute the required erase-count buffer size for TSDB.
 *
 * The caller must allocate a uint32_t array of at least this
 * many bytes and pass it as `ec_buf` to rdb_tsdb_init().
 * May be NULL if erase count tracking is not needed.
 *
 * @param sector_cnt  Number of sectors in the TSDB partition.
 * @return            Required buffer size in bytes.
 */
size_t rdb_tsdb_ec_size(uint8_t sector_cnt);

/* ═══════════════════════════════════════════════════════════════════════════
 *  §13  KVDB public API
 *
 *  Lifecycle:     format → init → {set, get, delete, exists, gc} → (repeat)
 *  Thread safety: all functions acquire the flash lock internally.
 *  Memory:        zero dynamic allocation; all buffers caller-provided.
 * ═══════════════════════════════════════════════════════════════════════════ */

/**
 * @brief Initialise the KVDB from existing flash contents.
 *
 * Scans all sectors, recovers interrupted writes, marks stale
 * duplicates, and selects the active sector.  After a successful
 * return, the database is ready for read/write operations.
 *
 * If the flash is blank (never formatted), call rdb_kvdb_format() first.
 *
 * @param db        Database handle (caller-allocated).
 * @param part      Partition descriptor (must outlive the database).
 * @param meta_buf  Buffer for sector metadata; size from rdb_kvdb_meta_size().
 * @return          RDB_OK on success.
 */
rdb_err_t rdb_kvdb_init(rdb_kvdb_t* db, const rdb_partition_t* part,
    void* meta_buf);

/**
 * @brief Erase all sectors and create a fresh KVDB.
 *
 * Preserves erase counts for wear-leveling continuity.
 * After format, sector 0 is the active sector with an empty log.
 *
 * @param db  Database handle (part and sectors must be assigned).
 * @return    RDB_OK on success.
 */
rdb_err_t rdb_kvdb_format(rdb_kvdb_t* db);

/**
 * @brief Write or update a key-value pair.
 *
 * If the key already exists, the old record is invalidated after
 * the new record is committed (update-in-place semantics).
 *
 * May trigger garbage collection if space is insufficient.
 *
 * @param db   Database handle.
 * @param key  Null-terminated key string (1..RDB_MAX_KEY_LEN chars).
 * @param val  Value data pointer (may be NULL if len == 0).
 * @param len  Value length in bytes (0..RDB_MAX_VAL_LEN).
 * @return     RDB_OK, RDB_ERR_FULL, RDB_ERR_TOO_LARGE, etc.
 */
rdb_err_t rdb_kvdb_set(rdb_kvdb_t* db, const char* key,
    const void* val, uint16_t len);

/**
 * @brief Read the value associated with a key.
 *
 * If buf_len < actual value length, copies buf_len bytes and returns
 * RDB_ERR_TOO_LARGE.  out_len always receives the actual length.
 *
 * @param db       Database handle.
 * @param key      Null-terminated key string.
 * @param buf      Buffer to receive the value (may be NULL to query length).
 * @param buf_len  Size of buf in bytes.
 * @param out_len  Receives the actual value length (may be NULL).
 * @return         RDB_OK, RDB_ERR_NOT_FOUND, RDB_ERR_CRC, etc.
 */
rdb_err_t rdb_kvdb_get(rdb_kvdb_t* db, const char* key,
    void* buf, uint16_t buf_len, uint16_t* out_len);

/**
 * @brief Delete all copies of a key.
 *
 * Marks all VALID records matching the key as DEAD across all sectors.
 *
 * @param db   Database handle.
 * @param key  Null-terminated key string.
 * @return     RDB_OK if found and deleted, RDB_ERR_NOT_FOUND otherwise.
 */
rdb_err_t rdb_kvdb_delete(rdb_kvdb_t* db, const char* key);

/**
 * @brief Check if a key exists without reading the value.
 *
 * @param db   Database handle.
 * @param key  Null-terminated key string.
 * @return     RDB_OK on success, RDB_ERR_NOT_FOUND if absent,
 *             RDB_ERR_PARAM or RDB_ERR_NOT_INIT on invalid input.
 */
rdb_err_t rdb_kvdb_exists(rdb_kvdb_t* db, const char* key);

/**
 * @brief Trigger manual garbage collection.
 *
 * If the GC invariant is already satisfied (sufficient erased sectors),
 * this is a no-op.  Otherwise, runs the four-phase GC strategy.
 *
 * @param db  Database handle.
 * @return    RDB_OK if the GC invariant is satisfied after return.
 */
rdb_err_t rdb_kvdb_gc(rdb_kvdb_t* db);

/**
 * @brief Query storage space utilisation.
 *
 * Reports logical capacity (excludes GC reserve sectors).
 *
 * @param db     Database handle.
 * @param total  Total logical capacity in bytes (may be NULL).
 * @param used   Used space (live_bytes) in bytes (may be NULL).
 * @param avail  Available space in bytes (may be NULL).
 */
void rdb_kvdb_space_info(rdb_kvdb_t* db,
    uint32_t* total, uint32_t* used, uint32_t* avail);

/**
 * @brief Query flash wear statistics.
 *
 * @param db      Database handle.
 * @param min_ec  Minimum erase count across all sectors (may be NULL).
 * @param max_ec  Maximum erase count across all sectors (may be NULL).
 * @param avg_ec  Average erase count across all sectors (may be NULL).
 */
void rdb_kvdb_wear_info(rdb_kvdb_t* db,
    uint32_t* min_ec, uint32_t* max_ec, uint32_t* avg_ec);

/**
 * @brief Copy runtime statistics to a caller buffer.
 *
 * @param db   Database handle.
 * @param out  Destination statistics structure.
 */
void rdb_kvdb_get_stats(rdb_kvdb_t* db, rdb_kv_stats_t* out);

/**
 * @brief Reset all runtime statistics counters to zero.
 *
 * @param db  Database handle.
 */
void rdb_kvdb_reset_stats(rdb_kvdb_t* db);

/**
 * @brief Initialise a key-value iterator.
 *
 * @param it  Iterator handle (caller-allocated).
 * @param db  Database handle.
 * @return    RDB_OK on success.
 */
rdb_err_t rdb_kv_iter_init(rdb_kv_iter_t* it, rdb_kvdb_t* db);

/**
 * @brief Advance the iterator to the next live key-value pair.
 *
 * Each call returns the next unique live key and its value.
 * Stale duplicates are automatically skipped.
 *
 * @param it        Iterator handle.
 * @param key_buf   Buffer for key string (null-terminated) (may be NULL).
 * @param key_cap   Size of key_buf in bytes.
 * @param val_buf   Buffer for value data (may be NULL).
 * @param val_cap   Size of val_buf in bytes.
 * @param out_klen  Receives actual key length (may be NULL).
 * @param out_vlen  Receives actual value length (may be NULL).
 * @return          RDB_OK on success, RDB_ERR_ITER_END when exhausted,
 *                  RDB_ERR_BUSY if database was modified since init.
 */
rdb_err_t rdb_kv_iter_next(rdb_kv_iter_t* it,
    char* key_buf, uint16_t key_cap,
    void* val_buf, uint16_t val_cap,
    uint16_t* out_klen, uint16_t* out_vlen);

/* ═══════════════════════════════════════════════════════════════════════════
 *  §14  TSDB public API
 *
 *  Lifecycle:     format → init → {append, query, reset_epoch} → (repeat)
 *  Thread safety: all functions acquire the flash lock internally.
 *  Memory:        zero dynamic allocation; all buffers caller-provided.
 * ═══════════════════════════════════════════════════════════════════════════ */

/**
 * @brief Initialise the TSDB from existing flash contents.
 *
 * Scans all sectors, classifies them, recovers the ring state,
 * and counts total records.  After a successful return, the
 * database is ready for append and query operations.
 *
 * If the flash is blank (never formatted), automatically calls
 * rdb_tsdb_format().
 *
 * @param db      Database handle (caller-allocated).
 * @param part    Partition descriptor (must outlive the database).
 * @param ec_buf  Per-sector erase count array; size from rdb_tsdb_ec_size().
 *                May be NULL if erase count tracking is not needed.
 * @return        RDB_OK on success.
 */
rdb_err_t rdb_tsdb_init(rdb_tsdb_t* db, const rdb_partition_t* part,
    uint32_t* ec_buf);

/**
 * @brief Erase all sectors and create a fresh TSDB.
 *
 * Preserves erase counts for wear-leveling continuity.
 * After format, sector 0 is the head/tail with an empty record log.
 *
 * @param db  Database handle (part must be assigned).
 * @return    RDB_OK on success.
 */
rdb_err_t rdb_tsdb_format(rdb_tsdb_t* db);

/**
 * @brief Append a time-series data record.
 *
 * Timestamps are monotonically enforced: if the provided time is
 * ≤ last_time or 0/INVALID, it is auto-corrected to last_time + 1.
 *
 * If the current head sector is full, a rotation is triggered
 * automatically (seal head, advance to next sector).
 *
 * @param db    Database handle.
 * @param time  Desired timestamp (0 or RDB_TIME_INVALID for auto).
 * @param data  Pointer to data payload.
 * @param len   Data length in bytes (1..max_data_len).
 * @return      RDB_OK, RDB_ERR_TOO_LARGE, RDB_ERR_TIME_EXHAUSTED, etc.
 */
rdb_err_t rdb_tsdb_append(rdb_tsdb_t* db, uint32_t time,
    const void* data, uint16_t len);

/**
 * @brief Force a time epoch boundary.
 *
 * Seals the current head sector and rotates to a fresh one with
 * an uninitialised time_base.  Resets last_time to 0.
 *
 * Use case: RTC wrap-around or clock adjustment that would cause
 * timestamps to go backwards.
 *
 * @param db  Database handle.
 * @return    RDB_OK on success.
 */
rdb_err_t rdb_tsdb_reset_epoch(rdb_tsdb_t* db);

/**
 * @brief Query records within a time range.
 *
 * Iterates forward through the ring (oldest to newest), invoking
 * the callback for each VALID record with time ∈ [from, to].
 *
 * Records larger than RDB_STACK_BUF_SIZE will have NULL data in
 * the callback.  Use rdb_tsdb_query_ex() with a user-provided
 * buffer for large records.
 *
 * @param db    Database handle.
 * @param from  Start timestamp (inclusive, 0 for beginning).
 * @param to    End timestamp (inclusive, 0 for all remaining).
 * @param cb    Callback function invoked per matching record.
 * @param arg   User context pointer passed to cb.
 * @return      RDB_OK on success (even if no records matched).
 */
rdb_err_t rdb_tsdb_query(rdb_tsdb_t* db, uint32_t from, uint32_t to,
    rdb_ts_cb_t cb, void* arg);

/**
 * @brief Query records with a user-provided read buffer.
 *
 * Same as rdb_tsdb_query(), but uses the provided buffer for
 * records larger than RDB_STACK_BUF_SIZE, enabling zero-copy
 * reads of large payloads.
 *
 * @param db        Database handle.
 * @param from      Start timestamp (inclusive).
 * @param to        End timestamp (inclusive).
 * @param cb        Callback function.
 * @param arg       User context pointer.
 * @param read_buf  Buffer for reading large data payloads.
 * @param buf_len   Size of read_buf in bytes.
 * @return          RDB_OK on success.
 */
rdb_err_t rdb_tsdb_query_ex(rdb_tsdb_t* db, uint32_t from, uint32_t to,
    rdb_ts_cb_t cb, void* arg,
    void* read_buf, uint16_t buf_len);

/**
 * @brief Retrieve the most recent record.
 *
 * Searches backwards from the head sector to find the latest VALID
 * record.  Verifies data CRC before returning.
 *
 * @param db       Database handle.
 * @param time     Receives the record's timestamp (may be NULL).
 * @param buf      Buffer for data payload (may be NULL).
 * @param buf_len  Size of buf in bytes.
 * @param out_len  Receives actual data length (may be NULL).
 * @return         RDB_OK, RDB_ERR_NOT_FOUND, RDB_ERR_CRC, etc.
 */
rdb_err_t rdb_tsdb_get_latest(rdb_tsdb_t* db, uint32_t* time,
    void* buf, uint16_t buf_len, uint16_t* out_len);

/**
 * @brief Retrieve the oldest record.
 *
 * Searches forward from the tail sector to find the first VALID
 * record.  Verifies data CRC before returning.
 *
 * @param db       Database handle.
 * @param time     Receives the record's timestamp (may be NULL).
 * @param buf      Buffer for data payload (may be NULL).
 * @param buf_len  Size of buf in bytes.
 * @param out_len  Receives actual data length (may be NULL).
 * @return         RDB_OK, RDB_ERR_NOT_FOUND, RDB_ERR_CRC, etc.
 */
rdb_err_t rdb_tsdb_get_oldest(rdb_tsdb_t* db, uint32_t* time,
    void* buf, uint16_t buf_len, uint16_t* out_len);

/**
 * @brief Get the total number of VALID records in the database.
 *
 * @param db  Database handle.
 * @return    Total record count, or 0 if not initialised.
 */
uint32_t rdb_tsdb_count(rdb_tsdb_t* db);

/**
 * @brief Report the oldest and newest timestamps in the database.
 *
 * @param db      Database handle.
 * @param oldest  Receives the oldest timestamp (may be NULL).
 *                Set to RDB_TIME_INVALID if no records exist.
 * @param newest  Receives the newest timestamp (may be NULL).
 *                Set to RDB_TIME_INVALID if no records exist.
 */
void rdb_tsdb_time_range(rdb_tsdb_t* db, uint32_t* oldest, uint32_t* newest);

/**
 * @brief Query flash wear statistics.
 *
 * If no erase count buffer was provided at init, all values are 0.
 *
 * @param db      Database handle.
 * @param min_ec  Minimum erase count across all sectors (may be NULL).
 * @param max_ec  Maximum erase count across all sectors (may be NULL).
 * @param avg_ec  Average erase count across all sectors (may be NULL).
 */
void rdb_tsdb_wear_info(rdb_tsdb_t* db,
    uint32_t* min_ec, uint32_t* max_ec, uint32_t* avg_ec);

/**
 * @brief Copy runtime statistics to a caller buffer.
 *
 * @param db   Database handle.
 * @param out  Destination statistics structure.
 */
void rdb_tsdb_get_stats(rdb_tsdb_t* db, rdb_ts_stats_t* out);

/**
 * @brief Reset all runtime statistics counters to zero.
 *
 * @param db  Database handle.
 */
void rdb_tsdb_reset_stats(rdb_tsdb_t* db);

#ifdef __cplusplus
}
#endif

#endif /* ROCKETDB_H */
