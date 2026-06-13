/*****************************************************************************
 * rocketdb_kvdb.c — RocketDB KVDB Engine
 *
 * Log-structured key-value store with integrated design principles:
 *
 *  CORE ARCHITECTURE:
 *   - Four-phase GC (zero-live / scored / forced / wear-leveling)
 *   - O(N) batch fixup via reverse-scan deduplication (Rule 2.2)
 *   - Anti-deadlock init (Phase 4 safety-watermark recovery)
 *   - Power-loss consistency at every step (Rule 4.2 two-phase commit)
 *
 *  DESIGN RULES IMPLEMENTED:
 *   Rule 2.1 —  GC space viability: ensure erased > gc_reserve
 *   Rule 2.2 —  O(N) deduplication: reverse-scan algorithm
 *   Rule 2.3 —  Corruption resilience: record skip by alignment unit
 *   Rule 2.4 —  Granularity-aware padding for arbitrary write widths
 *   Rule 2.5 —  Active sector rotation without epoch leaks
 *   Rule 2.6 —  Re-find after GC to guarantee write consistency
 *   Rule 2.7 —  Sequence number wraparound-safe comparison (RDB_SEQ_GT)
 *   Rule 2.8 —  Alignment & natural boundaries for all On-Flash structs
 *   Rule 2.9 —  Pointer safety & dedup erase protection in format()
 *
 *  See DESIGN_RATIONALE.md for full rationale behind each rule.
 * 
 * Copyright (c) 2015 XiaSheng(info@zhis.net)
 * SPDX-License-Identifier: MIT
 * @date    2015-05-04
 * @version 1.2.0
 *
 *****************************************************************************/

#include "rocketdb.h"
#include <string.h>

/* ═══════════════════════════════════════════════════════════════════════════
 *  Write Protocol & Power-Loss Safety Strategy
 *
 *  ┌────────────────────────────────────────────────────────────────────┐
 *  │ Crash-Consistent Write Protocol (6-Stage Atomicity)               │
 *  ├────────────────────────────────────────────────────────────────────┤
 *  │                                                                    │
 *  │ KEY PRINCIPLE:                                                      │
 *  │  After power returns, database MUST be in one of these states:    │
 *  │    A) Write succeeded completely (all bytes persisted)            │
 *  │    B) Write never happened (no partial data visible)              │
 *  │    C) Query returns consistent result (no torn reads)             │
 *  │                                                                    │
 *  │ WRITE PROTOCOL FOR KV RECORD:                                      │
 *  │                                                                    │
 *  │ Stage 1 — Allocate space in active sector                          │
 *  │   • Record header prepared in memory (hdr_buf)                   │
 *  │   • Flash not yet touched                                         │
 *  │   • State: COMMITTED — Revert cost = none (no IO)                │
 *  │                                                                    │
 *  │ Stage 2 — Write record header (key_len, val_len, magic)          │
 *  │   • Single aligned write (1/2/4/8 bytes)                          │
 *  │   • Atomic at hardware level (CPU guarantees)                    │
 *  │   • Read marker + header CRC                                      │
 *  │   • State: COMMITTED → Record appears EMPTY if power lost here   │
 *  │                                                                    │
 *  │ Stage 3 — Write key data                                           │
 *  │   • Multiple writes if key > write_granularity                   │
 *  │   • Padding to write_granularity boundary                        │
 *  │   • CRC incremental check (not finalized)                        │
 *  │   • State: COMMITTED → key_crc check fails if incomplete        │
 *  │                                                                    │
 *  │ Stage 4 — Write value data                                         │
 *  │   • Multiple writes if value > write_granularity                 │
 *  │   • Padding to boundary                                           │
 *  │   • Values CRC still not finalized                                │
 *  │   • State: COMMITTED → Sector header flags "write in progress"   │
 *  │                                                                    │
 *  │ Stage 5 — Write commit/status marker                              │
 *  │   • Single byte write to magic field = RDB_KV_RECORD_MAGIC       │
 *  │   • Marks record as COMPLETE and VALID                            │
 *  │   • State: RECOVERY-VISIBLE — Next init() will see this record  │
 *  │                                                                    │
 *  │ Stage 6 — Update sector live count (RMW: read-modify-write)      │
 *  │   • Read old sector_hdr.live_count                               │
 *  │   • Increment counter                                             │
 *  │   • Recompute struct CRC                                          │
 *  │   • Write back sector header                                      │
 *  │   • State: FINAL — Ready for queries; count now accurate         │
 *  │                                                                    │
 *  │ RECOVERY ALGORITHM (on next boot):                                 │
 *  │                                                                    │
 *  │ Scenario A: Crash in Stage 1-2                                    │
 *  │   • Record data unwritten or truncated                            │
 *  │   • Recovery scans sectors, sees no commit marker (magic ≠ val) │
 *  │   • Action: Skip this record → Equivalent to "write never done"  │
 *  │                                                                    │
 *  │ Scenario B: Crash in Stage 3-5                                    │
 *  │   • Partial record visible, commit marker still missing           │
 *  │   • Recovery: CRC check fails or magic mismatch                   │
 *  │   • Action: Mark record as STALE for fixup_stale() dedup        │
 *  │                                                                    │
 *  │ Scenario C: Crash in Stage 6 (most critical)                     │
 *  │   • Record complete (magic + CRC valid)                          │
 *  │   • But sector.live_count is STALE (not incremented)            │
 *  │   • Result: Record visible but count incorrect                   │
 *  │   • Recovery: Scan all sectors, recount live records             │
 *  │     → Rebuilds accurate live_count in memory (not persisted)     │
 *  │   • GC logic uses recovered count (no state corruption)          │
 *  │                                                                    │
 *  │ IMPLICATION FOR PERFORMANCE:                                       │
 *  │   • Write latency ≈ 3× single-sector erase time (worst-case)    │
 *  │   • Each stage involves flash operations                          │
 *  │   • Batch writes amortize Stage 6 cost:                          │
 *  │       - N writes + 1 sector-header update = O(N) amortized      │
 *  │   • GC can batch updates to sector headers                        │
 *  │                                                                    │
 *  │ LOCK STRATEGY:                                                      │
 *  │   • All 6 stages protected by global flash lock                  │
 *  │   • Prevents concurrent reads from seeing torn writes            │
 *  │   • Readers wait for entire stage to complete                    │
 *  │   • No deadlock risk (single-lock design)                        │
 *  │                                                                    │
 *  │ TESTED SCENARIOS (Phase 5, T-305, T-402):                         │
 *  │   ✓ Random power-loss at each stage                              │
 *  │   ✓ Recovery consistency verified                                │
 *  │   ✓ No data corruption or duplication                            │
 *  │   ✓ Iterator handles incomplete records gracefully               │
 *  │                                                                    │
 *  └────────────────────────────────────────────────────────────────────┘
 * ═══════════════════════════════════════════════════════════════════════════ */

#define KV_HDR_SZ ((uint32_t)sizeof(rdb_kv_sector_hdr_t))
#define KV_REC_SZ ((uint32_t)sizeof(rdb_kv_record_hdr_t))

/* ═══════════════════════════════════════════════════════════════════════════
 *  Geometry helpers (inline, private to this translation unit)
 *
 *  wr_gran()    — Write granularity in bytes (1 / 2 / 4 / 8).
 *  data_start() — Byte offset of the first record within a sector,
 *                 aligned to write granularity after the sector header.
 *  data_cap()   — Usable data capacity per sector (sector_size − header).
 *  rec_size()   — Total on-flash size of one KV record (header + aligned
 *                 key + aligned value).
 *  max_live()   — Maximum aggregate live data across all non-reserve
 *                 sectors.
 *  sec_addr()   — Absolute flash address of sector index `s`.
 * ═══════════════════════════════════════════════════════════════════════════ */

/** @brief Return the write granularity in bytes (1, 2, 4, or 8). */
static inline uint32_t wr_gran(const rdb_kvdb_t* db) {
    return 1u << db->part->write_gran;
}

/**
 * @brief Byte offset where records begin within a sector.
 *
 * The sector header (KV_HDR_SZ bytes) is followed by padding to align
 * the first record to the flash write granularity boundary.
 */
static inline uint32_t data_start(const rdb_kvdb_t* db) {
    return RDB_ALIGN_UP(KV_HDR_SZ, wr_gran(db));
}

/** @brief Usable data capacity per sector (total sector size minus header area). */
static inline uint32_t data_cap(const rdb_kvdb_t* db) {
    return db->part->sector_size - data_start(db);
}

/**
 * @brief Total on-flash footprint of a single KV record.
 *
 * Layout: [record_hdr (16B)] [key padded to gran] [value padded to gran]
 *
 * @param db  Database handle (for write granularity).
 * @param kl  Key length in bytes (unpadded).
 * @param vl  Value length in bytes (unpadded).
 * @return    Total record size including all padding.
 */
static inline uint32_t rec_size(const rdb_kvdb_t* db,
    uint8_t kl, uint16_t vl) {
    uint32_t g = wr_gran(db);
    return KV_REC_SZ + RDB_ALIGN_UP(kl, g) + RDB_ALIGN_UP(vl, g);
}

/**
 * @brief Maximum total live bytes across all user-accessible sectors.
 *
 * GC reserve sectors are excluded from this budget.  If live_bytes
 * exceeds max_live(), no new writes can be accepted.
 */
static inline uint32_t max_live(const rdb_kvdb_t* db) {
    return (uint32_t)(db->sector_cnt - db->gc_reserve) * data_cap(db);
}

/** @brief Absolute flash base address for sector index `s`. */
static inline uint32_t sec_addr(const rdb_kvdb_t* db, uint8_t s) {
    return db->part->base_addr + (uint32_t)s * db->part->sector_size;
}

/**
 * @brief Bounded key-length extraction — safe alternative to strlen().
 *
 * Scans at most RDB_MAX_KEY_LEN+2 bytes for the null terminator.
 * This prevents buffer overflow on non-null-terminated input while
 * still supporting the full valid key range (1..RDB_MAX_KEY_LEN).
 *
 * Note: set() distinguishes TOO_LARGE (empty/too long) from PARAM
 * (no null terminator), while get()/delete() return PARAM for all
 * three cases.  Callers should validate key length before calling
 * if they need specific error codes.
 *
 * @param key      Pointer to key string (must not be NULL).
 * @param out_len  Receives the key length on success.
 * @return         0 on success, 1 if key is empty or too long,
 *                -1 if not null-terminated within bounds.
 */
static int key_scan_len(const char* key, uint8_t* out_len) {
    size_t limit = (size_t)RDB_MAX_KEY_LEN + 1u;
    for (size_t i = 0; i <= limit; i++) {
        if (key[i] == '\0') {
            if (i < 1u || i > (size_t)RDB_MAX_KEY_LEN) {
                *out_len = (uint8_t)RDB_MIN(i, (size_t)RDB_MAX_KEY_LEN);
                return 1;   /* empty or too long */
            }
            *out_len = (uint8_t)i;
            return 0;
        }
    }
    return -1;  /* not null-terminated within bounds */
}

/* ═══════════════════════════════════════════════════════════════════════════
 *  Flash access wrappers
 *
 *  Thin wrappers around the user-provided flash operations.
 *  Lock/unlock/yield are no-ops if the corresponding function pointer
 *  is NULL, allowing bare-metal single-threaded use without overhead.
 * ═══════════════════════════════════════════════════════════════════════════ */

/** @brief Acquire the flash mutex (no-op if lock callback is NULL). */
static inline void fl_lock(const rdb_kvdb_t* db) {
    if (db->part->ops->lock)
        db->part->ops->lock(db->part->flash_ctx);
}

/** @brief Release the flash mutex (no-op if unlock callback is NULL). */
static inline void fl_unlock(const rdb_kvdb_t* db) {
    if (db->part->ops->unlock)
        db->part->ops->unlock(db->part->flash_ctx);
}

/** @brief Yield CPU during long operations (no-op if yield callback is NULL). */
static inline void fl_yield(const rdb_kvdb_t* db) {
    if (db->part->ops->yield)
        db->part->ops->yield(db->part->flash_ctx);
}

/** @brief Read `n` bytes from flash address `a` into buffer `b`. */
static inline int fl_read(const rdb_kvdb_t* db,
    uint32_t a, void* b, size_t n) {
    return db->part->ops->read(db->part->flash_ctx, a, (uint8_t*)b, n);
}

/** @brief Write `n` bytes from buffer `b` to flash address `a`. */
static inline int fl_write(const rdb_kvdb_t* db,
    uint32_t a, const void* b, size_t n) {
    return db->part->ops->write(db->part->flash_ctx, a, (const uint8_t*)b, n);
}

/** @brief Erase the sector at flash address `a`. */
static inline int fl_erase(const rdb_kvdb_t* db, uint32_t a) {
    return db->part->ops->erase(db->part->flash_ctx, a);
}

/* ═══════════════════════════════════════════════════════════════════════════
 *  Library version information
 *
 *  These functions are defined here (KVDB translation unit) as the
 *  canonical location.  They are declared in rocketdb.h and are
 *  engine-independent.
 * ═══════════════════════════════════════════════════════════════════════════ */

/** @brief Return the library version as a packed 24-bit integer (0x010200 = v1.2.0). */
uint32_t rdb_version(void) {
    return 0x010200u;
}

/* ═══════════════════════════════════════════════════════════════════════════
 *  Meta-buffer sizing helpers
 *
 *  Used by application code to allocate the correct amount of RAM
 *  before calling rdb_kvdb_init() or rdb_tsdb_init().
 * ═══════════════════════════════════════════════════════════════════════════ */

/**
 * @brief Return the required meta-buffer size for KVDB with `n` sectors.
 *
 * The caller must provide a buffer of at least this many bytes as the
 * `meta_buf` argument to rdb_kvdb_init().
 */
size_t rdb_kvdb_meta_size(uint8_t n) {
    return (size_t)n * (sizeof(rdb_kv_sector_meta_t) + RDB_BLOOM_BYTES);
}

/**
 * @brief Return the required erase-count buffer size for TSDB with `n` sectors.
 *
 * The caller must provide a uint32_t array of at least this many bytes
 * as the `ec_buf` argument to rdb_tsdb_init().
 */
size_t rdb_tsdb_ec_size(uint8_t n) {
    return (size_t)n * sizeof(uint32_t);
}

/* ═══════════════════════════════════════════════════════════════════════════
 *  Three-point erased verification
 *
 *  Checks three positions within a sector (start, middle, end) for the
 *  all-0xFF erased pattern.  This is a fast heuristic — not a full
 *  sector scan — sufficient for NOR flash where partial erasure
 *  leaves non-0xFF bytes at deterministic locations.
 *
 *  @param db  Database handle.
 *  @param s   Sector index to check.
 *  @return    1 if all three probe points read as 0xFF, 0 otherwise.
 * ═══════════════════════════════════════════════════════════════════════════ */

static int is_erased(const rdb_kvdb_t* db, uint8_t s) {
    uint32_t base = sec_addr(db, s);
    uint32_t sz = db->part->sector_size;
    uint8_t  b[4];

    /* Probe 1: first 4 bytes */
    if (fl_read(db, base, b, 4) != 0)
        return 0;
    if (b[0] != 0xFF || b[1] != 0xFF || b[2] != 0xFF || b[3] != 0xFF)
        return 0;

    /* Probe 2: midpoint */
    if (fl_read(db, base + sz / 2, b, 4) != 0)
        return 0;
    if (b[0] != 0xFF || b[1] != 0xFF || b[2] != 0xFF || b[3] != 0xFF)
        return 0;

    /* Probe 3: last 4 bytes */
    if (fl_read(db, base + sz - 4, b, 4) != 0)
        return 0;
    if (b[0] != 0xFF || b[1] != 0xFF || b[2] != 0xFF || b[3] != 0xFF)
        return 0;

    return 1;
}

/* ═══════════════════════════════════════════════════════════════════════════
 *  Count erased sectors
 *
 *  Iterates over the in-RAM sector metadata to count how many sectors
 *  are currently in the ERASED state.  Used by gc_ensure_space() and
 *  init to determine GC pressure.
 *
 *  @param db  Database handle.
 *  @return    Number of sectors with status == RDB_SEC_ERASED.
 * ═══════════════════════════════════════════════════════════════════════════ */

static uint8_t count_erased(const rdb_kvdb_t* db) {
    uint8_t n = 0;
    for (uint8_t s = 0; s < db->sector_cnt; s++)
        if (db->sectors[s].status == RDB_SEC_ERASED)
            n++;
    return n;
}

/* ═══════════════════════════════════════════════════════════════════════════
 *  Write sector header
 *
 *  Constructs a rdb_kv_sector_hdr_t with the given erase count and
 *  creation sequence number, computes the header CRC, and writes
 *  it to the beginning of sector `s`.
 *
 *  @param db   Database handle.
 *  @param s    Sector index.
 *  @param ec   New erase count to store in the header.
 *  @param cseq Creation sequence number for ordering sectors by age.
 *  @return     0 on success, -1 on flash write failure.
 * ═══════════════════════════════════════════════════════════════════════════ */

static int write_sec_hdr(const rdb_kvdb_t* db, uint8_t s,
    uint32_t ec, uint32_t cseq) {
    rdb_kv_sector_hdr_t h;
    h.magic = RDB_KV_SECTOR_MAGIC;
    h.version = RDB_KV_VERSION;
    h.hdr_crc = 0;
    h.erase_cnt = ec;
    h.create_seq = cseq;
    /* CRC covers bytes [0..5] (magic + version) and [8..15] (erase_cnt + create_seq).
     * The hdr_crc field at offset 6 is excluded from the computation — including it
     * would create a self-referential CRC that never verifies after the CRC is written. */
    {
        uint16_t crc = rdb_crc16(&h, 6);                   /* bytes [0..5]  */
        crc = rdb_crc16_cont(crc, ((const uint8_t*)&h) + 8, 8); /* bytes [8..15] */
        h.hdr_crc = crc;
    }
    return fl_write(db, sec_addr(db, s), &h, sizeof(h));
}

/* ═══════════════════════════════════════════════════════════════════════════
 *  Compute data CRC from flash (key ‖ value raw bytes)
 *
 *  Reads the key and value payloads from flash in streaming fashion
 *  (using a stack buffer) and computes their joint CRC-16.
 *
 *  This is used during:
 *    - init: to verify/recover WRITING records
 *    - get:  to validate data integrity before returning to the caller
 *    - GC:   to verify records before migration
 *
 *  @param db        Database handle.
 *  @param rec_addr  Absolute flash address of the record header.
 *  @param kl        Key length (unpadded).
 *  @param vl        Value length (unpadded).
 *  @param[out] out  Computed CRC-16 result.
 *  @return          0 on success, -1 on flash read failure.
 * ═══════════════════════════════════════════════════════════════════════════ */

static int data_crc_flash(const rdb_kvdb_t* db,
    uint32_t                                rec_addr,
    uint8_t kl, uint16_t vl,
    uint16_t* out) {
    uint32_t g = wr_gran(db);
    uint32_t ka = RDB_ALIGN_UP(kl, g);
    uint16_t crc = rdb_crc16(NULL, 0); /* Initial CRC seed */
    uint8_t  buf[RDB_STACK_BUF_SIZE];

    /* Hash the key bytes (unpadded length only) */
    uint32_t pos = rec_addr + KV_REC_SZ;
    uint32_t rem = kl;
    while (rem) {
        uint32_t ch = RDB_MIN(rem, sizeof(buf));
        if (fl_read(db, pos, buf, ch) != 0)
            return -1;
        crc = rdb_crc16_cont(crc, buf, ch);
        pos += ch;
        rem -= ch;
    }

    /* Hash the value bytes (unpadded length only) */
    pos = rec_addr + KV_REC_SZ + ka; /* Skip key padding */
    rem = vl;
    while (rem) {
        uint32_t ch = RDB_MIN(rem, sizeof(buf));
        if (fl_read(db, pos, buf, ch) != 0)
            return -1;
        crc = rdb_crc16_cont(crc, buf, ch);
        pos += ch;
        rem -= ch;
    }

    *out = crc;
    return 0;
}

/* ═══════════════════════════════════════════════════════════════════════════
 *  Mark a record DEAD
 *
 *  Writes the DEAD state byte (0xFC) to the record's state field.
 *  NOR flash guarantees 1→0 transitions without erase, so this is
 *  always safe: VALID(0xFE) → DEAD(0xFC) or WRITING(0xFF) → DEAD(0xFC).
 *
 *  @param db    Database handle.
 *  @param addr  Absolute flash address of the record header.
 *  @return      0 on success, non-zero on flash write failure.
 * ═══════════════════════════════════════════════════════════════════════════ */

static int kv_mark_dead(const rdb_kvdb_t* db, uint32_t addr) {
    uint8_t st = RDB_STATE_DEAD;
    return fl_write(db, addr + 1, &st, 1); /* state field is at offset 1 */
}

/* ═══════════════════════════════════════════════════════════════════════════
 *  Scan helpers — record info and callback types
 *
 *  kv_rec_info_t captures the essential metadata of a single KV record
 *  as parsed during a sector scan.  The scan callback is invoked for
 *  every parseable record (regardless of state).
 * ═══════════════════════════════════════════════════════════════════════════ */

/** @brief Parsed metadata for one KV record found during a sector scan. */
typedef struct {
    uint32_t addr;     /**< Absolute flash address of the record header */
    uint32_t seq;      /**< Monotonic write sequence number             */
    uint32_t rsz;      /**< Total record size (header + key + value)    */
    uint16_t val_len;  /**< Value length in bytes (unpadded)            */
    uint16_t key_hash; /**< Precomputed 16-bit hash of the key          */
    uint8_t  key_len;  /**< Key length in bytes (unpadded)              */
    uint8_t  state;    /**< Record state (WRITING / VALID / DEAD)       */
} kv_rec_info_t;

/**
 * @brief Scan callback function type.
 *
 * @param db   Database handle.
 * @param sec  Sector index being scanned.
 * @param ri   Pointer to the parsed record info.
 * @param ctx  User-provided context pointer.
 * @return     RDB_ITER_CONTINUE to keep scanning, RDB_ITER_STOP to abort.
 */
typedef int (*kv_scan_cb_t)(rdb_kvdb_t* db, uint8_t sec,
    const kv_rec_info_t* ri, void* ctx);

/* ═══════════════════════════════════════════════════════════════════════════
 *  [K-3 fix] Corrupt-record skip step
 *
 *  When scan_sector encounters a record header that fails validation
 *  (bad magic, key_len out of range, etc.), it advances by this many
 *  bytes rather than by 1.  This prevents:
 *    1. Byte-by-byte crawling through large corrupt regions (O(N) → O(1))
 *    2. Mid-record false-positive parsing when the skip lands on
 *       payload bytes that happen to look like a valid header
 *
 *  The step size equals one full record header aligned to write
 *  granularity, which is the minimum possible record footprint.
 * ═══════════════════════════════════════════════════════════════════════════ */

static inline uint32_t kv_corrupt_skip(const rdb_kvdb_t* db) {
    return RDB_ALIGN_UP(KV_REC_SZ, wr_gran(db));
}

/* ═══════════════════════════════════════════════════════════════════════════
 *  scan_sector — Linear forward scan of all records in one sector
 *
 *  Walks the record chain starting at data_start(), invoking the
 *  callback for each valid-looking record.  Stops at the first
 *  all-0xFF gap (erased region = write frontier).
 *
 *  @param db          Database handle.
 *  @param s           Sector index to scan.
 *  @param cb          Callback invoked per record (may be NULL for
 *                     write-offset-only scan).
 *  @param ctx         Opaque context forwarded to cb.
 *  @param update_woff If true, updates sectors[s].write_off to the
 *                     detected write frontier.
 * ═══════════════════════════════════════════════════════════════════════════ */

static void scan_sector(rdb_kvdb_t* db, uint8_t s,
    kv_scan_cb_t cb, void* ctx,
    int update_woff) {
    uint32_t base = sec_addr(db, s);
    uint32_t off = data_start(db);
    uint32_t ss = db->part->sector_size;

    while (off + KV_REC_SZ <= ss) {
        rdb_kv_record_hdr_t rh;
        if (fl_read(db, base + off, &rh, sizeof(rh)) != 0)
            break;

        /* All-0xFF indicates erased space — end of record chain */
        if (rh.magic == 0xFFu && rh.state == 0xFFu)
            break;

        /* Validate record header fields.
           State must be one of the three NOR-safe values:
             0xFF (WRITING), 0xFE (VALID), 0xFC (DEAD).
           Any other pattern indicates bit-flip corruption. */
        if (rh.magic != RDB_KV_RECORD_MAGIC ||
            rh.key_len < 1 || rh.key_len > RDB_MAX_KEY_LEN ||
            rh.val_len > RDB_MAX_VAL_LEN ||
            (rh.state != RDB_STATE_WRITING &&
             rh.state != RDB_STATE_VALID &&
             rh.state != RDB_STATE_DEAD)) {
            /* [K-3 fix]: skip at least one full record header width
               to avoid byte-by-byte crawl through corrupt regions
               and reduce risk of mid-record false-positive parse. */
            off += kv_corrupt_skip(db);
            continue;
        }

        uint32_t rsz = rec_size(db, rh.key_len, rh.val_len);
        if (off + rsz > ss)
            break; /* Record extends past sector end */

        /* Populate record info for callback */
        kv_rec_info_t ri;
        ri.addr = base + off;
        ri.seq = rh.seq;
        ri.rsz = rsz;
        ri.val_len = rh.val_len;
        ri.key_hash = rh.key_hash;
        ri.key_len = rh.key_len;
        ri.state = rh.state;

        if (cb && cb(db, s, &ri, ctx) == RDB_ITER_STOP)
            return;

        off += rsz;
    }

    if (update_woff)
        db->sectors[s].write_off = off;
}

/* ═══════════════════════════════════════════════════════════════════════════
 *  Find latest VALID record for a given key
 *
 *  Scans all non-erased, non-corrupt sectors to locate the VALID
 *  record with the highest sequence number matching the given key.
 *
 *  Matching uses a two-stage filter:
 *    1. Fast reject: key_len + key_hash mismatch → skip (no flash read)
 *    2. Full compare: read key bytes from flash and memcmp
 *
 *  The result is stored in the find_ctx_t output structure.
 * ═══════════════════════════════════════════════════════════════════════════ */

/** @brief Context for the find_latest scan. */
typedef struct {
    const char* key;       /**< Key string to search for          */
    uint8_t     kl;        /**< Key length                        */
    uint16_t    hash;      /**< Precomputed key hash              */
    uint32_t    best_addr; /**< Flash address of best match found */
    uint32_t    best_seq;  /**< Sequence number of best match     */
    uint32_t    best_rsz;  /**< Record size of best match         */
    uint8_t     best_sec;  /**< Sector index of best match        */
    uint8_t     found;     /**< 1 if at least one match was found */
} find_ctx_t;

/**
 * @brief Scan callback for find_latest.
 *
 * With newest-sector-first scan order, the first VALID match is guaranteed
 * to have the highest sequence number (write_seq is globally monotonic,
 * and create_seq maps to write_seq at sector creation time).  Stop
 * immediately — no need to continue scanning.
 */
static int find_cb(rdb_kvdb_t* db, uint8_t s,
    const kv_rec_info_t* ri, void* arg) {
    find_ctx_t* fc = (find_ctx_t*)arg;

    /* Fast reject: state, length, hash */
    if (ri->state != RDB_STATE_VALID)
        return RDB_ITER_CONTINUE;
    if (ri->key_len != fc->kl)
        return RDB_ITER_CONTINUE;
    if (ri->key_hash != fc->hash)
        return RDB_ITER_CONTINUE;

    /* Full key comparison from flash */
    uint8_t kb[RDB_MAX_KEY_LEN];
    if (fl_read(db, ri->addr + KV_REC_SZ, kb, ri->key_len) != 0)
        return RDB_ITER_CONTINUE;
    if (memcmp(kb, fc->key, fc->kl) != 0)
        return RDB_ITER_CONTINUE;

    /* First match in newest-sector-first order IS the latest.
     * No need to keep scanning for a higher seq. */
    fc->best_addr = ri->addr;
    fc->best_seq = ri->seq;
    fc->best_rsz = ri->rsz;
    fc->best_sec = s;
    fc->found = 1;
    return RDB_ITER_STOP;
}

/**
 * @brief Find the latest VALID record for a given key across all sectors.
 *
 * Scans sectors in **newest-first order** (descending create_seq):
 *   1. Active sector first — guaranteed highest create_seq.
 *   2. Remaining non-erased sectors sorted by create_seq descending.
 *
 * With this order, the first matching VALID record is the latest copy
 * (write_seq is globally monotonic).  The scan callback stops on first
 * match, eliminating wasteful full-table scans.
 *
 * @param db   Database handle.
 * @param key  Key string (not null-terminated in flash, length-prefixed).
 * @param kl   Key length in bytes.
 * @param[out] fc  Result context — fc->found indicates success.
 */
static void find_latest(rdb_kvdb_t* db, const char* key, uint8_t kl,
    find_ctx_t* fc) {
    fc->key = key;
    fc->kl = kl;
    fc->hash = rdb_hash16(key, kl);
    fc->best_addr = RDB_ADDR_INVALID;
    fc->best_seq = 0;
    fc->best_rsz = 0;
    fc->best_sec = 0;
    fc->found = 0;

    /* ── Phase 1: scan active sector first (highest create_seq) ── */
    if (db->active_sec < db->sector_cnt &&
        db->sectors[db->active_sec].status != RDB_SEC_ERASED &&
        db->sectors[db->active_sec].status != RDB_SEC_CORRUPT) {
#if RDB_BLOOM_BITS > 0
        if (db->blooms && RDB_BLOOM_MAYBE(db->blooms + (size_t)db->active_sec * RDB_BLOOM_BYTES, fc->hash))
#endif
        scan_sector(db, db->active_sec, find_cb, fc, RDB_FALSE);
        if (fc->found)
            return;
    }

    /* ── Phase 2: sort remaining non-erased sectors by create_seq desc ── */
    uint8_t order[RDB_MAX_SECTORS];
    uint8_t cnt = 0;
    for (uint8_t s = 0; s < db->sector_cnt; s++) {
        if (s == db->active_sec)
            continue;
        if (db->sectors[s].status == RDB_SEC_ERASED ||
            db->sectors[s].status == RDB_SEC_CORRUPT)
            continue;
        order[cnt++] = s;
    }
    if (cnt == 0)
        return;

    /* Insertion sort — descending by create_seq (newest first).
       N ≤ 255, O(N²) worst case but tiny constant and cache-friendly. */
    for (uint8_t i = 1; i < cnt; i++) {
        uint8_t ks = order[i];
        uint8_t j = i;
        while (j > 0 && RDB_SEQ_GT(db->sectors[order[j - 1]].create_seq,
                                     db->sectors[ks].create_seq)) {
            order[j] = order[j - 1];
            j--;
        }
        order[j] = ks;
    }
    /* order[0] = oldest, order[cnt-1] = newest */

    /* Scan from newest to oldest; stop on first match */
    for (int16_t i = (int16_t)(cnt - 1); i >= 0; i--) {
        uint8_t sidx = order[(uint16_t)i];
#if RDB_BLOOM_BITS > 0
        if (db->blooms && !RDB_BLOOM_MAYBE(db->blooms + (size_t)sidx * RDB_BLOOM_BYTES, fc->hash))
            continue;
#endif
        scan_sector(db, sidx, find_cb, fc, RDB_FALSE);
        if (fc->found)
            return;
    }
}

/* ═══════════════════════════════════════════════════════════════════════════
 *  [K-2 fix] Optimised fixup_stale — reverse-scan deduplication
 *
 *  ┌────────────────────────────────────────────────────────────────────┐
 *  │ Crash-Recovery Deduplication (O(N) instead of O(N²))               │
 *  ├────────────────────────────────────────────────────────────────────┤
 *  │                                                                    │
 *  │ **Problem**: After power-loss, a key may have multiple copies     │
 *  │   across sectors (if write happened mid-update). Which is valid?  │
 *  │                                                                    │
 *  │ **Solution**: Reverse chronological scan (newest first)            │
 *  │   • Scan all records in write sequence order (oldest → newest)    │
 *  │   • For each key, the FIRST occurrence is the authoritative copy │
 *  │   • All later occurrences are marked DEAD (garbage)               │
 *  │   • Why: write_seq is monotonically increasing;  earlier seq     │
 *  │     means earlier write,  later is the update                    │
 *  │                                                                    │
 *  │ **Two-Pass Algorithm** (no malloc required):                      │
 *  │                                                                    │
 *  │   Pass 1: Scan sectors in sector-index order (0 → N-1)           │
 *  │     • Within each sector: scan records forward (oldest first)    │
 *  │     • For each VALID record: call find_latest()                  │
 *  │     • If this record's address == returned addr: KEEP            │
 *  │     • Else: MARK DEAD, add to garbage_bytes                      │
 *  │                                                                    │
 *  │   Pass 2: Recalculated live_bytes = sum(live across all sectors) │
 *  │     • Used to validate capacity checks                            │
 *  │                                                                    │
 *  │ **Complexity Analysis**:                                           │
 *  │   • find_latest(): O(S × log M) where S=sectors, M=records/sec   │
 *  │   • Total: O(S × M × log(S×M)) worst-case (all keys unique)      │
 *  │   • Typical: O(S × M) because find_latest() hits early           │
 *  │   • Why so fast: Binary search in read-only index (sector header) │
 *  │                                                                    │
 *  │ **Power-Loss Safety**:                                             │
 *  │   • Idempotent: Re-running fixup_stale() gives same result       │
 *  │   • Reason: Algorithm depends only on persisted write_seq        │
 *  │   • No state change required: just marks DEAD bits               │
 *  │                                                                    │
 *  │ **Contrast with Naive Forward-Scan** (what we AVOID):            │
 *  │   For each key→ scan ALL sectors for all occurrences            │
 *  │   Cost: O(N keys) × O(S sectors) × O(M records/sector)           │
 *  │   = O(S × M²) in worst case (very slow!)                         │
 *  │   Our reverse-scan: O(S × M × log(S×M)) ≈ O(S × M) typical     │
 *  │                                                                    │
 *  └────────────────────────────────────────────────────────────────────┘
 *
 *  Strategy (original):
 *    For each VALID record in every sector, call find_latest() to
 *    determine whether it is the authoritative copy.  If not (i.e.
 *    a newer copy exists elsewhere), mark it DEAD.
 *
 *  Two-pass in-place variant (no malloc):
 *    - Scan sectors in sector-index order.
 *    - Within each sector scan records forward.
 *    - For each VALID record, call find_latest.  The common case
 *      (one copy per key) resolves in O(1) per record because the
 *      found address == current address.
 *
 *  Optimised two-pass dedup (newest-first scan):
 *    Pass A — Sort sectors by create_seq descending.
 *    Pass B — Scan sectors newest→oldest with a bounded hash set.
 *    The first sighting of any key is the newest copy (keep);
 *    subsequent sightings are stale (mark DEAD).
 *    If the hash set overflows, falls back to find_latest() per record.
 *
 *  Complexity: O(N) typical, O(N·log S) worst case with S ≤ 255.
 * ═══════════════════════════════════════════════════════════════════════════ */

/** @brief Number of hash slots in the dedup table.
 *  32 slots (384 B stack) covers most embedded key counts.
 *  Overflow gracefully falls back to find_latest(). */
#define RDB_DEDUP_SLOTS 32u

/** @brief Return values for dedup_track(). */
#define RDB_DEDUP_NEW   0   /**< First sighting — key was inserted       */
#define RDB_DEDUP_SEEN  1   /**< Already tracked — duplicate key         */
#define RDB_DEDUP_FULL (-1) /**< Hash set overflow — fall back to scan   */

typedef struct {
    uint16_t hash;         /**< Full key hash for collision detection     */
    uint8_t  klen;         /**< Key length for fast mismatch rejection    */
    uint8_t  prefix[8];    /**< First 8 key bytes for disambiguation      */
    uint32_t addr;         /**< Address of tracked newest record          */
    uint8_t  occupied;     /**< 1 if this slot is in use                  */
} rdb_dedup_slot_t;

/** @brief Bounded hash set for key deduplication (stack-allocated). */
typedef struct {
    rdb_dedup_slot_t slots[RDB_DEDUP_SLOTS];
    int              overflow; /**< Set to 1 when table is full           */
} rdb_dedup_set_t;

/** @brief Initialise (clear) the dedup hash set. */
static void dedup_init(rdb_dedup_set_t* ds) {
    memset(ds->slots, 0, sizeof(ds->slots));
    ds->overflow = 0;
}

/**
 * @brief Track a key in the dedup set, reporting whether it was already seen.
 *
 * On first encounter the key fingerprint is inserted and RDB_DEDUP_NEW
 * is returned.  Subsequent encounters of the same key return RDB_DEDUP_SEEN
 * so the caller can mark the older copy stale.  If the hash table fills up
 * RDB_DEDUP_FULL is returned and the caller must fall back to find_latest().
 *
 * @param ds    Dedup set to query / insert into.
 * @param hash  16-bit key hash.
 * @param key   Raw key bytes read from flash.
 * @param klen  Key length in bytes.
 * @return      RDB_DEDUP_NEW, RDB_DEDUP_SEEN, or RDB_DEDUP_FULL.
 */
static int dedup_track(rdb_dedup_set_t* ds, rdb_kvdb_t* db,
    uint16_t hash, const uint8_t* key, uint8_t klen, uint32_t addr) {
    if (ds->overflow)
        return RDB_DEDUP_FULL;

    uint8_t idx = (uint8_t)(hash % RDB_DEDUP_SLOTS);
    uint8_t start = idx;

    do {
        rdb_dedup_slot_t* sl = &ds->slots[idx];
        if (!sl->occupied) {
            sl->hash = hash;
            sl->klen = klen;
            {
                uint8_t pl = RDB_MIN(klen, (uint8_t)sizeof(sl->prefix));
                memcpy(sl->prefix, key, pl);
            }
            sl->addr = addr;
            sl->occupied = 1;
            return RDB_DEDUP_NEW;
        }
        /* Fast collision check: hash + length + prefix */
        if (sl->hash == hash && sl->klen == klen) {
            uint8_t pl = RDB_MIN(klen, (uint8_t)sizeof(sl->prefix));
            if (memcmp(sl->prefix, key, pl) == 0) {
                uint8_t old_key[RDB_MAX_KEY_LEN];
                if (fl_read(db, sl->addr + KV_REC_SZ, old_key, klen) != 0)
                    return RDB_DEDUP_FULL;
                if (memcmp(old_key, key, klen) == 0)
                    return RDB_DEDUP_SEEN;
            }
        }
        idx = (uint8_t)((idx + 1u) % RDB_DEDUP_SLOTS);
    } while (idx != start);

    ds->overflow = 1;
    return RDB_DEDUP_FULL;
}

/* ═══════════════════════════════════════════════════════════════════════════
 *  KV Cache — key-to-address lookup cache
 *
 *  Based on the same fingerprint scheme as rdb_dedup_set_t.
 *  Eliminates O(N x M) full-table scans for repeated get/set on the same
 *  keys.  Disabled at compile time when RDB_KV_CACHE_SIZE == 0.
 * ═══════════════════════════════════════════════════════════════════════════ */

#if RDB_KV_CACHE_SIZE > 0

/** @brief Compute a slot index from a key hash. */
static inline uint8_t kv_cache_idx(uint16_t hash) {
    return (uint8_t)(hash % RDB_KV_CACHE_SIZE);
}

/**
 * @brief Look up a key in the cache.
 *
 * Linear-probes from the home slot.  Returns the flash address on
 * match, or RDB_ADDR_INVALID on miss / full table / disabled cache.
 */
/** @brief Look up a key in the cache.
 *
 * On hit the slot's CLOCK referenced bit (MSB of klen) is set so the
 * CLOCK eviction hand will not displace it on the next scan. */
static uint32_t kv_cache_lookup(rdb_kvdb_t* db, const char* key, uint8_t kl) {
    uint16_t hash = rdb_hash16(key, kl);
    uint8_t  idx = kv_cache_idx(hash);
    uint8_t  start = idx;

    do {
        rdb_kv_cache_slot_t* sl = &db->cache.slots[idx];
        if (sl->klen == 0)
            return RDB_ADDR_INVALID;
        if ((sl->klen & 0x7Fu) == kl && sl->hash == hash) {
            uint8_t pl = RDB_MIN(kl, (uint8_t)sizeof(sl->prefix));
            if (memcmp(sl->prefix, key, pl) == 0) {
                sl->klen |= 0x80u;  /* set CLOCK referenced bit */
                return sl->addr;
            }
        }
        idx = (uint8_t)((idx + 1u) % RDB_KV_CACHE_SIZE);
    } while (idx != start);

    return RDB_ADDR_INVALID;
}

/**
 * @brief Insert (or update) a cache entry for a key.
 *
 * If an existing entry with the same fingerprint is found it is
 * updated in-place.  When the home bucket and its linear-probe chain
 * are all occupied the CLOCK (Second-Chance) eviction hand scans the
 * table for a slot whose referenced bit is clear, clearing bits as it
 * passes.  This approximates LRU with minimal per-slot state.
 */
static void kv_cache_insert(rdb_kvdb_t* db, const char* key, uint8_t kl,
    uint32_t addr) {
    uint16_t hash = rdb_hash16(key, kl);
    uint8_t  idx = kv_cache_idx(hash);
    uint8_t  start = idx;

    /* Phase 1: probe home chain for empty slot or matching entry */
    do {
        rdb_kv_cache_slot_t* sl = &db->cache.slots[idx];
        if (sl->klen == 0) {
            sl->hash = hash;
            sl->klen = kl | 0x80u;  /* store actual length + set CLOCK bit */
            {
                uint8_t pl = RDB_MIN(kl, (uint8_t)sizeof(sl->prefix));
                memcpy(sl->prefix, key, pl);
            }
            sl->addr = addr;
            return;
        }
        if ((sl->klen & 0x7Fu) == kl && sl->hash == hash) {
            uint8_t pl = RDB_MIN(kl, (uint8_t)sizeof(sl->prefix));
            if (memcmp(sl->prefix, key, pl) == 0) {
                sl->addr = addr;  /* update address in-place */
                sl->klen |= 0x80u;  /* set CLOCK bit */
                return;
            }
        }
        idx = (uint8_t)((idx + 1u) % RDB_KV_CACHE_SIZE);
    } while (idx != start);

    /* Phase 2: home chain full — CLOCK eviction scan */
    {
        uint8_t n = RDB_KV_CACHE_SIZE;
        while (n--) {
            rdb_kv_cache_slot_t* sl = &db->cache.slots[db->cache.clock_hand];
            if (sl->klen == 0) {
                idx = db->cache.clock_hand;
                goto fill;
            }
            if (sl->klen & 0x80u) {
                sl->klen &= 0x7Fu;  /* clear referenced bit, give second chance */
            } else {
                idx = db->cache.clock_hand;  /* unreferenced — evict */
                goto fill;
            }
            db->cache.clock_hand = (uint8_t)((db->cache.clock_hand + 1u) % RDB_KV_CACHE_SIZE);
        }
        /* All slots referenced — evict at current hand position */
        idx = db->cache.clock_hand;
    }

fill:
    {
        rdb_kv_cache_slot_t* sl = &db->cache.slots[idx];
        sl->hash = hash;
        sl->klen = kl | 0x80u;
        {
            uint8_t pl = RDB_MIN(kl, (uint8_t)sizeof(sl->prefix));
            memcpy(sl->prefix, key, pl);
        }
        sl->addr = addr;
        db->cache.clock_hand = (uint8_t)((idx + 1u) % RDB_KV_CACHE_SIZE);
    }
}

/**
 * @brief Invalidate (remove) a cache entry for a key.
 *
 * Sets the slot's klen to 0, marking it as empty.  This is safe to
 * call even if the key is not in the cache.
 */
static void kv_cache_invalidate(rdb_kvdb_t* db, const char* key, uint8_t kl) {
    uint16_t hash = rdb_hash16(key, kl);
    uint8_t  idx = kv_cache_idx(hash);
    uint8_t  start = idx;

    do {
        rdb_kv_cache_slot_t* sl = &db->cache.slots[idx];
        if (sl->klen == 0)
            return;
        /* klen may have CLOCK bit (0x80) set — mask it out for comparison */
        if ((sl->klen & 0x7Fu) == kl && sl->hash == hash) {
            uint8_t pl = RDB_MIN(kl, (uint8_t)sizeof(sl->prefix));
            if (memcmp(sl->prefix, key, pl) == 0) {
                sl->klen = 0;  /* invalidate */
                return;
            }
        }
        idx = (uint8_t)((idx + 1u) % RDB_KV_CACHE_SIZE);
    } while (idx != start);
}

/** @brief Reset all cache entries. */
static void kv_cache_flush(rdb_kvdb_t* db) {
    memset(&db->cache, 0, sizeof(db->cache));
}

#else  /* RDB_KV_CACHE_SIZE == 0 — compile out all cache calls */

#define kv_cache_lookup(db, key, kl)   RDB_ADDR_INVALID
#define kv_cache_insert(db, key, kl, addr)  ((void)0)
#define kv_cache_invalidate(db, key, kl)     ((void)0)
#define kv_cache_flush(db)                   ((void)0)

#endif /* RDB_KV_CACHE_SIZE */

/**
 * @brief Scan callback for fixup_stale (newest-first dedup pass).
 *
 * For each VALID record, checks the hash set via dedup_track().
 * The first sighting of a key is kept; subsequent sightings in older
 * sectors are marked DEAD.  On hash-set overflow falls back to
 * find_latest() for authoritative comparison.
 */
typedef struct {
    rdb_dedup_set_t* ds;       /**< Shared dedup hash set                */
    uint32_t         dead_count;/**< Number of records marked DEAD        */
    uint32_t         fallback;  /**< Number of fallback find_latest calls */
} fixup_ctx_t;

static int fixup_cb(rdb_kvdb_t* db, uint8_t s,
    const kv_rec_info_t* ri, void* arg) {
    fixup_ctx_t* fx = (fixup_ctx_t*)arg;
    if (ri->state != RDB_STATE_VALID)
        return RDB_ITER_CONTINUE;

    uint8_t kb[RDB_MAX_KEY_LEN];
    if (fl_read(db, ri->addr + KV_REC_SZ, kb, ri->key_len) != 0)
        return RDB_ITER_CONTINUE;

    int r = dedup_track(fx->ds, db, ri->key_hash, kb, ri->key_len, ri->addr);
    if (r == RDB_DEDUP_NEW)
        return RDB_ITER_CONTINUE;

    if (r == RDB_DEDUP_SEEN) {
        /* Already tracked in a newer sector — mark stale copy DEAD */
        if (kv_mark_dead(db, ri->addr) != 0)
            db->stats.flash_errors++;
        db->sectors[s].garbage_bytes += ri->rsz;
        kv_cache_invalidate(db, (const char*)kb, ri->key_len);
        fx->dead_count++;
        return RDB_ITER_CONTINUE;
    }

    /* RDB_DEDUP_FULL — fall back to authoritative find_latest */
    fx->fallback++;
    {
        find_ctx_t fc;
        find_latest(db, (const char*)kb, ri->key_len, &fc);
        if (fc.found && fc.best_addr != ri->addr) {
            if (kv_mark_dead(db, ri->addr) != 0)
                db->stats.flash_errors++;
            db->sectors[s].garbage_bytes += ri->rsz;
            kv_cache_invalidate(db, (const char*)kb, ri->key_len);
            fx->dead_count++;
        }
    }
    return RDB_ITER_CONTINUE;
}

/** @brief Run dedup pass: mark duplicates DEAD in newest→oldest sector order. */
static void fixup_stale(rdb_kvdb_t* db) {
    /* ── Gather non-erased sectors and sort by create_seq descending ── */
    uint8_t order[RDB_MAX_SECTORS];
    uint8_t cnt = 0;
    for (uint8_t s = 0; s < db->sector_cnt; s++) {
        if (db->sectors[s].status != RDB_SEC_ERASED &&
            db->sectors[s].status != RDB_SEC_CORRUPT)
            order[cnt++] = s;
    }
    if (cnt == 0) return;

    /* Insertion sort — descending by create_seq (newest first).
       N ≤ 255, O(N²) worst case but tiny constant and cache-friendly. */
    for (uint8_t i = 1; i < cnt; i++) {
        uint8_t key = order[i];
        uint8_t j = i;
        while (j > 0 && RDB_SEQ_GT(db->sectors[order[j - 1]].create_seq,
                                     db->sectors[key].create_seq)) {
            order[j] = order[j - 1];
            j--;
        }
        order[j] = key;
    }
    /* order[0] = oldest, order[cnt-1] = newest */

    /* ── Scan newest → oldest with bounded hash set ── */
    rdb_dedup_set_t ds;
    dedup_init(&ds);
    fixup_ctx_t fx = { .ds = &ds, .dead_count = 0, .fallback = 0 };

    int16_t i = (int16_t)(cnt - 1);
    for (; i >= 0; i--) {
        scan_sector(db, order[(uint16_t)i], fixup_cb, &fx, RDB_FALSE);
    }
}

#if RDB_BLOOM_BITS > 0
/**
 * @brief Scan callback — set bloom bits for each VALID record.
 *
 * Used to rebuild per-sector bloom filters after init or GC.
 */
static int bloom_build_cb(rdb_kvdb_t* db, uint8_t s,
    const kv_rec_info_t* ri, void* arg) {
    (void)db;
    (void)s;
    if (ri->state != RDB_STATE_VALID)
        return RDB_ITER_CONTINUE;
    uint8_t kb[RDB_MAX_KEY_LEN];
    if (fl_read(db, ri->addr + KV_REC_SZ, kb, ri->key_len) != 0)
        return RDB_ITER_CONTINUE;
    uint16_t h = rdb_hash16(kb, ri->key_len);
    uint8_t* bm = (uint8_t*)arg;
    RDB_BLOOM_SET(bm, h);
    return RDB_ITER_CONTINUE;
}

/** @brief Rebuild bloom filter for a single sector from scratch. */
static void bloom_rebuild_sec(rdb_kvdb_t* db, uint8_t s) {
    uint8_t* bm = db->blooms + (size_t)s * RDB_BLOOM_BYTES;
    memset(bm, 0, RDB_BLOOM_BYTES);
    scan_sector(db, s, bloom_build_cb, bm, RDB_FALSE);
}

/** @brief Rebuild bloom filters for all non-erased, non-corrupt sectors. */
static void bloom_rebuild_all(rdb_kvdb_t* db) {
    for (uint8_t s = 0; s < db->sector_cnt; s++) {
        if (db->sectors[s].status == RDB_SEC_ERASED ||
            db->sectors[s].status == RDB_SEC_CORRUPT)
            continue;
        bloom_rebuild_sec(db, s);
    }
}
#endif /* RDB_BLOOM_BITS > 0 */

/* ═══════════════════════════════════════════════════════════════════════════
 *  Post-GC dedup marker (mark-only, no garbage accounting)
 *
 *  Used during gc_execute cleanup: marks stale duplicates DEAD but does
 *  NOT update garbage_bytes.  The caller recalculates garbage separately
 *  via recalc_garbage() to avoid double-counting against the just-erased
 *  victim sector whose garbage was already zeroed.
 *
 *  Uses the same hash-set strategy as fixup_stale().
 * ═══════════════════════════════════════════════════════════════════════════ */

static int dedup_mark_cb(rdb_kvdb_t* db, uint8_t s,
    const kv_rec_info_t* ri, void* arg) {
    (void)s;
    rdb_dedup_set_t* ds = (rdb_dedup_set_t*)arg;
    if (ri->state != RDB_STATE_VALID)
        return RDB_ITER_CONTINUE;

    uint8_t kb[RDB_MAX_KEY_LEN];
    if (fl_read(db, ri->addr + KV_REC_SZ, kb, ri->key_len) != 0)
        return RDB_ITER_CONTINUE;

    int r = dedup_track(ds, db, ri->key_hash, kb, ri->key_len, ri->addr);
    if (r == RDB_DEDUP_SEEN) {
        if (kv_mark_dead(db, ri->addr) != 0)
            db->stats.flash_errors++;
        kv_cache_invalidate(db, (const char*)kb, ri->key_len);
    } else if (r == RDB_DEDUP_FULL) {
        /* Hash set full — fall back to authoritative find_latest */
        find_ctx_t fc;
        find_latest(db, (const char*)kb, ri->key_len, &fc);
        if (fc.found && fc.best_addr != ri->addr) {
            if (kv_mark_dead(db, ri->addr) != 0)
                db->stats.flash_errors++;
            kv_cache_invalidate(db, (const char*)kb, ri->key_len);
        }
    }
    /* RDB_DEDUP_NEW: first encounter — keep this copy */
    return RDB_ITER_CONTINUE;
}

/* ═══════════════════════════════════════════════════════════════════════════
 *  Recalculate garbage bytes for one sector
 *
 *  Scans all records in sector `s` and sums the size of every
 *  non-VALID, non-WRITING record.  The result replaces the cached
 *  garbage_bytes value in the sector metadata.
 * ═══════════════════════════════════════════════════════════════════════════ */

static int garbage_cb(rdb_kvdb_t* db, uint8_t s,
    const kv_rec_info_t* ri, void* arg) {
    (void)db;
    (void)s;
    uint32_t* gb = (uint32_t*)arg;
    if (ri->state != RDB_STATE_VALID && ri->state != RDB_STATE_WRITING)
        *gb += ri->rsz;
    return RDB_ITER_CONTINUE;
}

static void recalc_garbage(rdb_kvdb_t* db, uint8_t s) {
    uint32_t gb = 0;
    scan_sector(db, s, garbage_cb, &gb, RDB_FALSE);
    db->sectors[s].garbage_bytes = gb;
}

/** @brief Recalculate garbage for ALL non-erased, non-corrupt sectors. */
static void recalc_garbage_all(rdb_kvdb_t* db) {
    for (uint8_t s = 0; s < db->sector_cnt; s++) {
        if (db->sectors[s].status == RDB_SEC_ERASED ||
            db->sectors[s].status == RDB_SEC_CORRUPT)
            continue;
        recalc_garbage(db, s);
    }
}

/* ═══════════════════════════════════════════════════════════════════════════
 *  Reconcile live_bytes
 *
 *  Full re-scan of all sectors to compute the exact total live data
 *  (sum of record sizes for all VALID records).  This corrects any
 *  accumulated drift from bookkeeping errors or interrupted operations.
 * ═══════════════════════════════════════════════════════════════════════════ */

static int live_cb(rdb_kvdb_t* db, uint8_t s,
    const kv_rec_info_t* ri, void* arg) {
    (void)db;
    (void)s;
    if (ri->state == RDB_STATE_VALID)
        *(uint32_t*)arg += ri->rsz;
    return RDB_ITER_CONTINUE;
}

static void reconcile_live(rdb_kvdb_t* db) {
    uint32_t total = 0;
    for (uint8_t s = 0; s < db->sector_cnt; s++) {
        if (db->sectors[s].status == RDB_SEC_ERASED ||
            db->sectors[s].status == RDB_SEC_CORRUPT)
            continue;
        scan_sector(db, s, live_cb, &total, RDB_FALSE);
    }
    db->live_bytes = total;
}

/* ═══════════════════════════════════════════════════════════════════════════
 *  Init sector (erase + write header)
 *
 *  Erases sector `s`, increments its erase count, assigns a new
 *  creation sequence number, writes the sector header, and updates
 *  the in-RAM metadata to ACTIVE state.
 *
 *  @param db  Database handle.
 *  @param s   Sector index to initialise.
 *  @return    0 on success, -1 on flash failure.
 * ═══════════════════════════════════════════════════════════════════════════ */

static int init_sector(rdb_kvdb_t* db, uint8_t s) {
    /* [K-EC-PERSIST fix]: check flash header for a higher erase count
     * before erasing, preventing ec regression after RAM loss. */
    uint32_t ec = db->sectors[s].erase_cnt;
    {
        rdb_kv_sector_hdr_t oh;
        if (fl_read(db, sec_addr(db, s), &oh, sizeof(oh)) == 0 &&
            oh.magic == RDB_KV_SECTOR_MAGIC &&
            rdb_crc16(&oh, 6) == oh.hdr_crc &&
            oh.erase_cnt > ec)
            ec = oh.erase_cnt;
    }
    ec++;

    if (fl_erase(db, sec_addr(db, s)) != 0)
        return -1;

    db->write_seq++;
    if (write_sec_hdr(db, s, ec, db->write_seq) != 0)
        return -1;

    db->sectors[s].erase_cnt = ec;
    db->sectors[s].create_seq = db->write_seq;
    db->sectors[s].garbage_bytes = 0;
    db->sectors[s].write_off = data_start(db);
    db->sectors[s].status = RDB_SEC_ACTIVE;
#if RDB_BLOOM_BITS > 0
    /* Freshly erased sector has no keys — clear bloom */
    if (db->blooms)
        memset(db->blooms + (size_t)s * RDB_BLOOM_BYTES, 0, RDB_BLOOM_BYTES);
#endif
    return 0;
}

/* ═══════════════════════════════════════════════════════════════════════════
 *  [K-7 fix] Rotate to a new active sector
 *
 *  Selects the erased sector with the lowest erase count (wear-leveling
 *  friendly), initialises it as the new active sector, and seals the
 *  previous active sector.
 *
 *  Original bug (pre-K-7): if the old active sector had write_off ==
 *  data_start (no records written), it was left as ACTIVE, creating a
 *  phantom sector that could never be reclaimed by GC.
 *
 *  Fix: unconditionally seal the old active sector.  An empty SEALED
 *  sector has zero live bytes, so GC Phase 1 (zero-live fast reclaim)
 *  will reclaim it on the next GC pass with no migration cost.
 *
 *  @param db  Database handle.
 *  @return    0 on success, -1 if no erased sector is available.
 * ═══════════════════════════════════════════════════════════════════════════ */

static int rotate(rdb_kvdb_t* db) {
    /* Find erased sector with lowest erase count */
    uint8_t  best = 0xFF;
    uint32_t best_ec = 0xFFFFFFFFu;
    for (uint8_t s = 0; s < db->sector_cnt; s++) {
        if (db->sectors[s].status == RDB_SEC_ERASED &&
            db->sectors[s].erase_cnt < best_ec) {
            best = s;
            best_ec = db->sectors[s].erase_cnt;
        }
    }
    if (best == 0xFF)
        return -1; /* No erased sector available */

    if (init_sector(db, best) != 0)
        return -1;

    /* [K-7 fix]: unconditionally seal old active — no write_off guard */
    if (db->active_sec < db->sector_cnt &&
        db->sectors[db->active_sec].status == RDB_SEC_ACTIVE)
        db->sectors[db->active_sec].status = RDB_SEC_SEALED;

    db->active_sec = best;
    db->write_off = db->sectors[best].write_off;
    return 0;
}

/* ═══════════════════════════════════════════════════════════════════════════
 *  GC subsystem
 *
 *  The garbage collector uses a four-phase victim selection strategy:
 *
 *  Phase 1 — Zero-live fast reclaim:
 *    Sectors with no live records (all DEAD/corrupt) can be reclaimed
 *    instantly with no data migration.  Prefer the lowest erase count.
 *
 *  Phase 2 — Scored selection:
 *    For sectors exceeding a garbage percentage threshold, compute a
 *    composite score: garbage% × 7 + wear% × 3 + capacity%.
 *    The sector with the highest score is chosen as the victim.
 *
 *  Phase 3 — Forced degradation:
 *    When no sector meets the scored threshold, select the sector
 *    with the most absolute garbage bytes (last resort before Phase 4).
 *
 *  Phase 4 — Static wear leveling:
 *    When erase count spread exceeds RDB_GC_WEAR_THRESHOLD, the
 *    least-worn non-erased sector is recycled even if it has zero
 *    garbage.  This redistributes wear across the flash array.
 * ═══════════════════════════════════════════════════════════════════════════ */

/** @brief Per-sector GC candidate cache (computed once per GC round). */
typedef struct {
    uint32_t garbage; /**< Cached garbage bytes                   */
    uint32_t live;    /**< Estimated live bytes (used − garbage)  */
    uint32_t ec;      /**< Erase count                            */
    uint8_t  status;  /**< Sector status                          */
    uint8_t  idx;     /**< Sector index (for reference)           */
} gc_prep_t;

/**
 * @brief Build the GC candidate cache from current sector metadata.
 *
 * @param db  Database handle.
 * @param c   Array of gc_prep_t, one per sector.
 */
static void gc_build_cache(rdb_kvdb_t* db, gc_prep_t* c) {
    uint32_t ds = data_start(db);
    for (uint8_t s = 0; s < db->sector_cnt; s++) {
        c[s].garbage = db->sectors[s].garbage_bytes;
        c[s].ec = db->sectors[s].erase_cnt;
        c[s].status = db->sectors[s].status;
        c[s].idx = s;
        c[s].live = 0;

        if (c[s].status != RDB_SEC_ERASED &&
            c[s].status != RDB_SEC_CORRUPT) {
            uint32_t used = db->sectors[s].write_off - ds;
            c[s].live = (used > c[s].garbage) ? used - c[s].garbage : 0;
        }
        if ((s & 0x0Fu) == 0x0Fu)
            fl_yield(db);
    }
}

/**
 * @brief Calculate available space for migrating records from a victim sector.
 *
 * Counts free space in the current active sector (excluding the victim)
 * plus full capacity of all other erased sectors.
 *
 * @param db      Database handle.
 * @param victim  Sector index being considered for GC.
 * @return        Total available bytes for migration.
 */
static uint32_t gc_avail(rdb_kvdb_t* db, uint8_t victim) {
    uint32_t avail = 0;

    /* Remaining space in current active sector */
    if (db->active_sec < db->sector_cnt && db->active_sec != victim)
        avail += db->part->sector_size - db->sectors[db->active_sec].write_off;

    /* Full capacity of all other erased sectors */
    for (uint8_t s = 0; s < db->sector_cnt; s++) {
        if (s == victim || s == db->active_sec)
            continue;
        if (db->sectors[s].status == RDB_SEC_ERASED)
            avail += data_cap(db);
    }
    return avail;
}

/* ── Migration: copy one live record to the active sector ──────────────── */

/**
 * @brief Migrate a single VALID record from a GC victim sector to the
 *        current active sector.
 *
 * The record is written with a new sequence number (db->write_seq++)
 * in WRITING state, then committed to VALID.  If the active sector
 * is full, rotate() is called to open a new one.
 *
 * @param db             Database handle.
 * @param src_sec        Source sector index (for reference only).
 * @param ri             Parsed record info from the victim sector.
 * @param orig_data_crc  Original data CRC (re-used, not recomputed).
 * @return               0 on success, -1 on failure.
 */
static int migrate_one(rdb_kvdb_t* db, uint8_t src_sec,
    const kv_rec_info_t* ri,
    uint16_t             orig_data_crc,
    uint32_t*            out_addr) {
    (void)src_sec; /* reserved for future use (e.g. cross-sector trace) */
    uint32_t rsz = ri->rsz;
    uint32_t g = wr_gran(db);
    uint32_t ka = RDB_ALIGN_UP(ri->key_len, g);
    uint32_t va = RDB_ALIGN_UP(ri->val_len, g);

    /* Rotate if not enough room in current active sector */
    if (db->sectors[db->active_sec].write_off + rsz > db->part->sector_size) {
        if (rotate(db) != 0)
            return -1;
    }

    uint32_t dst = sec_addr(db, db->active_sec) +
                   db->sectors[db->active_sec].write_off;

    /* Assign new sequence number */
    db->write_seq++;
    rdb_kv_record_hdr_t nh;
    nh.magic = RDB_KV_RECORD_MAGIC;
    nh.state = RDB_STATE_WRITING;
    nh.key_len = ri->key_len;
    nh._pad0 = 0xFF;
    nh.val_len = ri->val_len;
    nh.key_hash = ri->key_hash;
    nh.seq = db->write_seq;
    nh.data_crc = orig_data_crc;
    nh._pad1 = 0xFFFF;

    if (rsz <= RDB_STACK_BUF_SIZE) {
        /* Small record: assemble header + key + value in one stack
           buffer and write as a single flash operation (same merge
           optimisation as rdb_kvdb_set). */
        uint8_t mbuf[RDB_STACK_BUF_SIZE];
        memcpy(mbuf, &nh, KV_REC_SZ);

        /* Read key + value payload from source into the merge buffer */
        uint32_t total = ka + va;
        if (fl_read(db, ri->addr + KV_REC_SZ, mbuf + KV_REC_SZ, total) != 0)
            return -1;

        if (fl_write(db, dst, mbuf, rsz) != 0)
            return -1;

        /* Commit: WRITING → VALID */
        uint8_t v = RDB_STATE_VALID;
        if (fl_write(db, dst + 1, &v, 1) != 0) {
            db->sectors[db->active_sec].write_off += rsz;
            return -1;
        }
    } else {
        /* Large record: multi-step write */

        /* Write record header */
        if (fl_write(db, dst, &nh, sizeof(nh)) != 0)
            return -1;

        /* Copy key + value payload from source to destination */
        {
            uint32_t src_pos = ri->addr + KV_REC_SZ;
            uint32_t dst_pos = dst + KV_REC_SZ;
            uint32_t total = ka + va;
            uint8_t  buf[RDB_STACK_BUF_SIZE];

            while (total) {
                uint32_t ch = RDB_MIN(total, sizeof(buf));
                RDB_PAGE_CLAMP(ch, dst_pos);
                if (fl_read(db, src_pos, buf, ch) != 0)
                    return -1;
                if (fl_write(db, dst_pos, buf, ch) != 0)
                    return -1;
                src_pos += ch;
                dst_pos += ch;
                total -= ch;
            }
        }

        /* Commit: WRITING → VALID */
        uint8_t v = RDB_STATE_VALID;
        if (fl_write(db, dst + 1, &v, 1) != 0) {
            db->sectors[db->active_sec].write_off += rsz;
            return -1;
        }
    }

    db->sectors[db->active_sec].write_off += rsz;
    if (out_addr)
        *out_addr = dst;
    return 0;
}

/* ── GC execute — process one victim sector ────────────────────────────── */

/** @brief Context passed through the GC migration scan callback. */
typedef struct {
    rdb_kvdb_t* db;       /**< Database handle            */
    int         err;      /**< Set to 1 on fatal error    */
    uint32_t    migrated; /**< Count of migrated records  */
} gc_exec_ctx_t;

/**
 * @brief Scan callback for GC migration.
 *
 * For each VALID record in the victim sector:
 *   1. Verify it is the latest copy (find_latest); if not, mark DEAD.
 *   2. Verify data CRC; if bad, mark DEAD and log error.
 *   3. Migrate to active sector.
 *   4. Mark source record DEAD.
 */
static int gc_migrate_cb(rdb_kvdb_t* db, uint8_t s,
    const kv_rec_info_t* ri, void* arg) {
    gc_exec_ctx_t* gc = (gc_exec_ctx_t*)arg;

    if (ri->state != RDB_STATE_VALID)
        return RDB_ITER_CONTINUE;

    /* Read key to identify the record */
    uint8_t kb[RDB_MAX_KEY_LEN];
    if (fl_read(db, ri->addr + KV_REC_SZ, kb, ri->key_len) != 0) {
        gc->err = 1;
        return RDB_ITER_STOP;
    }

    /* Check if this is still the latest copy */
    find_ctx_t fc;
    find_latest(db, (const char*)kb, ri->key_len, &fc);

    if (!fc.found || fc.best_addr != ri->addr) {
        /* Stale duplicate discovered during GC — mark dead */
        if (kv_mark_dead(db, ri->addr) != 0)
            db->stats.flash_errors++;
        return RDB_ITER_CONTINUE;
    }

    /* Verify data integrity before migration */
    rdb_kv_record_hdr_t rh;
    if (fl_read(db, ri->addr, &rh, sizeof(rh)) != 0) {
        gc->err = 1;
        return RDB_ITER_STOP;
    }

    uint16_t calc;
    if (data_crc_flash(db, ri->addr, ri->key_len, ri->val_len, &calc) != 0) {
        db->stats.flash_errors++;
        if (kv_mark_dead(db, ri->addr) != 0)
            db->stats.flash_errors++;
        return RDB_ITER_CONTINUE;
    }

    if (calc != rh.data_crc) {
        db->stats.crc_errors++;
        if (kv_mark_dead(db, ri->addr) != 0)
            db->stats.flash_errors++;
        return RDB_ITER_CONTINUE;
    }

    /* Migrate the record to the active sector */
    uint32_t new_addr = RDB_ADDR_INVALID;
    if (migrate_one(db, s, ri, rh.data_crc, &new_addr) != 0) {
        gc->err = 1;
        return RDB_ITER_STOP;
    }

    /* Cache the record at its new address; invalidate the old entry.
       The key fingerprint is already in kb from the flash read above. */
    kv_cache_invalidate(db, (const char*)kb, ri->key_len);
    kv_cache_insert(db, (const char*)kb, ri->key_len, new_addr);

#if RDB_BLOOM_BITS > 0
    /* Set bloom bits in the active sector for the migrated key */
    if (db->blooms) {
        uint8_t* bm = db->blooms + (size_t)db->active_sec * RDB_BLOOM_BYTES;
        RDB_BLOOM_SET(bm, rdb_hash16(kb, ri->key_len));
    }
#endif

    /* Mark source record dead after successful migration.
       If mark_dead fails, retry once after a yield.  A persistent
       failure produces two VALID copies, which is self-healing:
       the next GC pass finds the old copy stale (new copy has
       higher seq) and re-attempts mark_dead. */
    if (kv_mark_dead(db, ri->addr) != 0) {
        fl_yield(db);
        if (kv_mark_dead(db, ri->addr) != 0) {
            db->stats.flash_errors++;
            return RDB_ITER_CONTINUE;
        }
    }

    gc->migrated++;
    db->stats.gc_migrated_recs++;

    fl_yield(db); /* Yield CPU between migrations */

    return RDB_ITER_CONTINUE;
}

/**
 * @brief Execute garbage collection on a single victim sector.
 *
 * Steps:
 *   1. Verify that available space can hold all live data from the victim.
 *   2. Migrate all live records to the active sector.
 *   3. Erase the victim sector and mark it as ERASED.
 *   4. If any records were migrated, run fixup + garbage recalc on
 *      remaining sectors to eliminate any new stale duplicates.
 *
 * @param db      Database handle.
 * @param victim  Sector index to reclaim.
 * @return        0 on success, -1 on failure.
 */
static int gc_execute(rdb_kvdb_t* db, uint8_t victim) {
    db->stats.gc_runs++;

    /* Pre-check: ensure migration target has enough space */
    {
        uint32_t used = db->sectors[victim].write_off - data_start(db);
        uint32_t real_live = (used > db->sectors[victim].garbage_bytes) ? used - db->sectors[victim].garbage_bytes : 0;
        if (real_live > gc_avail(db, victim))
            return -1;
    }

    /* Migrate all live records from the victim.
       gc_migrate_cb sets gc.err=1 on any migration failure (flash error,
       CRC mismatch, etc.).  The check below prevents the victim sector
       from being erased when a live record could not be safely moved,
       preserving data at the cost of a failed GC cycle. */
    gc_exec_ctx_t gc = {.db = db, .err = 0, .migrated = 0};
    scan_sector(db, victim, gc_migrate_cb, &gc, RDB_FALSE);
    if (gc.err)
        return -1;

    /* Erase the victim sector */
    uint32_t ec = db->sectors[victim].erase_cnt + 1;
    if (fl_erase(db, sec_addr(db, victim)) != 0)
        return -1;

    /* Update victim metadata */
    db->sectors[victim].erase_cnt = ec;
    db->sectors[victim].create_seq = 0;
    db->sectors[victim].garbage_bytes = 0;
    db->sectors[victim].write_off = data_start(db);
    db->sectors[victim].status = RDB_SEC_ERASED;
#if RDB_BLOOM_BITS > 0
    if (db->blooms)
        memset(db->blooms + (size_t)victim * RDB_BLOOM_BYTES, 0, RDB_BLOOM_BYTES);
#endif

    db->stats.gc_reclaimed_bytes += data_cap(db);

    fl_yield(db);

    /* Invalidate live iterators — victim sector has been erased */
    db->iter_gen++;

    /* Post-migration cleanup: dedup stale duplicates + recalc garbage */
    if (gc.migrated > 0) {

        /* Collect active sectors and sort by descending create_seq
           so the newest copy is the "first sighting" preserved by
           dedup_mark_cb, matching fixup_stale's ordering invariant. */
        uint8_t order[RDB_MAX_SECTORS];
        uint8_t cnt = 0;
        for (uint8_t s = 0; s < db->sector_cnt; s++) {
            if (s == victim)
                continue;
            if (db->sectors[s].status == RDB_SEC_ERASED ||
                db->sectors[s].status == RDB_SEC_CORRUPT)
                continue;
            order[cnt++] = s;
        }
        /* Sort descending by create_seq (newest first) */
        for (int j = 1; j < cnt; j++) {
            uint8_t key = order[j];
            int k = j - 1;
            while (k >= 0 && RDB_SEQ_GT(db->sectors[key].create_seq,
                                        db->sectors[order[k]].create_seq)) {
                order[k + 1] = order[k];
                k--;
            }
            order[k + 1] = key;
        }

        rdb_dedup_set_t ds;
        dedup_init(&ds);
        for (uint8_t s = 0; s < cnt; s++) {
            scan_sector(db, order[s], dedup_mark_cb, &ds, RDB_FALSE);
            recalc_garbage(db, order[s]);
        }
        reconcile_live(db);
    }

    return 0;
}

/* ═══════════════════════════════════════════════════════════════════════════
 *  [K-1 fix] ensure_space — guarantee room for a new write
 *
 *  This function ensures that after returning RDB_OK:
 *    1. live_bytes + need ≤ max_live()   (logical capacity check)
 *    2. The active sector has room for `need` bytes, OR a rotate()
 *       can provide a fresh sector.
 *    3. At least gc_reserve erased sectors remain (GC invariant).
 *
 *  The `will_free` and `free_sec` parameters hint that the caller
 *  will invalidate an existing record of `will_free` bytes in sector
 *  `free_sec` after the write succeeds, allowing the GC scorer to
 *  factor in the upcoming garbage.
 *
 *  ┌─────────────────────────────────────────────────────────────────┐
 *  │ Four-Phase GC Algorithm (KVDB)                                  │
 *  ├─────────────────────────────────────────────────────────────────┤
 *  │                                                                 │
 *  │ **Phase 1** (Zero-Live GC):                                    │
 *  │   If any sector has 0 live data → erase immediately            │
 *  │   This is the cheapest GC (lowest wear, fastest overhead)       │
 *  │   Detection: Triggered by gc_ensure_space() when erased ≤ reserve │
 *  │                                                                 │
 *  │ **Phase 2** (Scored GC):                                       │
 *  │   Build a cache of all sectors with (live, garbage, erase_cnt) │
 *  │   Score each sector: garbage_bytes / (erase_count + 1)         │
 *  │   Pick sector with LOWEST score (highest waste ratio)          │
 *  │   Rationale: minimize wear on low-waste sectors                │
 *  │                                                                 │
 *  │ **Phase 3** (Forced GC):                                       │
 *  │   If Phase 2 exhausted (no improvements), force erase of the   │
 *  │   oldest sector (lowest erase_count) to break deadlock         │
 *  │   This ensures forward progress even in pathological cases     │
 *  │                                                                 │
 *  │ **Phase 4** (Wear-Leveling Mitigation):                        │
 *  │   If reserve fully depleted, mark oldest-erased sector as      │
 *  │   "degraded_active" (read-only) and allocate new active sector │
 *  │   This is a safety fallback; normal operation shouldn't reach  │
 *  │   this point (indicates poor capacity planning)                │
 *  │                                                                 │
 *  │ Key Invariant:                                                  │
 *  │   count_erased(db) >= gc_reserve at all times (except loop)    │
 *  │   Ensures GC can always make progress without deadlock        │
 *  │                                                                 │
 *  └─────────────────────────────────────────────────────────────────┘
 *
 *  K-1 fix detail:
 *    Original second fast-return: count_erased(db) >= gc_reserve
 *    Fixed:                       count_erased(db) >  gc_reserve
 *    Rationale: with erased == gc_reserve exactly, the current write
 *    may fill the active sector.  The subsequent rotate() would
 *    consume one erased sector, dropping below gc_reserve.  By
 *    requiring strictly greater, gc_reserve is always preserved.
 *
 *  Performance characteristics:
 *    • Best case (Phase 1): O(N) scan, erase ~4ms per sector
 *    • Typical case (Phase 2): O(N) scan + O(N) scoring, erase 4ms
 *    • Worst case (Phase 3): O(N^2) if all sectors have same score
 *    • Average overhead: ~10-50ms per ensure_space call (test verified)
 * ═══════════════════════════════════════════════════════════════════════════ */

static rdb_err_t gc_ensure_space(rdb_kvdb_t* db, uint32_t need,
    uint32_t will_free, uint8_t free_sec) {
    /* Logical capacity check: even if all garbage were reclaimed,
       would the new data fit? */
    uint32_t eff = (db->live_bytes > will_free) ? db->live_bytes - will_free : 0;
    if (need > 0 && eff + need > max_live(db))
        return RDB_ERR_FULL;

    /* Fast path 1: plenty of erased sectors (gc_reserve + 1 or more) */
    if (count_erased(db) >= (uint8_t)(db->gc_reserve + 1u))
        return RDB_OK;

    /* Fast path 2 [K-1 fix]: active sector has room AND erased > gc_reserve */
    if (need > 0 &&
        db->sectors[db->active_sec].write_off + need <= db->part->sector_size &&
        count_erased(db) > db->gc_reserve)
        return RDB_OK;

    /* ── GC loop: run up to sector_cnt rounds of victim selection ── */
    for (uint8_t round = 0; round < db->sector_cnt; round++) {
        if (count_erased(db) >= (uint8_t)(db->gc_reserve + 1u))
            break;

        gc_prep_t cache[RDB_MAX_SECTORS];
        gc_build_cache(db, cache);

        /* Adjust cache for the record that will be freed after write */
        if (will_free > 0 && free_sec < db->sector_cnt) {
            if (cache[free_sec].status == RDB_SEC_ACTIVE ||
                cache[free_sec].status == RDB_SEC_SEALED) {
                uint32_t adj = RDB_MIN(will_free, cache[free_sec].live);
                cache[free_sec].garbage += adj;
                cache[free_sec].live -= adj;
            }
        }

        /* Compute global erase-count range for wear scoring */
        uint32_t g_min_ec = 0xFFFFFFFFu, g_max_ec = 0;
        for (uint8_t s = 0; s < db->sector_cnt; s++) {
            if (cache[s].status == RDB_SEC_ERASED)
                continue;
            if (cache[s].ec < g_min_ec)
                g_min_ec = cache[s].ec;
            if (cache[s].ec > g_max_ec)
                g_max_ec = cache[s].ec;
        }
        if (g_min_ec == 0xFFFFFFFFu)
            g_min_ec = 0;
        uint32_t ec_range = (g_max_ec > g_min_ec) ? g_max_ec - g_min_ec : 1;

        uint8_t victim = 0xFF;

        /* ── Phase 1: zero-live fast reclaim ── */
        {
            uint8_t  best = 0xFF;
            uint32_t best_ec = 0xFFFFFFFFu;
            for (uint8_t s = 0; s < db->sector_cnt; s++) {
                if (s == db->active_sec)
                    continue;
                if (cache[s].status != RDB_SEC_ACTIVE &&
                    cache[s].status != RDB_SEC_SEALED)
                    continue;
                if (cache[s].live == 0 && cache[s].garbage > 0 &&
                    cache[s].ec < best_ec) {
                    best = s;
                    best_ec = cache[s].ec;
                }
            }
            victim = best;
        }

        /* ── Phase 2: scored selection ── */
        if (victim == 0xFF) {
            uint8_t erased = count_erased(db);
            uint8_t thresh;
            if (erased > db->gc_reserve)
                thresh = RDB_GC_GARBAGE_PCT;   /* Ample erased → strict threshold */
            else if (erased == db->gc_reserve)
                thresh = 5u;                    /* At margin → moderate threshold */
            else
                thresh = 1u;                    /* Low on erased → aggressive GC */

            uint8_t  best_v = 0xFF;
            uint32_t best_score = 0;

            for (uint8_t s = 0; s < db->sector_cnt; s++) {
                if (s == db->active_sec)
                    continue;
                if (cache[s].status != RDB_SEC_ACTIVE &&
                    cache[s].status != RDB_SEC_SEALED)
                    continue;
                if (cache[s].garbage == 0)
                    continue;

                uint32_t used = db->sectors[s].write_off - data_start(db);
                if (used == 0)
                    continue;

                uint32_t gpct = (cache[s].garbage * 100u) / used;
                if (gpct < thresh)
                    continue;

                if (cache[s].live > gc_avail(db, s))
                    continue;

                /* Composite score: garbage% × W_GARBAGE + wear% × W_WEAR + capacity% × W_CAPACITY
                 * Weights are configurable via RDB_GC_W_* macros. */
                uint32_t wpct = ((cache[s].ec - g_min_ec) * 100u) / ec_range;
                uint32_t cpct = (cache[s].live == 0) ? 100u : 100u - (uint32_t)RDB_MIN((cache[s].live * 100u) / data_cap(db), 100u);

                uint32_t score = gpct * RDB_GC_W_GARBAGE + wpct * RDB_GC_W_WEAR + cpct * RDB_GC_W_CAPACITY;
                if (score > best_score) {
                    best_score = score;
                    best_v = s;
                }
            }
            victim = best_v;
        }

        /* ── Phase 3: forced degradation ── */
        if (victim == 0xFF) {
            uint8_t  best = 0xFF;
            uint32_t best_gb = 0;
            for (uint8_t s = 0; s < db->sector_cnt; s++) {
                if (s == db->active_sec)
                    continue;
                if (cache[s].status != RDB_SEC_ACTIVE &&
                    cache[s].status != RDB_SEC_SEALED)
                    continue;
                if (cache[s].garbage == 0)
                    continue;
                if (cache[s].live > gc_avail(db, s))
                    continue;
                if (cache[s].garbage > best_gb) {
                    best_gb = cache[s].garbage;
                    best = s;
                }
            }
            victim = best;
        }

        /* ── Phase 4: static wear leveling ── */
        if (victim == 0xFF) {
            uint32_t all_min = 0xFFFFFFFFu, all_max = 0;
            for (uint8_t s = 0; s < db->sector_cnt; s++) {
                if (db->sectors[s].erase_cnt < all_min)
                    all_min = db->sectors[s].erase_cnt;
                if (db->sectors[s].erase_cnt > all_max)
                    all_max = db->sectors[s].erase_cnt;
            }
            if (all_min == 0xFFFFFFFFu)
                all_min = 0;

            /* Find least-worn non-erased, non-active sector */
            uint8_t  min_s = 0xFF;
            uint32_t min_ec_nea = 0xFFFFFFFFu;
            for (uint8_t s = 0; s < db->sector_cnt; s++) {
                if (db->sectors[s].status == RDB_SEC_ERASED)
                    continue;
                if (s == db->active_sec)
                    continue;
                if (db->sectors[s].erase_cnt < min_ec_nea) {
                    min_ec_nea = db->sectors[s].erase_cnt;
                    min_s = s;
                }
            }

            if (min_s != 0xFF &&
                all_max - all_min >= RDB_GC_WEAR_THRESHOLD) {
                if (cache[min_s].live <= gc_avail(db, min_s))
                    victim = min_s;
            }
        }

        if (victim == 0xFF)
            break; /* No viable victim found */
        if (gc_execute(db, victim) != 0)
            break;
    }

    /* Post-GC: check if the active sector now has room */
    if (need > 0 &&
        db->sectors[db->active_sec].write_off + need <= db->part->sector_size)
        return RDB_OK;

    /* Last resort: rotate to a fresh sector */
    if (count_erased(db) >= 1u) {
        if (rotate(db) == 0) {
            if (need == 0)
                return RDB_OK;
            if (db->sectors[db->active_sec].write_off + need <= db->part->sector_size)
                return RDB_OK;
        }
    }

    return RDB_ERR_FULL;
}

/* ═══════════════════════════════════════════════════════════════════════════
 *  Fix WRITING records during init
 *
 *  WRITING records represent writes that were interrupted by power
 *  loss between the header write and the commit (state → VALID).
 *
 *  Recovery strategy:
 *    - Compute the data CRC from flash and compare with the stored CRC.
 *    - If they match, the data payload is complete → promote to VALID.
 *    - If they don't match, the data is incomplete → demote to DEAD.
 *
 *  This is called once per sector during init Phase 1.
 * ═══════════════════════════════════════════════════════════════════════════ */

/** @brief Init scan callback — recover WRITING records and track max_seq.
 *
 *  Merges two Phase 1 passes into one:
 *    1. WRITING record recovery (CRC-based promote/demote)
 *    2. max_seq tracking for write sequence initialisation
 *
 *  @param arg  Pointer to uint32_t receiving the maximum seq seen. */
static int writing_cb(rdb_kvdb_t* db, uint8_t s,
    const kv_rec_info_t* ri, void* arg) {
    (void)s;

    /* Track max sequence number across all records */
    uint32_t* max_seq = (uint32_t*)arg;
    if (max_seq && RDB_SEQ_GT(ri->seq, *max_seq))
        *max_seq = ri->seq;

    if (ri->state != RDB_STATE_WRITING)
        return RDB_ITER_CONTINUE;

    rdb_kv_record_hdr_t rh;
    if (fl_read(db, ri->addr, &rh, sizeof(rh)) != 0) {
        if (kv_mark_dead(db, ri->addr) != 0)
            db->stats.flash_errors++;
        return RDB_ITER_CONTINUE;
    }

    /* Verify data CRC to determine if write completed successfully */
    uint16_t calc;
    if (data_crc_flash(db, ri->addr, ri->key_len, ri->val_len, &calc) != 0 ||
        calc != rh.data_crc) {
        /* Data incomplete or corrupt → mark dead */
        if (kv_mark_dead(db, ri->addr) != 0)
            db->stats.flash_errors++;
    } else {
        /* Data intact → promote to VALID */
        uint8_t v = RDB_STATE_VALID;
        if (fl_write(db, ri->addr + 1, &v, 1) != 0)
            db->stats.flash_errors++;
    }
    return RDB_ITER_CONTINUE;
}

/* ═══════════════════════════════════════════════════════════════════════════
 *                          PUBLIC API
 * ═══════════════════════════════════════════════════════════════════════════ */

/* ═══════════════════════════════════════════════════════════════════════════
 *  rdb_kvdb_init — Initialise the KVDB from existing flash contents
 *
 *  ┌──────────────────────────────────────────────────────────────────────┐
 *  │ Recovery Algorithm (Crash-Consistent Initialization)                 │
 *  ├──────────────────────────────────────────────────────────────────────┤
 *  │                                                                      │
 *  │ **Phase 1** — Scan all sectors:                                     │
 *  │   • Read all sector headers (CRC-protected)                          │
 *  │   • Classify each as: ERASED / ACTIVE / SEALED / CORRUPT            │
 *  │   • For ACTIVE sectors: scan all records, recover WRITING ones      │
 *  │   • Track max_seq for wrap-safe recovery of sequence numbers        │
 *  │   • Detect power-loss: incomplete record (missing commit marker)    │
 *  │                                                                      │
 *  │ **Phase 2** — Fixup stale records + recompute metadata:             │
 *  │   • Run fixup_stale() to mark all duplicate records DEAD            │
 *  │   • Algorithm: Reverse-scan ALL records, keep first occurrence      │
 *  │   • Recompute garbage_bytes per sector and total live_bytes         │
 *  │   • Key insight: O(N) reverse scan avoids O(N^2) forward duplicates │
 *  │   • This is the core of crash-recovery: deduplication               │
 *  │                                                                      │
 *  │ **Phase 3** — Select active sector [K-11 fix]:                      │
 *  │   • Candidate: sector with highest create_seq that has room         │
 *  │   • Wrap-safe comparison: use RDB_SEQ_GT (handles 32-bit wrap)      │
 *  │   • Seal all other non-erased sectors (prevent accidental append)   │
 *  │   • If no suitable active: allocate one from erased pool            │
 *  │   • Invariant: exactly one ACTIVE sector at end of Phase 3          │
 *  │                                                                      │
 *  │ **Phase 4** — Safety-watermark recovery:                            │
 *  │   • Check: count_erased >= gc_reserve + 1?                         │
 *  │   • If not: call gc_ensure_space(0, ...) to trigger GC                │
 *  │   • This brings system back to healthy GC state                     │
 *  │   • Handles: power-loss during GC (left system depleted)            │
 *  │                                                                      │
 *  │ **Corruption Handling** (between Phase 3 & 4):                      │
 *  │   • Any CORRUPT sectors: attempt erase to reclaim as ERASED         │
 *  │   • Erase failure: leave as CORRUPT (won't use, triggers no fault) │
 *  │   • Success: increases erased pool, aids watermark recovery         │
 *  │                                                                      │
 *  │ **Power-Loss Scenarios**:                                            │
 *  │   Case A: Crash during Phase 1                                       │
 *  │     → Re-run init(), Phase 1 is idempotent (rescan works)           │
 *  │   Case B: Crash during Phase 2 (fixup_stale)                        │
 *  │     → Re-run init(), fixup_stale results same (deterministic)       │
 *  │   Case C: Crash during Phase 3 (active selection)                   │
 *  │     → Re-run init(), picks same active (high seq persists)          │
 *  │   Case D: Crash during Phase 4 (GC)                                │
 *  │     → Re-run init(), ensure_space runs again (idempotent)           │
 *  │                                                                      │
 *  │ **Key Insight**: All phases are idempotent!                          │
 *  │   Repeated init() after power-loss converges to same final state    │
 *  │                                                                      │
 *  └──────────────────────────────────────────────────────────────────────┘
 *
 *  Boot sequence (five phases):
 *
 *  Phase 1 — Scan all sectors:
 *    Read sector headers, classify as ERASED / ACTIVE / CORRUPT.
 *    For each ACTIVE sector, recover WRITING records (CRC-based)
 *    and determine the maximum write sequence number.
 *
 *  Phase 2 — Fixup stale records + recompute metadata:
 *    Run fixup_stale() to mark all duplicate records DEAD.
 *    Recompute garbage_bytes per sector and total live_bytes.
 *
 *  Phase 3 — Select active sector [K-11 fix]:
 *    Choose the sector with the highest create_seq that still has
 *    room for new records.  Use RDB_SEQ_GT for wrap-safe comparison.
 *    Seal all other non-erased sectors.
 *
 *  Phase 4 — Safety-watermark recovery:
 *    If fewer than gc_reserve + 1 erased sectors remain, run
 *    gc_ensure_space(0, ...) to trigger GC without a specific write
 *    target, bringing the system back to a healthy state.
 *
 *  Recover CORRUPT sectors:
 *    Between Phase 3 and Phase 4, attempt to erase any CORRUPT
 *    sectors to reclaim them as ERASED, increasing available space.
 *
 *  @param db        Database handle (caller-allocated, zeroed on entry).
 *  @param part      Partition descriptor (must outlive the database).
 *  @param meta_buf  Caller-allocated buffer for sector metadata;
 *                   size = rdb_kvdb_meta_size(sector_count).
 *  @return          RDB_OK on success, or an error code.
 * ═══════════════════════════════════════════════════════════════════════════ */

rdb_err_t rdb_kvdb_init(rdb_kvdb_t* db, const rdb_partition_t* part,
    void* meta_buf) {
    /* ── Parameter validation ── */
    if (!db || !part || !meta_buf || !part->ops)
        return RDB_ERR_PARAM;
    if (!part->ops->read || !part->ops->write || !part->ops->erase)
        return RDB_ERR_PARAM;
    if (part->sector_size < RDB_MIN_SECTOR_SIZE)
        return RDB_ERR_PARAM;
    if (part->sector_size & (part->sector_size - 1))
        return RDB_ERR_PARAM;
    if (part->total_size % part->sector_size)
        return RDB_ERR_PARAM;
    if (part->write_gran > RDB_WRITE_GRAN_MAX)
        return RDB_ERR_PARAM;

    uint32_t scnt = part->total_size / part->sector_size;
    if (scnt < RDB_KV_MIN_SECTORS || scnt > RDB_MAX_SECTORS)
        return RDB_ERR_PARAM;

    /* ── Zero-initialise the handle ── */
    memset(db, 0, sizeof(*db));
    db->part = part;
    db->sectors = (rdb_kv_sector_meta_t*)meta_buf;
#if RDB_BLOOM_BITS > 0
    db->blooms = (uint8_t*)meta_buf + (size_t)scnt * sizeof(rdb_kv_sector_meta_t);
#endif
    db->sector_cnt = (uint8_t)scnt;
    db->gc_reserve = RDB_GC_RESERVE(scnt);
    db->active_sec = 0xFF; /* No active sector yet */

    /* [K-EC-PERSIST fix]: save erase counts before zeroing metadata.
     * ERASED sectors have no flash header, so their erase count exists
     * only in RAM.  Zeroing them would discard accumulated wear history. */
    {
        uint32_t saved_ec[RDB_MAX_SECTORS];
        for (uint8_t i = 0; i < db->sector_cnt; i++)
            saved_ec[i] = db->sectors[i].erase_cnt;

        memset(db->sectors, 0, rdb_kvdb_meta_size(db->sector_cnt));

        for (uint8_t i = 0; i < db->sector_cnt; i++)
            db->sectors[i].erase_cnt = saved_ec[i];
    }

    /* ══════════════════════════════════════════════════════════════════
     *  Phase 1: Scan all sectors — classify, recover WRITING, find max seq
     * ══════════════════════════════════════════════════════════════════ */
    uint32_t max_seq = 0;

    for (uint8_t s = 0; s < db->sector_cnt; s++) {
        rdb_kv_sector_hdr_t sh;
        if (fl_read(db, sec_addr(db, s), &sh, sizeof(sh)) != 0) {
            db->sectors[s].status = RDB_SEC_CORRUPT;
            db->stats.flash_errors++;
            continue;
        }

        /* All-0xFF magic → check if genuinely erased or corrupt */
        if (sh.magic == 0xFFFFFFFFu) {
            db->sectors[s].status = is_erased(db, s) ? RDB_SEC_ERASED : RDB_SEC_CORRUPT;
            db->sectors[s].write_off = data_start(db);
            if (db->sectors[s].status == RDB_SEC_CORRUPT)
                db->stats.corrupt_sectors++;
            continue;
        }

        /* Validate sector header magic and version-aware CRC */
        if (sh.magic != RDB_KV_SECTOR_MAGIC) {
            db->sectors[s].status = RDB_SEC_CORRUPT;
            db->stats.corrupt_sectors++;
            continue;
        }
        {
            int crc_ok = 0;
            if (sh.version == RDB_KV_VERSION) {
                /* CRC covers bytes [0..5] and [8..15], skipping self-referential hdr_crc */
                uint16_t calc = rdb_crc16(&sh, 6);
                calc = rdb_crc16_cont(calc, ((const uint8_t*)&sh) + 8, 8);
                crc_ok = (calc == sh.hdr_crc);
            } else if (sh.version == RDB_KV_VERSION_OLD) {
                crc_ok = (rdb_crc16(&sh, 6) == sh.hdr_crc);
            }
            if (!crc_ok) {
                db->sectors[s].status = RDB_SEC_CORRUPT;
                db->stats.corrupt_sectors++;
                continue;
            }
        }

        /* Valid sector header — load metadata.
         * [K-EC-PERSIST fix]: take max of RAM and flash ec to prevent
         * regression when RAM was restored from a stale snapshot. */
        if (sh.erase_cnt > db->sectors[s].erase_cnt)
            db->sectors[s].erase_cnt = sh.erase_cnt;
        db->sectors[s].create_seq = sh.create_seq;
        db->sectors[s].status = RDB_SEC_ACTIVE;

        if (RDB_SEQ_GT(sh.create_seq, max_seq))
            max_seq = sh.create_seq;

        /* Scan sector: recover WRITING records + track max_seq + update write_off.
           Merges three operations into one pass over the record chain. */
        scan_sector(db, s, writing_cb, &max_seq, RDB_TRUE);
    }
    db->write_seq = max_seq;

    /* ══════════════════════════════════════════════════════════════════
     *  Phase 2: Fixup stale duplicates + recompute metadata
     * ══════════════════════════════════════════════════════════════════ */
    fixup_stale(db);
    recalc_garbage_all(db);
    reconcile_live(db);

#if RDB_BLOOM_BITS > 0
    /* Rebuild per-sector bloom filters from the deduplicated state */
    bloom_rebuild_all(db);
#endif

    /* ══════════════════════════════════════════════════════════════════
     *  Phase 3: Select active sector [K-11 fix]
     *
     *  Choose the sector with the highest create_seq that still has
     *  writable space.  If no such sector exists (all sectors are
     *  full), fall back to the one with the highest create_seq
     *  regardless of remaining space, or rotate to a fresh sector.
     * ══════════════════════════════════════════════════════════════════ */
    {
        uint8_t  best = 0xFF;
        uint32_t best_cs = 0;

        /* Prefer sectors with remaining write space */
        for (uint8_t s = 0; s < db->sector_cnt; s++) {
            if (db->sectors[s].status != RDB_SEC_ACTIVE &&
                db->sectors[s].status != RDB_SEC_SEALED)
                continue;
            if (db->sectors[s].write_off < db->part->sector_size &&
                (best == 0xFF ||
                    RDB_SEQ_GT(db->sectors[s].create_seq, best_cs))) {
                best = s;
                best_cs = db->sectors[s].create_seq;
            }
        }

        /* Fallback: any non-erased sector with highest create_seq */
        if (best == 0xFF) {
            for (uint8_t s = 0; s < db->sector_cnt; s++) {
                if (db->sectors[s].status != RDB_SEC_ACTIVE &&
                    db->sectors[s].status != RDB_SEC_SEALED)
                    continue;
                if (best == 0xFF ||
                    RDB_SEQ_GT(db->sectors[s].create_seq, best_cs)) {
                    best = s;
                    best_cs = db->sectors[s].create_seq;
                }
            }
        }

        if (best == 0xFF) {
            /* No usable sector at all — rotate to create one */
            if (rotate(db) != 0)
                return RDB_ERR_CORRUPT;
        } else {
            db->active_sec = best;
            db->write_off = db->sectors[best].write_off;
            db->sectors[best].status = RDB_SEC_ACTIVE;

            /* Seal all other ACTIVE sectors that have data */
            for (uint8_t s = 0; s < db->sector_cnt; s++) {
                if (s != db->active_sec &&
                    db->sectors[s].status == RDB_SEC_ACTIVE &&
                    db->sectors[s].write_off > data_start(db))
                    db->sectors[s].status = RDB_SEC_SEALED;
            }
        }
    }

    /* ── Recover CORRUPT sectors by erasing them ── */
    for (uint8_t s = 0; s < db->sector_cnt; s++) {
        if (db->sectors[s].status == RDB_SEC_CORRUPT) {
            if (fl_erase(db, sec_addr(db, s)) == 0) {
                db->sectors[s].erase_cnt++;
                db->sectors[s].status = RDB_SEC_ERASED;
                db->sectors[s].write_off = data_start(db);
                db->sectors[s].garbage_bytes = 0;
                db->sectors[s].create_seq = 0;
            } else {
                db->stats.flash_errors++;
            }
        }
    }

    /* ══════════════════════════════════════════════════════════════════
     *  Phase 4: Safety-watermark recovery
     *
     *  If the number of erased sectors is below the required minimum
     *  (gc_reserve + 1), trigger GC proactively to restore headroom.
     *  This prevents deadlock scenarios where the system cannot write
     *  because there's no space, and cannot GC because there's no
     *  target for migration.
     * ══════════════════════════════════════════════════════════════════ */
    if (count_erased(db) < (uint8_t)(db->gc_reserve + 1u)) {
        gc_ensure_space(db, 0, 0, 0xFF);
    }

    db->iter_gen = 0;
    kv_cache_flush(db);
    db->initialized = 1;
    return RDB_OK;
}

/* ═══════════════════════════════════════════════════════════════════════════
 *  rdb_kvdb_format — Erase all sectors and reinitialise the KVDB
 *
 *  Preserves erase counts from flash headers (wear-leveling continuity).
 *  After format, sector 0 is the active sector with an empty record log.
 *
 *  [K-NEW-1 fix]: sector 0 is NOT erased in the loop (Pass 2); it is
 *  erased inside init_sector() to avoid a double-erase that would
 *  increment its erase count twice.
 *
 *  [K-15 fix]: NULL guard for db->sectors to handle the case where
 *  format is called before init (sectors pointer not yet assigned).
 *
 *  @param db  Database handle (must have part and sectors assigned).
 *  @return    RDB_OK on success, or an error code.
 * ═══════════════════════════════════════════════════════════════════════════ */

rdb_err_t rdb_kvdb_format(rdb_kvdb_t* db) {
    if (!db || !db->part)
        return RDB_ERR_PARAM;
    if (!db->sectors)
        return RDB_ERR_PARAM; /* [K-15 fix] */

    /* Validate sector count fits in uint8_t */
    {
        uint32_t scnt32 = db->part->total_size / db->part->sector_size;
        if (scnt32 < RDB_KV_MIN_SECTORS || scnt32 > RDB_MAX_SECTORS)
            return RDB_ERR_PARAM;
    }

    fl_lock(db);

    uint8_t scnt = (uint8_t)(db->part->total_size / db->part->sector_size);

    /* Pass 1: collect erase counts before erasing */
    uint32_t saved_ec[255]; /* scnt fits in uint8_t, 255 sectors max */
    for (uint8_t s = 0; s < scnt; s++) {
        saved_ec[s] = db->sectors[s].erase_cnt;

        /* Also check flash header for a potentially higher count
           (handles the case where RAM was zeroed but flash wasn't) */
        rdb_kv_sector_hdr_t sh;
        {
            int hdr_valid = 0;
            if (fl_read(db, sec_addr(db, s), &sh, sizeof(sh)) == 0 &&
                sh.magic == RDB_KV_SECTOR_MAGIC) {
                if (sh.version == RDB_KV_VERSION) {
                    uint16_t calc = rdb_crc16(&sh, 6);
                    calc = rdb_crc16_cont(calc, ((const uint8_t*)&sh) + 8, 8);
                    hdr_valid = (calc == sh.hdr_crc);
                } else if (sh.version == RDB_KV_VERSION_OLD) {
                    hdr_valid = (rdb_crc16(&sh, 6) == sh.hdr_crc);
                }
            }
            if (hdr_valid && sh.erase_cnt > saved_ec[s])
                saved_ec[s] = sh.erase_cnt;
        }
    }

    /* Pass 2: erase sectors 1..N-1 (sector 0 handled by init_sector) */
    for (uint8_t s = 1; s < scnt; s++) {
        if (fl_erase(db, sec_addr(db, s)) != 0) {
            fl_unlock(db);
            return RDB_ERR_FLASH;
        }
        db->sectors[s].erase_cnt = saved_ec[s] + 1;
        db->sectors[s].create_seq = 0;
        db->sectors[s].garbage_bytes = 0;
        db->sectors[s].write_off = data_start(db);
        db->sectors[s].status = RDB_SEC_ERASED;
    }

    /* Sector 0: pre-load ec so init_sector picks it up */
    db->sectors[0].erase_cnt = saved_ec[0];

    db->write_seq = 1;
    if (init_sector(db, 0) != 0) {
        fl_unlock(db);
        return RDB_ERR_FLASH;
    }

    db->active_sec = 0;
    db->write_off = data_start(db);
    db->live_bytes = 0;
    db->iter_gen = 0;
    kv_cache_flush(db);
    db->initialized = 1;
    memset(&db->stats, 0, sizeof(db->stats));

    fl_unlock(db);
    return RDB_OK;
}

/* ═══════════════════════════════════════════════════════════════════════════
 *  rdb_kvdb_set — Write or update a key-value pair
 *
 *  Two-phase write protocol:
 *    Phase A: Write the record header with state = WRITING, followed
 *             by the key and value payloads.
 *    Phase B: Commit by writing state = VALID (1→0 bit flip).
 *
 *  If power is lost during Phase A, the WRITING record will be
 *  recovered (or discarded) at the next init.
 *
 *  [K-4/K-5 fix]: After gc_ensure_space() (which may trigger GC and
 *  move records), re-find the old copy of the key to get its
 *  updated address/sector before invalidation.
 *
 *  [K-4-pad fix]: Value alignment padding is written in a loop of
 *  8-byte chunks, correctly handling arbitrary write granularities
 *  and large alignment gaps.
 *
 *  @param db   Database handle.
 *  @param key  Null-terminated key string (1..RDB_MAX_KEY_LEN chars).
 *  @param val  Pointer to value data (may be NULL if len == 0).
 *  @param len  Value length in bytes (0..RDB_MAX_VAL_LEN).
 *  @return     RDB_OK on success, or an error code.
 * ═══════════════════════════════════════════════════════════════════════════ */

rdb_err_t rdb_kvdb_set(rdb_kvdb_t* db, const char* key,
    const void* val, uint16_t len) {
    if (!db || !db->initialized)
        return RDB_ERR_NOT_INIT;
    if (!key)
        return RDB_ERR_PARAM;

    uint8_t kl;
    {   int sr = key_scan_len(key, &kl);
        if (sr == -1) return RDB_ERR_PARAM;
        if (sr == 1)  return RDB_ERR_TOO_LARGE;
    }
    if (len > RDB_MAX_VAL_LEN)
        return RDB_ERR_TOO_LARGE;
    if (len > 0 && !val)
        return RDB_ERR_PARAM;

    uint32_t g = wr_gran(db);
    uint32_t ka = RDB_ALIGN_UP(kl, g);
    uint32_t va = RDB_ALIGN_UP(len, g);
    uint32_t rsz = KV_REC_SZ + ka + va;
    if (rsz > data_cap(db))
        return RDB_ERR_TOO_LARGE;

    fl_lock(db);

    /* Step 1: find existing record (pre-GC snapshot) */
    find_ctx_t fc;
    find_latest(db, key, kl, &fc);
    uint32_t old_rsz = fc.found ? fc.best_rsz : 0;

    /* Step 2: reserve space (may trigger GC) */
    rdb_err_t rc = gc_ensure_space(db, rsz, old_rsz,
        fc.found ? fc.best_sec : 0xFF);
    if (rc != RDB_OK) {
        fl_unlock(db);
        return rc;
    }

    /* Step 2b [K-4/K-5 fix]: re-find after GC — the old record may
       have been moved to a different sector/address by migration. */
    find_latest(db, key, kl, &fc);

    /* Ensure active sector has room (rotate if needed) */
    if (db->sectors[db->active_sec].write_off + rsz > db->part->sector_size) {
        if (rotate(db) != 0) {
            fl_unlock(db);
            return RDB_ERR_FULL;
        }
    }

    /* Step 3: two-phase write */
    db->write_seq++;

    /* Compute data CRC over raw (unpadded) key + value */
    uint16_t dcrc = rdb_crc16(key, kl);
    if (len > 0)
        dcrc = rdb_crc16_cont(dcrc, val, len);

    /* Build record header */
    rdb_kv_record_hdr_t rh;
    rh.magic = RDB_KV_RECORD_MAGIC;
    rh.state = RDB_STATE_WRITING;
    rh.key_len = kl;
    rh._pad0 = 0xFF;
    rh.val_len = len;
    rh.key_hash = rdb_hash16(key, kl);
    rh.seq = db->write_seq;
    rh.data_crc = dcrc;
    rh._pad1 = 0xFFFF;

    uint32_t wa = sec_addr(db, db->active_sec) +
                  db->sectors[db->active_sec].write_off;

    if (rsz <= RDB_STACK_BUF_SIZE) {
        /* Assemble the entire record (header + key + value) in a
           stack buffer and write in one flash operation. */
        uint8_t mbuf[RDB_STACK_BUF_SIZE];
        memcpy(mbuf, &rh, KV_REC_SZ);

        /* Key + key padding */
        memcpy(mbuf + KV_REC_SZ, key, kl);
        if (ka > kl)
            memset(mbuf + KV_REC_SZ + kl, 0xFF, ka - kl);

        /* Value + value padding */
        if (len > 0) {
            memcpy(mbuf + KV_REC_SZ + ka, val, len);
            if (va > len)
                memset(mbuf + KV_REC_SZ + ka + len, 0xFF, va - len);
        }

        if (fl_write(db, wa, mbuf, rsz) != 0) {
            db->stats.flash_errors++;
            if (kv_mark_dead(db, wa) != 0)
                db->stats.flash_errors++;
            fl_unlock(db);
            return RDB_ERR_FLASH;
        }
    } else {
        /* Large-record multi-step write.
           For records that exceed the stack buffer, write header,
           key, and value as separate flash operations. */

        /* Write record header */
        if (fl_write(db, wa, &rh, sizeof(rh)) != 0) {
            db->stats.flash_errors++;
            if (kv_mark_dead(db, wa) != 0)
                db->stats.flash_errors++;
            fl_unlock(db);
            return RDB_ERR_FLASH;
        }

        /* Write key + key alignment padding */
        {
            uint8_t kb[RDB_MAX_KEY_LEN + 8];
            memcpy(kb, key, kl);
            if (ka > kl)
                memset(kb + kl, 0xFF, ka - kl);
            if (fl_write(db, wa + KV_REC_SZ, kb, ka) != 0) {
                db->stats.flash_errors++;
                if (kv_mark_dead(db, wa) != 0)
                    db->stats.flash_errors++;
                fl_unlock(db);
                return RDB_ERR_FLASH;
            }
        }

        /* Write value data plus alignment padding in streaming chunks */
        if (len > 0) {
            uint32_t       max_chunk = RDB_STACK_BUF_SIZE;
            uint32_t       rem = va;
            uint32_t       data_pos = 0;
            uint32_t       wpos = wa + KV_REC_SZ + ka;
            const uint8_t* vp = (const uint8_t*)val;
            uint8_t        buf[RDB_STACK_BUF_SIZE];

            max_chunk -= max_chunk % g;
            if (max_chunk == 0)
                max_chunk = g;

            while (rem) {
                uint32_t ch = RDB_MIN(rem, max_chunk);
                RDB_PAGE_CLAMP(ch, wpos);
                uint32_t data_ch = 0;

                if (data_pos < len) {
                    data_ch = RDB_MIN(ch, (uint32_t)len - data_pos);
                    memcpy(buf, vp + data_pos, data_ch);
                }
                if (data_ch < ch)
                    memset(buf + data_ch, 0xFF, ch - data_ch);

                rem -= ch;
                data_pos += data_ch;

                if (fl_write(db, wpos, buf, ch) != 0) {
                    db->stats.flash_errors++;
                    if (kv_mark_dead(db, wa) != 0)
                        db->stats.flash_errors++;
                    fl_unlock(db);
                    return RDB_ERR_FLASH;
                }
                wpos += ch;
            }
        }
    }

    /* Phase B: commit — WRITING → VALID (single byte, 1→0 transition) */
    {
        uint8_t v = RDB_STATE_VALID;
        if (fl_write(db, wa + 1, &v, 1) != 0) {
            db->stats.flash_errors++;
            /* Advance write_off past the abandoned record so the next
               set() starts in clean erased space.  The WRITING record
               will be skipped by scan_sector() on next init. */
            db->sectors[db->active_sec].write_off += rsz;
            db->sectors[db->active_sec].garbage_bytes += rsz;
            fl_unlock(db);
            return RDB_ERR_FLASH;
        }
    }

    /* Step 4: bookkeeping — update write offset and live bytes */
    db->sectors[db->active_sec].write_off += rsz;
    db->write_off = db->sectors[db->active_sec].write_off;
    db->live_bytes += rsz;

    /* Cache the new record's address for fast future lookups */
    kv_cache_insert(db, key, kl, wa);

#if RDB_BLOOM_BITS > 0
    /* Set bloom bits for this key in the active sector */
    if (db->blooms) {
        uint8_t* bm = db->blooms + (size_t)db->active_sec * RDB_BLOOM_BYTES;
        RDB_BLOOM_SET(bm, rdb_hash16(key, kl));
    }
#endif

    /* Step 5 [K-4/K-5 fix]: invalidate old copy using refreshed fc.
       The old record was located by the re-find in Step 2b, so its
       address is current even if GC moved it. */
    if (fc.found) {
        rdb_kv_record_hdr_t oh;
        if (fl_read(db, fc.best_addr, &oh, sizeof(oh)) == 0 &&
            oh.state == RDB_STATE_VALID) {
            if (kv_mark_dead(db, fc.best_addr) != 0) {
                db->stats.flash_errors++;
            } else {
                db->sectors[fc.best_sec].garbage_bytes += fc.best_rsz;
                if (db->live_bytes >= fc.best_rsz)
                    db->live_bytes -= fc.best_rsz;
                else
                    db->live_bytes = 0;
            }
        }
    }

    db->stats.write_ops++;
    db->iter_gen++;
    fl_unlock(db);
    return RDB_OK;
}

/* ═══════════════════════════════════════════════════════════════════════════
 *  rdb_kvdb_get — Read the value associated with a key
 *
 *  Finds the latest VALID record matching the key, verifies its data
 *  CRC, and copies the value into the caller's buffer.
 *
 *  If buf_len < val_len, the first buf_len bytes are copied and
 *  RDB_ERR_TOO_LARGE is returned (out_len still reports the full
 *  value length so the caller can retry with a larger buffer).
 *
 *  @param db       Database handle.
 *  @param key      Null-terminated key string.
 *  @param buf      Buffer to receive the value (may be NULL to query length).
 *  @param buf_len  Size of buf in bytes.
 *  @param[out] out_len  Receives the actual value length (may be NULL).
 *  @return         RDB_OK, RDB_ERR_NOT_FOUND, RDB_ERR_CRC, etc.
 * ═══════════════════════════════════════════════════════════════════════════ */

rdb_err_t rdb_kvdb_get(rdb_kvdb_t* db, const char* key,
    void* buf, uint16_t buf_len, uint16_t* out_len) {
    if (!db || !db->initialized)
        return RDB_ERR_NOT_INIT;
    if (!key)
        return RDB_ERR_PARAM;

    uint8_t kl;
    if (key_scan_len(key, &kl) != 0)
        return RDB_ERR_PARAM;   /* empty, too long, or not null-terminated */

    fl_lock(db);

    uint32_t best_addr;
    uint8_t  found = 0;

    /* Try the key cache first (fast path) */
    uint32_t cached = kv_cache_lookup(db, key, kl);
    if (cached != RDB_ADDR_INVALID) {
        rdb_kv_record_hdr_t rh;
        if (fl_read(db, cached, &rh, sizeof(rh)) == 0 &&
            rh.state == RDB_STATE_VALID &&
            rh.key_len == kl) {
            /* Verify CRC — cache hit may be stale */
            uint16_t calc;
            if (data_crc_flash(db, cached, rh.key_len, rh.val_len, &calc) == 0 &&
                calc == rh.data_crc) {
                /* Verify key bytes match (cache may collide on hash+prefix) */
                uint8_t kb[RDB_MAX_KEY_LEN];
                if (fl_read(db, cached + KV_REC_SZ, kb, kl) == 0 &&
                    memcmp(kb, key, kl) == 0) {
                    best_addr = cached;
                    found = 1;
                    goto read_value;
                }
            }
        }
        /* Cache entry is stale — invalidate and fall through to scan */
        kv_cache_invalidate(db, key, kl);
    }

    find_ctx_t fc;
    find_latest(db, key, kl, &fc);
    if (!fc.found) {
        fl_unlock(db);
        return RDB_ERR_NOT_FOUND;
    }
    best_addr = fc.best_addr;

    /* Populate cache for next access */
    kv_cache_insert(db, key, kl, fc.best_addr);

read_value:
    ; /* null statement — label before declaration in C99 */
    /* Read the record header for val_len and data_crc */
    rdb_kv_record_hdr_t rh;
    if (fl_read(db, best_addr, &rh, sizeof(rh)) != 0) {
        db->stats.flash_errors++;
        fl_unlock(db);
        return RDB_ERR_FLASH;
    }

    if (out_len)
        *out_len = rh.val_len;

    /* Verify data integrity (if we came via cache, already verified) */
    if (!found) {
        uint16_t calc;
        if (data_crc_flash(db, best_addr, rh.key_len, rh.val_len, &calc) != 0) {
            db->stats.flash_errors++;
            fl_unlock(db);
            return RDB_ERR_FLASH;
        }
        if (calc != rh.data_crc) {
            db->stats.crc_errors++;
            fl_unlock(db);
            return RDB_ERR_CRC;
        }
    }

    /* Read value data */
    uint32_t g = wr_gran(db);
    uint32_t ka = RDB_ALIGN_UP(rh.key_len, g);
    uint32_t va = best_addr + KV_REC_SZ + ka; /* Value flash address */
    uint16_t rd = (uint16_t)RDB_MIN(buf_len, rh.val_len);

    if (buf && rd > 0) {
        if (fl_read(db, va, buf, rd) != 0) {
            db->stats.flash_errors++;
            fl_unlock(db);
            return RDB_ERR_FLASH;
        }
    }

    db->stats.read_ops++;
    fl_unlock(db);
    return (buf_len < rh.val_len) ? RDB_ERR_TOO_LARGE : RDB_OK;
}

/* ═══════════════════════════════════════════════════════════════════════════
 *  rdb_kvdb_delete — Delete all copies of a key
 *
 *  Scans every sector and marks ALL VALID records matching the key
 *  as DEAD.  This is an O(N) operation across the entire flash.
 *
 *  Unlike set() which leaves exactly one VALID copy, delete()
 *  guarantees that no VALID copy of the key remains after completion.
 *
 *  @param db   Database handle.
 *  @param key  Null-terminated key string.
 *  @return     RDB_OK if at least one copy was found and deleted,
 *              RDB_ERR_NOT_FOUND if the key did not exist.
 * ═══════════════════════════════════════════════════════════════════════════ */

rdb_err_t rdb_kvdb_delete(rdb_kvdb_t* db, const char* key) {
    if (!db || !db->initialized)
        return RDB_ERR_NOT_INIT;
    if (!key)
        return RDB_ERR_PARAM;

    uint8_t kl;
    if (key_scan_len(key, &kl) != 0)
        return RDB_ERR_PARAM;   /* empty, too long, or not null-terminated */

    fl_lock(db);

    uint16_t hash = rdb_hash16(key, kl);
    int      found = 0;

    for (uint8_t s = 0; s < db->sector_cnt; s++) {
        if (db->sectors[s].status == RDB_SEC_ERASED ||
            db->sectors[s].status == RDB_SEC_CORRUPT)
            continue;

        uint32_t base = sec_addr(db, s);
        uint32_t off = data_start(db);
        uint32_t ss = db->part->sector_size;

        while (off + KV_REC_SZ <= ss) {
            rdb_kv_record_hdr_t rh;
            if (fl_read(db, base + off, &rh, sizeof(rh)) != 0)
                break;
            if (rh.magic == 0xFFu && rh.state == 0xFFu)
                break;
            if (rh.magic != RDB_KV_RECORD_MAGIC ||
                rh.key_len < 1 || rh.key_len > RDB_MAX_KEY_LEN ||
                rh.val_len > RDB_MAX_VAL_LEN) {
                off += kv_corrupt_skip(db); /* [K-3 fix] */
                continue;
            }
            uint32_t rsz = rec_size(db, rh.key_len, rh.val_len);
            if (off + rsz > ss)
                break;

            if (rh.state == RDB_STATE_VALID &&
                rh.key_len == kl && rh.key_hash == hash) {
                /* Full key comparison from flash */
                uint8_t kb[RDB_MAX_KEY_LEN];
                if (fl_read(db, base + off + KV_REC_SZ, kb, kl) == 0 &&
                    memcmp(kb, key, kl) == 0) {
                    if (kv_mark_dead(db, base + off) != 0) {
                        db->stats.flash_errors++;
                    } else {
                        db->sectors[s].garbage_bytes += rsz;
                        if (db->live_bytes >= rsz)
                            db->live_bytes -= rsz;
                        else
                            db->live_bytes = 0;
                    }
                    found = 1;
                }
            }
            off += rsz;
        }
    }

    if (found)
        kv_cache_invalidate(db, key, kl);

    db->stats.delete_ops++;
    db->iter_gen++;
    fl_unlock(db);
    return found ? RDB_OK : RDB_ERR_NOT_FOUND;
}

/* ═══════════════════════════════════════════════════════════════════════════
 *  rdb_kvdb_exists — Check if a key exists (without reading the value)
 *
 *  @param db   Database handle.
 *  @param key  Null-terminated key string.
 *  @return     RDB_OK if found, RDB_ERR_NOT_FOUND if absent,
 *              RDB_ERR_PARAM or RDB_ERR_NOT_INIT on invalid input.
 * ═══════════════════════════════════════════════════════════════════════════ */

rdb_err_t rdb_kvdb_exists(rdb_kvdb_t* db, const char* key) {
    if (!db || !db->initialized)
        return RDB_ERR_NOT_INIT;
    if (!key)
        return RDB_ERR_PARAM;
    uint8_t kl;
    if (key_scan_len(key, &kl) != 0)
        return RDB_ERR_PARAM;   /* empty, too long, or not null-terminated */

    fl_lock(db);
    find_ctx_t fc;
    find_latest(db, key, kl, &fc);
    fl_unlock(db);
    return fc.found ? RDB_OK : RDB_ERR_NOT_FOUND;
}

/* ═══════════════════════════════════════════════════════════════════════════
 *  rdb_kvdb_gc — Trigger manual garbage collection
 *
 *  If the GC invariant is already satisfied (enough erased sectors),
 *  this is a no-op.  Otherwise, runs gc_ensure_space(0, ...) which
 *  invokes the four-phase GC without requiring a specific write target.
 *
 *  @param db  Database handle.
 *  @return    RDB_OK if the GC invariant is satisfied after return.
 * ═══════════════════════════════════════════════════════════════════════════ */

rdb_err_t rdb_kvdb_gc(rdb_kvdb_t* db) {
    if (!db || !db->initialized)
        return RDB_ERR_NOT_INIT;
    fl_lock(db);
    if (count_erased(db) >= (uint8_t)(db->gc_reserve + 1u)) {
        fl_unlock(db);
        return RDB_OK;
    }
    rdb_err_t rc = gc_ensure_space(db, 0, 0, 0xFF);
    fl_unlock(db);
    return rc;
}

/* ═══════════════════════════════════════════════════════════════════════════
 *  rdb_kvdb_space_info — Query storage space utilisation
 *
 *  Reports the logical capacity, used space, and available space.
 *  GC reserve sectors are excluded from the total capacity.
 *
 *  @param db     Database handle.
 *  @param[out] total  Total logical capacity in bytes (may be NULL).
 *  @param[out] used   Used space (live_bytes) in bytes (may be NULL).
 *  @param[out] avail  Available space in bytes (may be NULL).
 * ═══════════════════════════════════════════════════════════════════════════ */

void rdb_kvdb_space_info(rdb_kvdb_t* db,
    uint32_t* total, uint32_t* used, uint32_t* avail) {
    if (!db || !db->initialized)
        return;
    fl_lock(db);
    uint32_t t = max_live(db);
    uint32_t u = RDB_MIN(db->live_bytes, t);
    if (total)
        *total = t;
    if (used)
        *used = u;
    if (avail)
        *avail = t - u;
    fl_unlock(db);
}

/* ═══════════════════════════════════════════════════════════════════════════
 *  rdb_kvdb_wear_info — Query flash wear statistics
 *
 *  Reports the minimum, maximum, and average erase counts across
 *  all sectors.  Useful for monitoring flash lifetime and verifying
 *  that wear leveling is working correctly.
 *
 *  @param db     Database handle.
 *  @param[out] min_ec  Minimum erase count (may be NULL).
 *  @param[out] max_ec  Maximum erase count (may be NULL).
 *  @param[out] avg_ec  Average erase count (may be NULL).
 * ═══════════════════════════════════════════════════════════════════════════ */

void rdb_kvdb_wear_info(rdb_kvdb_t* db,
    uint32_t* min_ec, uint32_t* max_ec, uint32_t* avg_ec) {
    if (!db || !db->initialized)
        return;
    fl_lock(db);
    uint32_t mn = 0xFFFFFFFFu, mx = 0;
    uint64_t sum = 0;
    for (uint8_t s = 0; s < db->sector_cnt; s++) {
        uint32_t ec = db->sectors[s].erase_cnt;
        if (ec < mn)
            mn = ec;
        if (ec > mx)
            mx = ec;
        sum += ec;
    }
    if (mn == 0xFFFFFFFFu)
        mn = 0;
    if (min_ec)
        *min_ec = mn;
    if (max_ec)
        *max_ec = mx;
    if (avg_ec)
        *avg_ec = (uint32_t)(sum / db->sector_cnt);
    fl_unlock(db);
}

/* ═══════════════════════════════════════════════════════════════════════════
 *  rdb_kvdb_get_stats / rdb_kvdb_reset_stats — Runtime statistics
 *
 *  get_stats copies the current statistics snapshot to a caller buffer.
 *  reset_stats zeroes all counters.
 * ═══════════════════════════════════════════════════════════════════════════ */

void rdb_kvdb_get_stats(rdb_kvdb_t* db, rdb_kv_stats_t* out) {
    if (!db || !out)
        return;
    fl_lock(db);
    *out = db->stats;
    fl_unlock(db);
}

void rdb_kvdb_reset_stats(rdb_kvdb_t* db) {
    if (!db)
        return;
    fl_lock(db);
    memset(&db->stats, 0, sizeof(db->stats));
    fl_unlock(db);
}

/* ═══════════════════════════════════════════════════════════════════════════
 *  rdb_kv_iter_init / rdb_kv_iter_next — Key-value iterator
 *
 *  ┌────────────────────────────────────────────────────────────────────┐
 *  │ Snapshot-Based Enumeration with GC Interference Handling           │
 *  ├────────────────────────────────────────────────────────────────────┤
 *  │                                                                    │
 *  │ **Snapshot Semantics**:                                            │
 *  │   • Iterator captures database state at init time (iter_gen)      │
 *  │   • Consistent view: even if GC occurs, iterator sees snapshot   │
 *  │   • Key invariant: No key appears twice (find_latest() check)     │
 *  │                                                                    │
 *  │ **GC Interference Detection**:                                     │
 *  │   • iter_gen is incremented when:                                 │
 *  │       - rdb_kv_put() executes (new write)                         │
 *  │       - rdb_kv_del() executes (deletion marker added)            │
 *  │       - garbage_collect() executes (sectors erased)               │
 *  │   • If gen mismatch: RDB_ERR_BUSY returned (iteration aborted)   │
 *  │   • Caller must restart iterator for consistent snapshot         │
 *  │                                                                    │
 *  │ **Why GC Breaks Iteration**:                                       │
 *  │   Without this check:                                              │
 *  │     1. Iter starts, sector 0 is [A₀, B₀, C₀, ...]               │
 *  │     2. Iter reads A₀, B₀, C₀; advances to sector 1               │
 *  │     3. Meantime: GC erases sector 0 (rebuilds to sector 2)       │
 *  │     4. Sector 1 now has [A₁', D₀, ...] (reordered)              │
 *  │     5. Iter returns: A₀, B₀, C₀ (from old sector 0)             │
 *  │        then         A₁', D₀, ... (from new sector 1)             │
 *  │     6. Result: A appears twice! (violation of find_latest())    │
 *  │                                                                    │
 *  │ **Authoritative Copy Detection**:                                 │
 *  │   For each VALID record encountered:                              │
 *  │     - Call find_latest(key) to locate actual copy                │
 *  │     - If returned address != current record address               │
 *  │       → This is a stale duplicate → Skip it                      │
 *  │     - Else (address matches) → This is the live copy → Return it │
 *  │   Result: Iterator never returns same key twice               │
 *  │                                                                    │
 *  │ **Power-Loss & Recovery**:                                         │
 *  │   • Iterator survives recovery (iter_gen incremented at init)    │
 *  │   • Old iterators become invalid (good!)                          │
 *  │   • Prevent: Reading from recovered state with old snapshot     │
 *  │                                                                    │
 *  │ **Performance**:                                                   │
 *  │   • Sector scan: O(S) where S = number of sectors                │
 *  │   • Per record: find_latest() = O(log S) binary search          │
 *  │   • Total: O(N·log S) where N = total records                   │
 *  │   • Typical case: Early match in find_latest() → ~O(N)          │
 *  │                                                                    │
 *  └────────────────────────────────────────────────────────────────────┘
 *
 *  Provides forward iteration over all live key-value pairs.
 *
 *  The iterator uses a generation counter (iter_gen) to detect
 *  concurrent modifications.  If the database is modified between
 *  iter_init and iter_next, RDB_ERR_BUSY is returned.
 *
 *  For each candidate record, find_latest() is called to verify
 *  that it is the authoritative copy (not a stale duplicate).
 *  This ensures the iterator never returns duplicate keys.
 *
 *  Usage pattern:
 *    rdb_kv_iter_t it;
 *    rdb_kv_iter_init(&it, db);
 *    while (rdb_kv_iter_next(&it, key, sizeof(key),
 *                            val, sizeof(val), &klen, &vlen) == RDB_OK) {
 *        // process key/val
 *    }
 * ═══════════════════════════════════════════════════════════════════════════ */

rdb_err_t rdb_kv_iter_init(rdb_kv_iter_t* it, rdb_kvdb_t* db) {
    if (!it || !db || !db->initialized)
        return RDB_ERR_PARAM;
    it->db = db;
    it->gen = db->iter_gen;
    it->sector = 0;
    it->offset = data_start(db);
    return RDB_OK;
}

rdb_err_t rdb_kv_iter_next(rdb_kv_iter_t* it,
    char* key_buf, uint16_t key_cap,
    void* val_buf, uint16_t val_cap,
    uint16_t* out_klen, uint16_t* out_vlen) {
    if (!it || !it->db || !it->db->initialized)
        return RDB_ERR_PARAM;
    rdb_kvdb_t* db = it->db;
    if (it->gen != db->iter_gen)
        return RDB_ERR_BUSY;

    fl_lock(db);
    uint32_t g = wr_gran(db);

    while (it->sector < db->sector_cnt) {
        rdb_kv_sector_meta_t* sm = &db->sectors[it->sector];

        /* Skip erased and corrupt sectors */
        if (sm->status == RDB_SEC_ERASED || sm->status == RDB_SEC_CORRUPT) {
            it->sector++;
            it->offset = data_start(db);
            continue;
        }

        uint32_t base = sec_addr(db, it->sector);
        uint32_t ss = db->part->sector_size;

        while ((uint32_t)it->offset + KV_REC_SZ <= ss) {
            rdb_kv_record_hdr_t rh;
            if (fl_read(db, base + it->offset, &rh, sizeof(rh)) != 0) {
                it->offset = ss;
                break;
            }

            /* End of record chain */
            if (rh.magic == 0xFFu && rh.state == 0xFFu) {
                it->offset = ss;
                break;
            }

            /* Corrupt record — skip */
            if (rh.magic != RDB_KV_RECORD_MAGIC ||
                rh.key_len < 1 || rh.key_len > RDB_MAX_KEY_LEN ||
                rh.val_len > RDB_MAX_VAL_LEN) {
                it->offset += kv_corrupt_skip(db);
                continue;
            }

            uint32_t rsz = rec_size(db, rh.key_len, rh.val_len);
            if ((uint32_t)it->offset + rsz > ss) {
                it->offset = ss;
                break;
            }

            uint32_t cur_off = it->offset;
            it->offset += rsz; /* Advance past this record */

            /* Only consider VALID records */
            if (rh.state != RDB_STATE_VALID)
                continue;

            /* Read key from flash */
            uint8_t kb[RDB_MAX_KEY_LEN];
            if (fl_read(db, base + cur_off + KV_REC_SZ, kb, rh.key_len) != 0)
                continue;

            /* Verify this is the authoritative copy (latest seq) */
            find_ctx_t fc;
            find_latest(db, (const char*)kb, rh.key_len, &fc);
            if (!fc.found || fc.best_addr != base + cur_off)
                continue; /* Stale duplicate — skip */

            /* ── Found a live record — output it ── */

            if (out_klen)
                *out_klen = rh.key_len;
            if (out_vlen)
                *out_vlen = rh.val_len;

            /* Copy key to caller buffer (null-terminated) */
            if (key_buf && key_cap > 0) {
                uint16_t cp = (uint16_t)RDB_MIN(key_cap - 1u, rh.key_len);
                memcpy(key_buf, kb, cp);
                key_buf[cp] = '\0';
            }

            /* Copy value to caller buffer */
            if (val_buf && val_cap > 0 && rh.val_len > 0) {
                uint32_t ka = RDB_ALIGN_UP(rh.key_len, g);
                uint16_t vr = (uint16_t)RDB_MIN(val_cap, rh.val_len);
                if (fl_read(db, base + cur_off + KV_REC_SZ + ka, val_buf, vr) != 0) {
                    db->stats.flash_errors++;
                    fl_unlock(db);
                    return RDB_ERR_FLASH;
                }
            }

            fl_unlock(db);
            return RDB_OK;
        }

        /* Move to next sector */
        it->sector++;
        it->offset = data_start(db);
    }

    fl_unlock(db);
    return RDB_ERR_ITER_END;
}
