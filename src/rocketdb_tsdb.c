/*****************************************************************************
 * rocketdb_tsdb.c — RocketDB TSDB Engine
 *
 * Ring-buffer time-series store with integrated design principles:
 *
 *  CORE ARCHITECTURE:
 *   - uint32_t time_delta (rotation driven by space, not time overflow)
 *   - Epoch management (primary feature, not recovery artifact — Rule 3.1)
 *   - EMPTY-gap tolerance in ring (data_gaps counter, no false CORRUPT)
 *   - Seal degradation (incomplete seal → ACTIVE, not CORRUPT)
 *   - CRC-safe get_latest/get_oldest (auto-skip bad records)
 *
 *  DESIGN RULES IMPLEMENTED:
 *   Rule 3.1 —  Epoch management handles RTC wraparound (49-day tick cycle)
 *   Rule 3.2 —  Ring buffer with strict head/tail pointer safety
 *   Rule 4.2 —  Two-phase commit for all metadata updates
 *   Rule 4.3 —  Progressive CRC validation for large data streams
 *   Rule 4.4 —  Hierarchical error codes for precise failure diagnosis
 *
 *  IMPLEMENTATION NOTES:
 *   - Rotation is driven by space availability, not timestamp wraparound
 *   - Epoch ID is monotonic global counter (independent of RTC resets)
 *   - Each epoch maps to exactly one Flash sector
 *
 *  See DESIGN_RATIONALE.md for full rationale behind each rule.
 * 
 * Copyright (c) 2015 XiaSheng(info@zhis.net)
 * SPDX-License-Identifier: MIT
 * @date    2015-05-04
 * @version 1.1.0
 * 
 *****************************************************************************/

#include "rocketdb.h"
#include <string.h>

#define TS_HDR_SZ ((uint32_t)sizeof(rdb_ts_sector_hdr_t))
#define TS_REC_SZ ((uint32_t)sizeof(rdb_ts_record_hdr_t))

/* ═══════════════════════════════════════════════════════════════════════════
 *  Geometry helpers (inline, private to this translation unit)
 *
 *  twr()   — Write granularity in bytes (1 / 2 / 4 / 8).
 *  tds()   — Byte offset of the first record within a sector,
 *            aligned to write granularity after the sector header.
 *  tdc()   — Usable data capacity per sector (sector_size − header).
 *  trs()   — Total on-flash size of one TS record (header + aligned data).
 *  tsa()   — Absolute flash address of sector index `s`.
 *  tnext() — Next sector index in the ring (wraps at sector_cnt).
 *  tprev() — Previous sector index in the ring (wraps at 0).
 * ═══════════════════════════════════════════════════════════════════════════ */

/** @brief Return the write granularity in bytes (1, 2, 4, or 8). */
static inline uint32_t twr(const rdb_tsdb_t* db) {
    return 1u << db->part->write_gran;
}

/**
 * @brief Byte offset where records begin within a sector.
 *
 * The sector header (TS_HDR_SZ bytes) is followed by padding to align
 * the first record to the flash write granularity boundary.
 */
static inline uint32_t tds(const rdb_tsdb_t* db) {
    return RDB_ALIGN_UP(TS_HDR_SZ, twr(db));
}

/** @brief Usable data capacity per sector (total sector size minus header area). */
static inline uint32_t tdc(const rdb_tsdb_t* db) {
    return db->sector_size - tds(db);
}

/**
 * @brief Total on-flash footprint of a single TS record.
 *
 * Layout: [record_hdr (12B)] [data padded to write granularity]
 *
 * @param db  Database handle (for write granularity).
 * @param dl  Data length in bytes (unpadded).
 * @return    Total record size including header and padding.
 */
static inline uint32_t trs(const rdb_tsdb_t* db, uint16_t dl) {
    return TS_REC_SZ + RDB_ALIGN_UP(dl, twr(db));
}

/** @brief Absolute flash base address for sector index `s`. */
static inline uint32_t tsa(const rdb_tsdb_t* db, uint8_t s) {
    return db->part->base_addr + (uint32_t)s * db->sector_size;
}

/** @brief Next sector index in the circular ring buffer. */
static inline uint8_t tnext(const rdb_tsdb_t* db, uint8_t s) {
    return (uint8_t)((s + 1u) % db->sector_cnt);
}

/** @brief Previous sector index in the circular ring buffer. */
static inline uint8_t tprev(const rdb_tsdb_t* db, uint8_t s) {
    return (s == 0) ? db->sector_cnt - 1 : s - 1;
}

/* ═══════════════════════════════════════════════════════════════════════════
 *  Flash access wrappers
 *
 *  Thin wrappers around the user-provided flash operations.
 *  Lock/unlock/yield are no-ops if the corresponding function pointer
 *  is NULL, allowing bare-metal single-threaded use without overhead.
 * ═══════════════════════════════════════════════════════════════════════════ */

/** @brief Acquire the flash mutex (no-op if lock callback is NULL). */
static inline void tlock(const rdb_tsdb_t* db) {
    if (db->part->ops->lock)
        db->part->ops->lock();
}

/** @brief Release the flash mutex (no-op if unlock callback is NULL). */
static inline void tunlock(const rdb_tsdb_t* db) {
    if (db->part->ops->unlock)
        db->part->ops->unlock();
}

/** @brief Yield CPU during long operations (no-op if yield callback is NULL). */
static inline void tyield(const rdb_tsdb_t* db) {
    if (db->part->ops->yield)
        db->part->ops->yield();
}

/** @brief Read `n` bytes from flash address `a` into buffer `b`. */
static inline int trd(const rdb_tsdb_t* db, uint32_t a, void* b, size_t n) {
    return db->part->ops->read(a, (uint8_t*)b, n);
}

/** @brief Write `n` bytes from buffer `b` to flash address `a`. */
static inline int twr_f(const rdb_tsdb_t* db, uint32_t a,
    const void* b, size_t n) {
    return db->part->ops->write(a, (const uint8_t*)b, n);
}

/** @brief Erase the sector at flash address `a`. */
static inline int tera(const rdb_tsdb_t* db, uint32_t a) {
    return db->part->ops->erase(a);
}

/* ═══════════════════════════════════════════════════════════════════════════
 *  Corrupt-record skip step (mirrors KVDB K-3 fix)
 *
 *  When a record header fails validation (bad magic, etc.), advance
 *  by this many bytes rather than by 1.  This prevents byte-by-byte
 *  crawling through corrupt regions and reduces the risk of
 *  mid-record false-positive parsing.
 *
 *  The step size equals one full record header aligned to write
 *  granularity, which is the minimum possible record footprint.
 * ═══════════════════════════════════════════════════════════════════════════ */

static inline uint32_t ts_corrupt_skip(const rdb_tsdb_t* db) {
    return RDB_ALIGN_UP(TS_REC_SZ, twr(db));
}

/* ═══════════════════════════════════════════════════════════════════════════
 *  ts_data_crc — Compute CRC-16 of record data payload from flash
 *
 *  Reads the data payload in streaming fashion (stack buffer) and
 *  computes a CRC-16.  Used for integrity verification on read
 *  and for WRITING record recovery during init.
 *
 *  @param db        Database handle.
 *  @param addr      Absolute flash address of the data payload start.
 *  @param dlen      Data length in bytes.
 *  @param[out] out  Computed CRC-16 result.
 *  @return          0 on success, -1 on flash read failure.
 * ═══════════════════════════════════════════════════════════════════════════ */

static int ts_data_crc(const rdb_tsdb_t* db, uint32_t addr,
    uint16_t dlen, uint16_t* out) {
    uint16_t crc = rdb_crc16(NULL, 0); /* Initial CRC seed */
    uint32_t rem = dlen, pos = addr;
    uint8_t  buf[RDB_STACK_BUF_SIZE];

    while (rem) {
        uint32_t ch = RDB_MIN(rem, sizeof(buf));
        if (trd(db, pos, buf, ch) != 0)
            return -1;
        crc = rdb_crc16_cont(crc, buf, ch);
        pos += ch;
        rem -= ch;
        tyield(db);
    }
    *out = crc;
    return 0;
}

/* ═══════════════════════════════════════════════════════════════════════════
 *  Sector classification
 *
 *  Reads the sector header from flash and classifies the sector into
 *  one of four states:
 *
 *    TS_EMPTY   — All-0xFF (genuinely erased, verified at 3 points).
 *    TS_ACTIVE  — Valid header, not sealed (count/end_off = 0xFFFF),
 *                 or seal CRC mismatch (degraded, NOT corrupt).
 *    TS_SEALED  — Valid header with finalised count, end_off, and
 *                 matching header CRC.
 *    TS_CORRUPT — Unrecognisable header or failed 3-point erase check.
 *
 *  Design note: a seal CRC mismatch is demoted to TS_ACTIVE (not
 *  TS_CORRUPT) because the sector data may still be intact — only
 *  the finalisation step was interrupted.  This preserves recoverable
 *  data during power-loss scenarios.
 *
 *  @param db       Database handle.
 *  @param s        Sector index to classify.
 *  @param[out] out Receives the raw sector header (may be NULL).
 *  @return         Classification enum value.
 * ═══════════════════════════════════════════════════════════════════════════ */

typedef enum {
    TS_EMPTY = 0,  /**< Sector is fully erased                      */
    TS_ACTIVE = 1, /**< Sector has valid header, not yet sealed     */
    TS_SEALED = 2, /**< Sector is sealed (finalised, read-only)     */
    TS_CORRUPT = 3 /**< Sector header is unrecognisable              */
} ts_cls_t;

/* Retry a flash read once before giving up, to tolerate transient bus errors
   that would otherwise permanently classify a sector as CORRUPT. */
static int ts_read_retry(const rdb_tsdb_t* db, uint32_t addr,
    void* buf, size_t len) {
    if (trd(db, addr, buf, len) == 0)
        return 0;
    return trd(db, addr, buf, len);
}

static ts_cls_t ts_classify(const rdb_tsdb_t* db, uint8_t s,
    rdb_ts_sector_hdr_t* out) {
    uint32_t            addr = tsa(db, s);
    rdb_ts_sector_hdr_t h;
    if (ts_read_retry(db, addr, &h, sizeof(h)) != 0)
        return TS_CORRUPT;
    if (out)
        *out = h;

    /* All-0xFF magic → verify with 3-point erase check.
     * NB: magic == 0xFFFFFFFFu at offset 0 implicitly covers the start-of-sector
     * probe, so the explicit midpoint and end probes below complete a full
     * 3-point check (start/mid/end), matching KVDB's is_erased() behavior. */
    if (h.magic == 0xFFFFFFFFu) {
        uint8_t b[4];

        /* Probe midpoint */
        if (ts_read_retry(db, addr + db->sector_size / 2, b, 4) != 0)
            return TS_CORRUPT;
        if (b[0] != 0xFF || b[1] != 0xFF || b[2] != 0xFF || b[3] != 0xFF)
            return TS_CORRUPT;

        /* Probe last 4 bytes */
        if (ts_read_retry(db, addr + db->sector_size - 4, b, 4) != 0)
            return TS_CORRUPT;
        if (b[0] != 0xFF || b[1] != 0xFF || b[2] != 0xFF || b[3] != 0xFF)
            return TS_CORRUPT;

        return TS_EMPTY;
    }

    /* Validate magic number */
    if (h.magic != RDB_TS_SECTOR_MAGIC)
        return TS_CORRUPT;

    /* Check for sealed sector (count and end_off both finalised) */
    if (h.count != 0xFFFF && h.end_off != 0xFFFF) {
        uint16_t calc = rdb_crc16(&h, 18);
        if (calc == h.hdr_crc)
            return TS_SEALED;
        /* Seal CRC mismatch — degrade to ACTIVE, not CORRUPT.
           The sector data is likely intact; only the seal step
           was interrupted by power loss. */
        return TS_ACTIVE;
    }

    /* Valid header but not sealed → actively being written */
    return TS_ACTIVE;
}

/* ═══════════════════════════════════════════════════════════════════════════
 *  [T-C2 fix] ts_active_info — Extract time_base and write frontier
 *  from a non-head ACTIVE sector
 *
 *  This handles degraded ACTIVE sectors (failed seal or mid-write
 *  crash on a non-head sector).  It reads time_base from the sector
 *  header and scans forward to find the write frontier (first all-0xFF
 *  gap or end of parseable records).
 *
 *  @param db          Database handle.
 *  @param s           Sector index.
 *  @param[out] out_tb Receives the sector's time_base.
 *  @param[out] out_end Receives the byte offset of the write frontier.
 * ═══════════════════════════════════════════════════════════════════════════ */

static void ts_active_info(const rdb_tsdb_t* db, uint8_t s,
    uint32_t* out_tb, uint32_t* out_end) {
    uint32_t base = tsa(db, s);
    uint32_t off = tds(db);
    uint32_t ss = db->sector_size;

    /* Read time_base from sector header */
    rdb_ts_sector_hdr_t h;
    if (trd(db, base, &h, sizeof(h)) != 0) {
        *out_tb = RDB_TIME_INVALID;
        *out_end = (uint32_t)tds(db);
        return;
    }
    *out_tb = h.time_base;

    /* Scan forward to find the write frontier */
    while (off + TS_REC_SZ <= ss) {
        rdb_ts_record_hdr_t rh;
        if (trd(db, base + off, &rh, sizeof(rh)) != 0)
            break;

        /* All-0xFF → erased space, end of record chain */
        if (rh.magic == 0xFFu && rh.state == 0xFFu)
            break;

        /* Bad magic → skip one aligned record width */
        if (rh.magic != RDB_TS_RECORD_MAGIC) {
            off += ts_corrupt_skip(db);
            continue;
        }

        uint32_t rsz = trs(db, rh.data_len);
        if (off + rsz > ss)
            break;
        off += rsz;
    }

    *out_end = off;
}

/* ═══════════════════════════════════════════════════════════════════════════
 *  [T-C4 fix] ts_sector_count — Count VALID records in a sector
 *
 *  For SEALED sectors: returns h.count from the header (fast path).
 *  For ACTIVE sectors: scans and counts VALID records (slow path).
 *  For EMPTY / CORRUPT: returns 0.
 *
 *  This function is read-only — it does NOT perform flash writes
 *  (no WRITING record recovery).  It is a counting helper used for
 *  total_count reconciliation during init and periodic recount.
 *
 *  @param db  Database handle.
 *  @param s   Sector index to count.
 *  @return    Number of VALID records in the sector.
 * ═══════════════════════════════════════════════════════════════════════════ */

static uint16_t ts_sector_count(const rdb_tsdb_t* db, uint8_t s) {
    rdb_ts_sector_hdr_t h;
    ts_cls_t            cls = ts_classify(db, s, &h);

    /* Fast path for sealed sectors with valid count */
    if (cls == TS_SEALED && h.count != 0xFFFFu)
        return h.count;

    /* Only scan ACTIVE or degraded SEALED sectors */
    if (cls != TS_ACTIVE && cls != TS_SEALED)
        return 0;

    /* Scan and count VALID records (read-only, no WRITING repair) */
    uint32_t base = tsa(db, s);
    uint32_t off = tds(db);
    uint32_t ss = db->sector_size;
    uint16_t cnt = 0;

    while (off + TS_REC_SZ <= ss) {
        rdb_ts_record_hdr_t rh;
        if (trd(db, base + off, &rh, sizeof(rh)) != 0)
            break;

        /* End of record chain */
        if (rh.magic == 0xFFu && rh.state == 0xFFu)
            break;

        /* Corrupt record — skip */
        if (rh.magic != RDB_TS_RECORD_MAGIC) {
            off += ts_corrupt_skip(db);
            continue;
        }

        uint32_t rsz = trs(db, rh.data_len);
        if (off + rsz > ss)
            break;

        if (rh.state == RDB_STATE_VALID)
            cnt++;

        off += rsz;
    }

    return cnt;
}

/* ═══════════════════════════════════════════════════════════════════════════
 *  [T-2 fix] ts_init_sec — Erase and initialise a sector
 *
 *  Erases the sector, increments its erase count, and writes a fresh
 *  sector header with the given sequence number.
 *
 *  T-2 fix detail: the erase count takes the maximum of the RAM-cached
 *  value and the value read from the existing flash header.  This
 *  prevents erase count regression when RAM is zeroed (e.g. after a
 *  warm reboot) but flash still holds the true count.
 *
 *  @param db   Database handle.
 *  @param s    Sector index to initialise.
 *  @param seq  Sequence number to assign to the new sector header.
 *  @return     0 on success, -1 on flash failure.
 * ═══════════════════════════════════════════════════════════════════════════ */

static int ts_init_sec(rdb_tsdb_t* db, uint8_t s, uint16_t seq) {
    uint32_t addr = tsa(db, s);
    uint32_t old_ec = db->erase_cnts ? db->erase_cnts[s] : 0;

    /* [T-2 fix]: take max of RAM and flash to prevent ec regression */
    rdb_ts_sector_hdr_t oh;
    if (trd(db, addr, &oh, sizeof(oh)) == 0 &&
        oh.magic == RDB_TS_SECTOR_MAGIC) {
        if (oh.erase_cnt > old_ec)
            old_ec = oh.erase_cnt;
    }

    if (tera(db, addr) != 0)
        return -1;

    /* Write fresh sector header */
    uint32_t            nec = old_ec + 1;
    rdb_ts_sector_hdr_t h;
    memset(&h, 0xFF, sizeof(h)); /* All-0xFF default (unsealed state) */
    h.magic = RDB_TS_SECTOR_MAGIC;
    h.erase_cnt = nec;
    h.time_base = RDB_TIME_INVALID; /* Set later on first append */
    h.seq = (uint16_t)seq;
    /* h.count, h.end_off, h.hdr_crc remain 0xFFFF (unsealed) */

    if (twr_f(db, addr, &h, sizeof(h)) != 0)
        return -1;
    if (db->erase_cnts)
        db->erase_cnts[s] = nec;
    return 0;
}

/* ═══════════════════════════════════════════════════════════════════════════
 *  ts_seal — Finalise (seal) a sector
 *
 *  Writes the record count and end-offset fields into the sector
 *  header, then computes and writes the header CRC to commit the seal.
 *
 *  After sealing, the sector is read-only.  The CRC covers all 18
 *  header bytes (magic through end_off), providing tamper detection.
 *
 *  @param db       Database handle.
 *  @param s        Sector index to seal.
 *  @param count    Number of VALID records in the sector.
 *  @param end_off  Byte offset of the write frontier (first free byte).
 *  @return         0 on success, -1 on flash failure.
 * ═══════════════════════════════════════════════════════════════════════════ */

static int ts_seal(rdb_tsdb_t* db, uint8_t s,
    uint16_t count, uint16_t end_off) {
    uint32_t addr = tsa(db, s);

    /* Write count field (offset 14) */
    if (twr_f(db, addr + 14, &count, 2) != 0)
        return -1;

    /* Write end_off field (offset 16) */
    if (twr_f(db, addr + 16, &end_off, 2) != 0)
        return -1;

    /* Read back full header and compute CRC */
    rdb_ts_sector_hdr_t h;
    if (trd(db, addr, &h, sizeof(h)) != 0)
        return -1;
    uint16_t crc = rdb_crc16(&h, 18); /* CRC covers bytes [0..17] */

    /* Write CRC to commit the seal (offset 18) */
    if (twr_f(db, addr + 18, &crc, 2) != 0)
        return -1;

    return 0;
}

/* ═══════════════════════════════════════════════════════════════════════════
 *  Scan sector records — generic forward scanner with callback
 *
 *  Walks the record chain within a sector starting at tds(), invoking
 *  the callback for each VALID record found.
 *
 *  When @p recover is RDB_TRUE, WRITING records are recovered
 *  (or discarded) in-place on flash:
 *    - If data CRC matches → promote to VALID, invoke callback.
 *    - If data CRC fails   → demote to DEAD, skip.
 *
 *  When @p recover is RDB_FALSE (read-only queries), WRITING records
 *  are silently skipped — the flash is not modified.
 *
 *  @param db         Database handle.
 *  @param s          Sector index to scan.
 *  @param time_base  Sector's time_base (used to compute absolute time).
 *  @param max_off    Scan boundary (typically sector_size or end_off).
 *  @param cb         Callback invoked for each VALID record (may be NULL).
 *  @param arg        Opaque context forwarded to cb.
 *  @param recover    RDB_TRUE to recover WRITING records, RDB_FALSE to skip.
 *  @return           Number of VALID records found during the scan.
 * ═══════════════════════════════════════════════════════════════════════════ */

/** @brief Parsed metadata for one TS record found during a sector scan. */
typedef struct {
    uint32_t time;     /**< Absolute timestamp (time_base + time_delta) */
    uint32_t addr;     /**< Absolute flash address of the record header */
    uint16_t data_len; /**< Data payload length in bytes (unpadded)     */
    uint8_t  state;    /**< Record state after recovery (VALID or DEAD) */
} ts_rec_t;

/** @brief Scan callback function type for TSDB record iteration. */
typedef int (*ts_scan_cb_t)(rdb_tsdb_t* db, const ts_rec_t* r, void* arg);

static uint16_t ts_scan(rdb_tsdb_t* db, uint8_t s,
    uint32_t time_base, uint32_t max_off,
    ts_scan_cb_t cb, void* arg, int recover) {
    uint32_t base = tsa(db, s);
    uint32_t off = tds(db);
    uint16_t cnt = 0;

    while (off + TS_REC_SZ <= max_off) {
        rdb_ts_record_hdr_t rh;
        if (trd(db, base + off, &rh, sizeof(rh)) != 0)
            break;

        /* All-0xFF → erased space, end of record chain */
        if (rh.magic == 0xFFu && rh.state == 0xFFu)
            break;

        /* Bad magic → skip one aligned record width */
        if (rh.magic != RDB_TS_RECORD_MAGIC) {
            off += ts_corrupt_skip(db);
            continue;
        }

        uint32_t rsz = trs(db, rh.data_len);
        if (off + rsz > max_off)
            break;

        uint8_t eff_state = rh.state;

        /* Recover WRITING records in-place (init/recount paths only).
           Read-only queries skip WRITING records to avoid flash wear. */
        if (eff_state == RDB_STATE_WRITING) {
            if (!recover) {
                off += rsz;
                continue;
            }
            uint16_t calc;
            uint32_t da = base + off + TS_REC_SZ;
            if (ts_data_crc(db, da, rh.data_len, &calc) == 0 &&
                calc == rh.data_crc) {
                /* Data intact → promote to VALID */
                uint8_t v = RDB_STATE_VALID;
                if (twr_f(db, base + off + 1, &v, 1) == 0)
                    eff_state = RDB_STATE_VALID;
                /* On write failure: leave as WRITING, will retry next init */
            } else {
                /* Data incomplete/corrupt → demote to DEAD */
                uint8_t d = RDB_STATE_DEAD;
                if (twr_f(db, base + off + 1, &d, 1) == 0)
                    eff_state = RDB_STATE_DEAD;
                /* On write failure: leave as WRITING, will retry next init */
            }
        }

        if (eff_state == RDB_STATE_VALID) {
            cnt++;
            if (cb) {
                ts_rec_t r;
                r.time = (time_base != RDB_TIME_INVALID) ? time_base + rh.time_delta : rh.time_delta;
                r.addr = base + off;
                r.data_len = rh.data_len;
                r.state = RDB_STATE_VALID;
                if (cb(db, &r, arg) == RDB_ITER_STOP)
                    return cnt;
            }
        }

        off += rsz;
    }

    return cnt;
}

/* ═══════════════════════════════════════════════════════════════════════════
 *  [T-C1/C5 fix] ts_rotate — Rotate to a new head sector
 *
 *  Seals the current head sector, advances the head pointer to the
 *  next sector in the ring, and initialises it.  If the new head
 *  overwrites the tail sector (ring is full), the oldest sector's
 *  records are counted and subtracted from total_count before the
 *  tail pointer advances.
 *
 *  [T-8 fix]: periodic total_count recount is NOT done here.  It is
 *  only performed in rdb_tsdb_append() to avoid double-triggering.
 *
 *  @param db  Database handle.
 *  @return    RDB_OK on success, RDB_ERR_FLASH on failure.
 * ═══════════════════════════════════════════════════════════════════════════ */

static rdb_err_t ts_rotate(rdb_tsdb_t* db) {
    /* Step 1: Seal the current head sector */
    if (ts_seal(db, db->head_sec, db->head_count, (uint16_t)db->head_off) != 0)
        return RDB_ERR_FLASH;

    uint8_t next = tnext(db, db->head_sec);

    /* Step 2: Handle ring wrap — oldest sector being overwritten */
    if (next == db->tail_sec && db->sector_cnt > 1u) {
        uint32_t lost = 0;

        /* Read sector header to get time_base and end_off for accurate scan.
           ts_scan() recovers WRITING records in-place (promote/demote) so
           the lost count includes records that were mid-write at crash. */
        rdb_ts_sector_hdr_t h;
        uint8_t old_tail = db->tail_sec;
        if (trd(db, tsa(db, old_tail), &h, sizeof(h)) == 0) {
            uint32_t max_off = (h.end_off != 0xFFFFu) ? h.end_off : db->sector_size;
            lost = ts_scan(db, old_tail, h.time_base, max_off, NULL, NULL, RDB_TRUE);
        }

        if (lost > 0) {
            db->stats.records_lost += lost;
            db->total_count = (db->total_count >= lost) ? db->total_count - lost : 0;
        }

        /* Advance tail past the overwritten sector */
        db->tail_sec = tnext(db, db->tail_sec);
    }

    /* Step 3: Initialise the new head sector */
    db->head_seq++;
    if (ts_init_sec(db, next, db->head_seq) != 0) {
        /* Head is already sealed; set head_off to sector_size to force
           a rotation on the next append rather than writing to a stale
           offset in the now-sealed sector. */
        db->head_off = db->sector_size;
        return RDB_ERR_FLASH;
    }

    db->head_sec = next;
    db->head_off = tds(db);
    db->head_count = 0;
    db->head_time_base = RDB_TIME_INVALID; /* Set on first append */

    db->stats.sector_rotations++;

    /* Step 4: Yield — sector erase is a long operation */
    tyield(db);

    return RDB_OK;
}

/* ═══════════════════════════════════════════════════════════════════════════
 *                          PUBLIC API
 * ═══════════════════════════════════════════════════════════════════════════ */

/* ═══════════════════════════════════════════════════════════════════════════
 *  rdb_tsdb_init — Initialise the TSDB from existing flash contents
 *
 *  Boot sequence (five phases):
 *
 *  Phase 1 — Classify all sectors:
 *    Read sector headers, classify as EMPTY / ACTIVE / SEALED / CORRUPT.
 *    CORRUPT sectors are immediately erased to reclaim space.
 *    Identify the head sector (highest seq) and tail sector (lowest seq).
 *    If no data sectors exist, fall through to rdb_tsdb_format().
 *
 *  Phase 2 — Validate sequence continuity (tail → head):
 *    Walk the ring from tail to head verifying monotonic seq progression.
 *    [T-3 fix]: non-monotonic seq is demoted to a data_gaps counter
 *    increment rather than triggering a full format.  This preserves
 *    all recoverable data.
 *
 *  Phase 3 — Recover head sector state:
 *    If the head sector is SEALED, extract count/end_off/last_time
 *    from the header.  If ACTIVE, scan records, recover WRITING
 *    records (CRC-based), and determine the write frontier.
 *
 *  Phase 4 — Count total records across the ring:
 *    Walk from tail to head summing ts_sector_count() per sector.
 *    The head sector uses head_count (already computed in Phase 3).
 *
 *  Phase 5 — last_time fallback:
 *    If last_time is still 0 but total_count > 0, scan all sectors
 *    to find the maximum timestamp (handles edge cases where the
 *    head sector was sealed with a degraded time_base).
 *
 *  @param db      Database handle (caller-allocated, zeroed on entry).
 *  @param part    Partition descriptor (must outlive the database).
 *  @param ec_buf  Caller-allocated array for per-sector erase counts;
 *                 size = rdb_tsdb_ec_size(sector_count).  May be NULL
 *                 if erase count tracking is not needed.
 *  @return        RDB_OK on success, or an error code.
 * ═══════════════════════════════════════════════════════════════════════════ */

rdb_err_t rdb_tsdb_init(rdb_tsdb_t* db, const rdb_partition_t* part,
    uint32_t* ec_buf) {
    /* ── Parameter validation ── */
    if (!db || !part || !part->ops)
        return RDB_ERR_PARAM;
    if (!part->ops->read || !part->ops->write || !part->ops->erase)
        return RDB_ERR_PARAM;
    if (part->sector_size < RDB_MIN_SECTOR_SIZE)
        return RDB_ERR_PARAM;
    if (part->sector_size & (part->sector_size - 1))
        return RDB_ERR_PARAM;
    if (part->total_size % part->sector_size)
        return RDB_ERR_PARAM;
    if (part->write_gran > 3)
        return RDB_ERR_PARAM;

    uint32_t scnt = part->total_size / part->sector_size;
    if (scnt < RDB_TS_MIN_SECTORS || scnt > RDB_MAX_SECTORS)
        return RDB_ERR_PARAM;

    /* ── Zero-initialise the handle ── */
    memset(db, 0, sizeof(*db));
    db->part = part;
    db->erase_cnts = ec_buf;
    db->sector_size = part->sector_size;
    db->sector_cnt = (uint8_t)scnt;

    /* Guard tdc() against underflow: sector must hold at least the header */
    if (db->sector_size <= tds(db))
        return RDB_ERR_PARAM;

    /* ── Compute max_data_len ──
       The maximum data payload that can fit in a single record within
       one sector.  Respects RDB_MAX_TS_DATA_LEN if configured. */
    {
        uint32_t dcap = tdc(db);
        uint32_t mdl = dcap - TS_REC_SZ;
        while (mdl > 0 && trs(db, (uint16_t)mdl) > dcap)
            mdl--;
        if (mdl > 0xFFFFu)
            mdl = 0xFFFFu;

#if RDB_MAX_TS_DATA_LEN > 0
        db->max_data_len = (uint16_t)RDB_MIN(mdl,
            (uint32_t)RDB_MAX_TS_DATA_LEN);
#else
        db->max_data_len = (uint16_t)mdl;
#endif
    }

    /* [T-EC-PERSIST fix]: do NOT zero ec_buf here.  Erase counts for
     * EMPTY sectors exist only in RAM; zeroing them would discard the
     * accumulated wear history.  For sectors with valid flash headers
     * take the maximum of RAM and flash to prevent regression. */

    /* ══════════════════════════════════════════════════════════════════
     *  Phase 1: Classify all sectors, find head/tail
     * ══════════════════════════════════════════════════════════════════ */
    int      has_data = 0;
    uint8_t  head_s = 0xFF, tail_s = 0xFF;
    uint16_t head_sq = 0, tail_sq = 0;

    for (uint8_t s = 0; s < db->sector_cnt; s++) {
        rdb_ts_sector_hdr_t h;
        ts_cls_t            cls = ts_classify(db, s, &h);

        if (cls == TS_CORRUPT) {
            /* Attempt to reclaim corrupt sectors by erasing.
             * [T-EC-PERSIST fix]: do NOT reset ec to 1 or 0.
             * The prior ec_buf[s] value is the best estimate. */
            if (tera(db, tsa(db, s)) != 0)
                db->stats.flash_errors++;
            /* ec_buf[s] untouched — keep pre-existing wear history */
            continue;
        }
        if (cls == TS_EMPTY)
            continue; /* ec_buf[s] untouched — keep pre-existing value */

        has_data = 1;
        /* [T-2 fix + T-EC-PERSIST]: take max of RAM and flash ec */
        if (ec_buf && h.erase_cnt > ec_buf[s])
            ec_buf[s] = h.erase_cnt;

        /* Track head (highest seq) and tail (lowest seq) */
        if (head_s == 0xFF || RDB_SEQ16_GT(h.seq, head_sq)) {
            head_s = s;
            head_sq = h.seq;
        }
        if (tail_s == 0xFF || RDB_SEQ16_GT(tail_sq, h.seq)) {
            tail_s = s;
            tail_sq = h.seq;
        }
    }

    /* No data sectors found — fresh format */
    if (!has_data)
        return rdb_tsdb_format(db);

    db->tail_sec = tail_s;

    /* ══════════════════════════════════════════════════════════════════
     *  Phase 2: Validate sequence continuity (tail → head)
     *
     *  [T-3 fix]: non-monotonic seq no longer triggers format.
     *  Instead, increment data_gaps counter and continue.
     *  This preserves all recoverable data after power-loss scenarios
     *  that may have left gaps in the ring.
     * ══════════════════════════════════════════════════════════════════ */
    {
        uint8_t  s = tail_s;
        uint16_t prev_seq = tail_sq;
        int      first = 1;

        for (uint8_t i = 0; i < db->sector_cnt; i++) {
            rdb_ts_sector_hdr_t h;
            ts_cls_t            cls = ts_classify(db, s, &h);

            if (cls == TS_ACTIVE || cls == TS_SEALED) {
                if (!first && !RDB_SEQ16_GT(h.seq, prev_seq) &&
                    h.seq != prev_seq) {
                    /* [T-3 fix]: demoted from full format to warning */
                    db->stats.data_gaps++;
                }
                prev_seq = h.seq;
                first = 0;
            } else if (cls == TS_EMPTY) {
                /* EMPTY gap in ring — note but don't panic */
                db->stats.data_gaps++;
            }

            if (s == head_s)
                break;
            s = tnext(db, s);
        }
    }

    /* ══════════════════════════════════════════════════════════════════
     *  Phase 3: Recover head sector state
     * ══════════════════════════════════════════════════════════════════ */
    rdb_ts_sector_hdr_t hh;
    ts_cls_t            hcls = ts_classify(db, head_s, &hh);

    db->head_sec = head_s;
    db->head_seq = hh.seq;
    db->last_time = 0;

    if (hcls == TS_SEALED) {
        /* Head sector is sealed — extract metadata from header */
        db->head_time_base = hh.time_base;
        db->head_count = hh.count;
        db->head_off = hh.end_off;

        /* Find last_time from the sealed head sector's records */
        if (hh.time_base != RDB_TIME_INVALID && hh.count > 0) {
            uint32_t base = tsa(db, head_s);
            uint32_t off = tds(db);

            while (off + TS_REC_SZ <= hh.end_off) {
                rdb_ts_record_hdr_t rh;
                if (trd(db, base + off, &rh, sizeof(rh)) != 0)
                    break;
                if (rh.magic != RDB_TS_RECORD_MAGIC) {
                    off += ts_corrupt_skip(db);
                    continue;
                }
                uint32_t rsz = trs(db, rh.data_len);
                if (off + rsz > hh.end_off)
                    break;
                if (rh.state == RDB_STATE_VALID) {
                    uint32_t t = hh.time_base + rh.time_delta;
                    if (t > db->last_time)
                        db->last_time = t;
                }
                off += rsz;
            }
        }

        /* Sealed head is full — next append will trigger rotation */
        db->head_off = db->sector_size;

    } else {
        /* ACTIVE head — scan records, recover WRITING, find frontier */
        db->head_time_base = hh.time_base;
        db->head_count = 0;

        uint32_t base = tsa(db, head_s);
        uint32_t off = tds(db);
        uint32_t ss = db->sector_size;

        while (off + TS_REC_SZ <= ss) {
            rdb_ts_record_hdr_t rh;
            if (trd(db, base + off, &rh, sizeof(rh)) != 0)
                break;

            /* End of record chain */
            if (rh.magic == 0xFFu && rh.state == 0xFFu)
                break;

            /* Corrupt record header — skip */
            if (rh.magic != RDB_TS_RECORD_MAGIC) {
                off += ts_corrupt_skip(db);
                continue;
            }

            uint32_t rsz = trs(db, rh.data_len);
            if (off + rsz > ss)
                break;

            /* Recover WRITING records via CRC check */
            if (rh.state == RDB_STATE_WRITING) {
                uint16_t calc;
                uint32_t da = base + off + TS_REC_SZ;
                if (ts_data_crc(db, da, rh.data_len, &calc) == 0 &&
                    calc == rh.data_crc) {
                    /* Data intact → promote to VALID */
                    uint8_t v = RDB_STATE_VALID;
                    if (twr_f(db, base + off + 1, &v, 1) == 0) {
                        db->head_count++;
                        if (db->head_time_base != RDB_TIME_INVALID) {
                            uint32_t t = db->head_time_base + rh.time_delta;
                            if (t > db->last_time)
                                db->last_time = t;
                        }
                    }
                    /* On write failure: leave as WRITING, will retry next init */
                } else {
                    /* Data incomplete → demote to DEAD */
                    uint8_t d = RDB_STATE_DEAD;
                    twr_f(db, base + off + 1, &d, 1);
                    /* On write failure: leave as WRITING, will retry next init */
                }
            } else if (rh.state == RDB_STATE_VALID) {
                db->head_count++;
                if (db->head_time_base != RDB_TIME_INVALID) {
                    uint32_t t = db->head_time_base + rh.time_delta;
                    if (t > db->last_time)
                        db->last_time = t;
                }
            }
            off += rsz;
        }
        db->head_off = off;
    }

    /* ══════════════════════════════════════════════════════════════════
     *  Phase 4: Count total records across the ring
     *
     *  Walk from tail to head, summing record counts per sector.
     *  Non-head sectors use ts_sector_count() (handles SEALED and
     *  degraded ACTIVE uniformly).  The head sector uses head_count
     *  computed during Phase 3.
     * ══════════════════════════════════════════════════════════════════ */
    db->total_count = 0;
    {
        uint8_t s = db->tail_sec;
        for (uint8_t i = 0; i < db->sector_cnt; i++) {
            if (s == db->head_sec) {
                db->total_count += db->head_count;
                break;
            }

            db->total_count += ts_sector_count(db, s);

            s = tnext(db, s);
        }
    }

    /* ══════════════════════════════════════════════════════════════════
     *  Phase 5: last_time fallback
     *
     *  If last_time is still 0 but records exist, scan all sectors
     *  backwards from head to find the maximum timestamp.  This
     *  handles edge cases where the head sector's time_base was
     *  RDB_TIME_INVALID or the head was freshly sealed.
     * ══════════════════════════════════════════════════════════════════ */
    if (db->last_time == 0 && db->total_count > 0) {
        uint8_t s = db->tail_sec;
        for (uint8_t i = 0; i < db->sector_cnt; i++) {
            rdb_ts_sector_hdr_t h;
            ts_cls_t            cls = ts_classify(db, s, &h);
            uint32_t            tb, eo;

            if (cls == TS_SEALED && h.time_base != RDB_TIME_INVALID) {
                tb = h.time_base;
                eo = h.end_off;
            } else if (cls == TS_ACTIVE) {
                ts_active_info(db, s, &tb, &eo);
            } else {
                goto next_fallback;
            }

            /* Scan sector for maximum timestamp */
            if (tb != RDB_TIME_INVALID) {
                uint32_t base = tsa(db, s);
                uint32_t off = tds(db);
                while (off + TS_REC_SZ <= eo) {
                    rdb_ts_record_hdr_t rh;
                    if (trd(db, base + off, &rh, sizeof(rh)) != 0)
                        break;
                    if (rh.magic != RDB_TS_RECORD_MAGIC) {
                        off += ts_corrupt_skip(db);
                        continue;
                    }
                    uint32_t rsz = trs(db, rh.data_len);
                    if (off + rsz > eo)
                        break;
                    if (rh.state == RDB_STATE_VALID) {
                        uint32_t t = tb + rh.time_delta;
                        if (t > db->last_time)
                            db->last_time = t;
                    }
                    off += rsz;
                }
            }

        next_fallback:
            if (s == db->head_sec)
                break;
            s = tnext(db, s);
        }
    }

    db->initialized = 1;
    return RDB_OK;
}

/* ═══════════════════════════════════════════════════════════════════════════
 *  rdb_tsdb_format — Erase all sectors and reinitialise the TSDB
 *
 *  Preserves erase counts from flash headers (wear-leveling continuity).
 *  After format, sector 0 is the head/tail with an empty record log.
 *
 *  Sector 0 is NOT erased in the Pass 2 loop; it is erased inside
 *  ts_init_sec() to avoid a double-erase that would increment its
 *  erase count twice.
 *
 *  @param db  Database handle (must have part assigned).
 *  @return    RDB_OK on success, or an error code.
 * ═══════════════════════════════════════════════════════════════════════════ */

rdb_err_t rdb_tsdb_format(rdb_tsdb_t* db) {
    if (!db || !db->part)
        return RDB_ERR_PARAM;

    /* Validate sector count */
    {
        uint32_t scnt32 = db->part->total_size / db->part->sector_size;
        if (scnt32 < RDB_TS_MIN_SECTORS || scnt32 > RDB_MAX_SECTORS)
            return RDB_ERR_PARAM;
        if (db->sector_cnt != 0 && db->sector_cnt != (uint8_t)scnt32)
            return RDB_ERR_PARAM;
    }

    tlock(db);

    uint8_t scnt = (uint8_t)(db->part->total_size / db->part->sector_size);

    /* Pass 1: collect erase counts from flash */
    static uint32_t saved_ec[RDB_MAX_SECTORS];
    for (uint8_t s = 0; s < scnt; s++) {
        saved_ec[s] = 0;
        if (db->erase_cnts)
            saved_ec[s] = db->erase_cnts[s];

        /* Also check flash header for a potentially higher count.
         * [T-CRC-FMT fix]: verify CRC for sealed sectors before trusting
         * erase_cnt.  Unsealed sectors (hdr_crc == 0xFFFF) have no CRC
         * to verify but magic matched, so their erase_cnt is trusted. */
        rdb_ts_sector_hdr_t h;
        uint32_t            addr = tsa(db, s);
        if (trd(db, addr, &h, sizeof(h)) == 0 &&
            h.magic == RDB_TS_SECTOR_MAGIC) {
            if (h.hdr_crc != 0xFFFFu &&
                rdb_crc16(&h, 18) != h.hdr_crc)
                continue; /* Sealed sector with bad CRC — unreliable */
            if (h.erase_cnt > saved_ec[s])
                saved_ec[s] = h.erase_cnt;
        }
    }

    /* Pass 2: erase sectors 1..N-1 (sector 0 handled by ts_init_sec) */
    for (uint8_t s = 1; s < scnt; s++) {
        if (tera(db, tsa(db, s)) != 0) {
            tunlock(db);
            return RDB_ERR_FLASH;
        }
        if (db->erase_cnts)
            db->erase_cnts[s] = saved_ec[s] + 1;
    }

    /* Sector 0: pre-load ec so ts_init_sec picks up the correct value */
    if (db->erase_cnts)
        db->erase_cnts[0] = saved_ec[0];

    /* ts_init_sec erases sector 0 internally, incrementing ec once */
    db->head_seq = 1;
    if (ts_init_sec(db, 0, db->head_seq) != 0) {
        tunlock(db);
        return RDB_ERR_FLASH;
    }

    /* Reset all runtime state */
    db->head_sec = 0;
    db->tail_sec = 0;
    db->head_off = tds(db);
    db->head_count = 0;
    db->head_time_base = RDB_TIME_INVALID;
    db->last_time = 0;
    db->total_count = 0;
    db->initialized = 1;
    memset(&db->stats, 0, sizeof(db->stats));

    tunlock(db);
    return RDB_OK;
}

/* ═══════════════════════════════════════════════════════════════════════════
 *  rdb_tsdb_append — Append a time-series record
 *
 *  Writes a new data record at the head sector's write frontier.
 *  If the head sector is full or the timestamp would overflow the
 *  sector's time_base range, a rotation is triggered first.
 *
 *  Two-phase write protocol:
 *    Phase A: Write record header (state = WRITING) + data payload.
 *    Phase B: Commit by writing state = VALID (1→0 bit flip).
 *
 *  Timestamp monotonicity is enforced: if the provided time is ≤
 *  last_time or is 0/INVALID, it is auto-corrected to last_time + 1.
 *
 *  [T-8 fix]: periodic total_count reconciliation is performed here
 *  (not in ts_rotate) every full ring cycle to eliminate accumulated
 *  drift from edge cases.
 *
 *  @param db    Database handle.
 *  @param time  Desired timestamp (0 or RDB_TIME_INVALID for auto).
 *  @param data  Pointer to data payload.
 *  @param len   Data length in bytes (1..max_data_len).
 *  @return      RDB_OK on success, or an error code.
 * ═══════════════════════════════════════════════════════════════════════════ */

rdb_err_t rdb_tsdb_append(rdb_tsdb_t* db, uint32_t time,
    const void* data, uint16_t len) {
    if (!db || !db->initialized)
        return RDB_ERR_NOT_INIT;
    if (len < 1 || len > db->max_data_len)
        return RDB_ERR_TOO_LARGE;
    if (!data)
        return RDB_ERR_PARAM;

    tlock(db);

    /* Check for time exhaustion */
    if (db->last_time >= RDB_TIME_MAX) {
        tunlock(db);
        return RDB_ERR_TIME_EXHAUSTED;
    }

    /* Monotonic time enforcement */
    if (time == 0 || time == RDB_TIME_INVALID)
        time = db->last_time + 1u;
    else if (db->last_time != 0 && time <= db->last_time)
        time = db->last_time + 1u;

    if (time > RDB_TIME_MAX) {
        tunlock(db);
        return RDB_ERR_TIME_EXHAUSTED;
    }

    uint32_t rsz = trs(db, len);

    /* ── Determine if rotation is needed ── */
    int need_rot = 0;

    /* Case 1: time_base overflow — timestamp cannot be represented
       as a delta from the current sector's time_base */
    if (db->head_time_base != RDB_TIME_INVALID) {
        if (time < db->head_time_base ||
            (time - db->head_time_base) > 0xFFFFFFFEu)
            need_rot = 1;
    }

    /* Case 2: sector space exhausted */
    if (!need_rot && db->head_off + rsz > db->sector_size)
        need_rot = 1;

    if (need_rot) {
        rdb_err_t rrc = ts_rotate(db);
        if (rrc != RDB_OK) {
            tunlock(db);
            return rrc;
        }
    }

    /* ── Set time_base for new sector if needed ── */
    if (db->head_time_base == RDB_TIME_INVALID) {
        uint32_t tb_off = tsa(db, db->head_sec) + 8; /* time_base at offset 8 */
        if (twr_f(db, tb_off, &time, 4) != 0) {
            db->stats.flash_errors++;
            tunlock(db);
            return RDB_ERR_FLASH;
        }
        db->head_time_base = time;
    }

    uint32_t delta = time - db->head_time_base;

    /* ── Build record header ── */
    uint16_t            dcrc = rdb_crc16(data, len);
    rdb_ts_record_hdr_t rh;
    rh.magic = RDB_TS_RECORD_MAGIC;
    rh.state = RDB_STATE_WRITING;
    rh.data_len = len;
    rh.time_delta = delta;
    rh.data_crc = dcrc;
    rh._pad = 0xFFFF;

    uint32_t wa = tsa(db, db->head_sec) + db->head_off;
    uint32_t da = RDB_ALIGN_UP(len, twr(db)); /* Aligned data size */

    /* ── Write record — merged path for small records ── */
    if (rsz <= RDB_STACK_BUF_SIZE) {
        /* Small record: assemble header + data + padding in one
           stack buffer and write as a single flash operation. */
        uint8_t mbuf[RDB_STACK_BUF_SIZE];
        memcpy(mbuf, &rh, TS_REC_SZ);
        memcpy(mbuf + TS_REC_SZ, data, len);
        if (da > len)
            memset(mbuf + TS_REC_SZ + len, 0xFF, da - len);

        if (twr_f(db, wa, mbuf, rsz) != 0) {
            db->stats.flash_errors++;
            tunlock(db);
            return RDB_ERR_FLASH;
        }
    } else {
        /* Large record: multi-step write */

        /* Write record header */
        if (twr_f(db, wa, &rh, sizeof(rh)) != 0) {
            db->stats.flash_errors++;
            tunlock(db);
            return RDB_ERR_FLASH;
        }

        /* Write data payload */
        if (twr_f(db, wa + TS_REC_SZ, data, len) != 0) {
            db->stats.flash_errors++;
            tunlock(db);
            return RDB_ERR_FLASH;
        }

        /* Write alignment padding (0xFF fill) */
        if (da > len) {
            uint32_t pad_rem = da - len;
            uint32_t pad_pos = wa + TS_REC_SZ + len;
            uint8_t  ff[8];
            memset(ff, 0xFF, sizeof(ff));
            while (pad_rem > 0) {
                uint32_t pw = RDB_MIN(pad_rem, (uint32_t)sizeof(ff));
                twr_f(db, pad_pos, ff, pw);
                pad_pos += pw;
                pad_rem -= pw;
            }
        }
    }

    /* ── Phase B: commit — WRITING → VALID (single byte, 1→0 transition) ── */
    {
        uint8_t v = RDB_STATE_VALID;
        if (twr_f(db, wa + 1, &v, 1) != 0) {
            db->stats.flash_errors++;
            tunlock(db);
            return RDB_ERR_FLASH;
        }
    }

    /* ── Bookkeeping ── */
    db->head_off += rsz;
    db->head_count++;
    db->last_time = time;
    db->total_count++;
    db->stats.write_ops++;

    /* ── [T-8 fix] Periodic total_count reconciliation ──
       Every full ring cycle (sector_rotations is a multiple of
       sector_cnt), re-derive total_count from actual sector data
       to eliminate accumulated drift from edge-case bookkeeping
       errors.  Uses ts_scan() in read-only mode to avoid
       unexpected flash writes during an append operation. */
    if (db->stats.sector_rotations > 0 &&
        (db->stats.sector_rotations % db->sector_cnt) == 0) {
        uint32_t recount = 0;
        uint8_t  s = db->tail_sec;
        for (uint8_t i = 0; i < db->sector_cnt; i++) {
            if (s == db->head_sec) {
                recount += db->head_count;
                break;
            }
            rdb_ts_sector_hdr_t h;
            if (trd(db, tsa(db, s), &h, sizeof(h)) == 0) {
                uint32_t max_off = (h.end_off != 0xFFFFu) ? h.end_off
                                                          : db->sector_size;
                recount += ts_scan(db, s, h.time_base, max_off, NULL, NULL, RDB_FALSE);
            }
            s = tnext(db, s);
        }
        db->total_count = recount;
    }

    tunlock(db);
    return RDB_OK;
}

/* ═══════════════════════════════════════════════════════════════════════════
 *  rdb_tsdb_reset_epoch — Force a time epoch boundary
 *
 *  ┌────────────────────────────────────────────────────────────────────┐
 *  │ Epoch Management & Time-Wrap Recovery                              │
 *  ├────────────────────────────────────────────────────────────────────┤
 *  │                                                                    │
 *  │ **What is an Epoch?**:                                             │
 *  │   • Logical boundary between segments of time-ordered data        │
 *  │   • Each sector in TSDB has an associated time_base (epoch start) │
 *  │   • Timestamps within sector: [time_base, time_base + ~32-bits]  │
 *  │   • When time_base overflows or wraps → create new epoch         │
 *  │                                                                    │
 *  │ **Why Epochs are Needed**:                                         │
 *  │   Problem: Time source may wrap (32-bit RTC, millisecond timer)  │
 *  │     • Device boots with time=0                                    │
 *  │     • Accumulates timestamps: 1000, 2000, 3000, ..., 2^32-1     │
 *  │     • Then wraps: 0, 1, 2, ... (backwards in chronological log!) │
 *  │     • Database queries fail: no way to distinguish old t=1       │
 *  │       from new t=1 after wrap                                     │
 *  │                                                                    │
 *  │   Solution: Reset epoch before wrap occurs                         │
 *  │     • Seal current active sector (finalize this epoch)           │
 *  │     • Start fresh sector with new time_base                      │
 *  │     • Subsequent timestamps relative to new base                 │
 *  │     • Query logic: compare sector time_base first, then offset   │
 *  │                                                                    │
 *  │ **Epoch Rotation Process** (what reset_epoch does):               │
 *  │   1. Call ts_rotate(): Seal current active sector                │
 *  │      • Write SECTOR_STATUS_SEALED to sector header               │
 *  │      • Mark sector start/end time in metadata                    │
 *  │      • Allocate new sector as active                             │
 *  │                                                                    │
 *  │   2. Reset last_time = 0                                          │
 *  │      • Prepare for fresh timestamps                               │
 *  │      • Monotonicity check: next append must be ≥ 0               │
 *  │                                                                    │
 *  │   3. Reset head_time_base = RDB_TIME_INVALID                     │
 *  │      • New sector will compute time_base on first append         │
 *  │      • Decoupled from previous epoch's time reference            │
 *  │                                                                    │
 *  │ **Recovery from Time Wrap**:                                       │
 *  │   Scenario: 32-bit millisecond timer wraps every ~49 days       │
 *  │     • Day 48: time ≈ 4 billion ms                                 │
 *  │     • Day 49: time wraps to 0 ms (backwards!)                    │
 *  │     • Application detects: if (new_time < last_time) wrapped      │
 *  │     • Action: call rdb_tsdb_reset_epoch()                        │
 *  │                                                                    │
 *  │   Result:                                                          │
 *  │     • Sectors 0-N: Sealed, time_base=[T0, TN]                   │
 *  │     • Sector N+1: Active, time_base=(new epoch, fresh start)     │
 *  │     • Query: Must check sector time_base ranges first            │
 *  │     • Result: complete isolation between epochs                  │
 *  │                                                                    │
 *  │ **Query Behavior Across Epochs**:                                 │
 *  │   Single-epoch query: [from_ts, to_ts]                           │
 *  │     • Scans sectors in ring order (tail → head)                  │
 *  │     • Within each sector: linear scan of records                 │
 *  │                                                                    │
 *  │   Multi-epoch query: time wraps, so [from_ts, to_ts] may span    │
 *  │     • Query must account for time_base offsets                   │
 *  │     • Sort results if needed (optional)                           │
 *  │                                                                    │
 *  │ **Power-Loss Safety**:                                             │
 *  │   • ts_rotate() atomically seals sector header                   │
 *  │   • Last_time/head_time_base are volatile (RAM only)            │
 *  │     → Lost on power-loss (acceptable)                             │
 *  │   • On reboot: Recover time_base from sealed sector headers      │
 *  │   • Query logic auto-adjusts (no hardcoded time state needed)    │
 *  │                                                                    │
 *  └────────────────────────────────────────────────────────────────────┘
 *
 *  Seals the current head sector and rotates to a fresh one.
 *  Resets last_time and head_time_base so the next append can start
 *  from any timestamp.
 *
 *  Use case: when the application's time source wraps around (e.g.
 *  32-bit RTC overflow) and timestamps would go backwards.
 *
 *  @param db  Database handle.
 *  @return    RDB_OK on success, or an error code.
 * ═══════════════════════════════════════════════════════════════════════════ */

rdb_err_t rdb_tsdb_reset_epoch(rdb_tsdb_t* db) {
    if (!db || !db->initialized)
        return RDB_ERR_NOT_INIT;
    tlock(db);

    rdb_err_t rc = ts_rotate(db);
    if (rc != RDB_OK) {
        tunlock(db);
        return rc;
    }

    db->last_time = 0;
    db->head_time_base = RDB_TIME_INVALID;

    tunlock(db);
    return RDB_OK;
}

/* ═══════════════════════════════════════════════════════════════════════════
 *  Query — iterate over records within a time range
 *
 *  Walks the ring from tail to head, invoking the user callback for
 *  each VALID record whose timestamp falls within [from, to].
 *
 *  For each record:
 *    1. Check time range bounds (skip if outside [from, to]).
 *    2. Verify data CRC; if invalid, invoke callback with NULL data.
 *    3. Read data payload into stack or user-provided buffer.
 *    4. Invoke callback with timestamp, data pointer, and length.
 *
 *  The callback may return RDB_ITER_STOP to abort the query early.
 *
 *  Degraded ACTIVE sectors (T-6 fix) in the ring body are handled
 *  by using ts_active_info() to recover their time_base and write
 *  frontier.
 * ═══════════════════════════════════════════════════════════════════════════ */

/** @brief Internal context for the query scan callback. */
typedef struct {
    rdb_tsdb_t* db;      /**< Database handle                            */
    rdb_ts_cb_t cb;      /**< User callback                              */
    void*       arg;     /**< User context for callback                  */
    void*       rbuf;    /**< Optional user-provided read buffer         */
    uint16_t    rlen;    /**< Size of rbuf                               */
    uint32_t    from;    /**< Query start timestamp (inclusive)           */
    uint32_t    to;      /**< Query end timestamp (inclusive)             */
    int         stopped; /**< Set to 1 if callback returned ITER_STOP    */
} ts_qctx_t;

/**
 * @brief Scan callback for query — filters by time range, verifies CRC,
 *        reads data, and invokes the user callback.
 */
static int ts_qcb(rdb_tsdb_t* db, const ts_rec_t* r, void* arg) {
    ts_qctx_t* q = (ts_qctx_t*)arg;

    /* Time range filter */
    if (r->time > q->to)
        return RDB_ITER_STOP; /* Past end → done  */
    if (r->time < q->from)
        return RDB_ITER_CONTINUE; /* Before start     */

    /* Read record header for CRC verification */
    rdb_ts_record_hdr_t rh;
    if (trd(db, r->addr, &rh, sizeof(rh)) != 0) {
        /* Flash read failure — report record with NULL data */
        if (q->cb(r->time, NULL, r->data_len, q->arg) == RDB_ITER_STOP) {
            q->stopped = 1;
            return RDB_ITER_STOP;
        }
        return RDB_ITER_CONTINUE;
    }

    /* Verify data CRC */
    uint16_t calc;
    uint32_t da = r->addr + TS_REC_SZ;
    if (ts_data_crc(db, da, r->data_len, &calc) != 0 ||
        calc != rh.data_crc) {
        /* CRC mismatch — report record with NULL data */
        db->stats.crc_errors++;
        if (q->cb(r->time, NULL, r->data_len, q->arg) == RDB_ITER_STOP) {
            q->stopped = 1;
            return RDB_ITER_STOP;
        }
        return RDB_ITER_CONTINUE;
    }

    /* Read data payload */
    const void* dp = NULL;
    uint8_t     sbuf[RDB_STACK_BUF_SIZE];

    if (r->data_len <= RDB_STACK_BUF_SIZE) {
        /* Small data: use stack buffer */
        if (trd(db, da, sbuf, r->data_len) == 0)
            dp = sbuf;
    } else if (q->rbuf && q->rlen >= r->data_len) {
        /* Large data: use caller-provided buffer */
        if (trd(db, da, q->rbuf, r->data_len) == 0)
            dp = q->rbuf;
    }
    /* If dp is still NULL, data is too large and no buffer was provided.
       The callback receives NULL data with the correct data_len so the
       caller can decide how to handle it. */

    db->stats.read_ops++;

    if (q->cb(r->time, dp, r->data_len, q->arg) == RDB_ITER_STOP) {
        q->stopped = 1;
        return RDB_ITER_STOP;
    }
    return RDB_ITER_CONTINUE;
}

/**
 * @brief Internal query implementation shared by rdb_tsdb_query and
 *        rdb_tsdb_query_ex.
 */
static rdb_err_t ts_query_impl(rdb_tsdb_t* db, uint32_t from, uint32_t to,
    rdb_ts_cb_t cb, void* arg,
    void* rbuf, uint16_t rlen) {
    if (!db || !db->initialized)
        return RDB_ERR_NOT_INIT;
    if (!cb)
        return RDB_ERR_PARAM;

    tlock(db);

    if (to == 0)
        to = RDB_TIME_MAX;
    if (from > to) {
        tunlock(db);
        return RDB_OK;
    }

    ts_qctx_t q = {
        .db = db, .cb = cb, .arg = arg, .rbuf = rbuf, .rlen = rlen, .from = from, .to = to, .stopped = 0};

    /* Walk the ring from tail to head */
    uint8_t s = db->tail_sec;
    for (uint8_t i = 0; i < db->sector_cnt && !q.stopped; i++) {
        rdb_ts_sector_hdr_t h;
        ts_cls_t            cls = ts_classify(db, s, &h);

        if (cls == TS_SEALED && h.time_base != RDB_TIME_INVALID) {
            /* Sealed sector with valid time_base — scan normally */
            ts_scan(db, s, h.time_base, h.end_off, ts_qcb, &q, RDB_FALSE);

        } else if (cls == TS_ACTIVE && s == db->head_sec &&
                   db->head_time_base != RDB_TIME_INVALID &&
                   db->head_count > 0) {
            /* Active head sector with records — scan up to head_off */
            ts_scan(db, s, db->head_time_base, db->head_off, ts_qcb, &q, RDB_FALSE);

        } else if (cls == TS_ACTIVE && s != db->head_sec) {
            /* [T-6 related] Degraded ACTIVE sector in ring body —
               recover time_base and write frontier, then scan */
            uint32_t tb, eo;
            ts_active_info(db, s, &tb, &eo);
            if (tb != RDB_TIME_INVALID && eo > tds(db))
                ts_scan(db, s, tb, eo, ts_qcb, &q, RDB_FALSE);
        }

        if (s == db->head_sec)
            break;
        s = tnext(db, s);
    }

    tunlock(db);
    return RDB_OK;
}

/**
 * @brief Query records within a time range using only the internal stack buffer.
 *
 * Records larger than RDB_STACK_BUF_SIZE will have NULL data in the callback.
 */
rdb_err_t rdb_tsdb_query(rdb_tsdb_t* db, uint32_t from, uint32_t to,
    rdb_ts_cb_t cb, void* arg) {
    return ts_query_impl(db, from, to, cb, arg, NULL, 0);
}

/**
 * @brief Query records within a time range with a user-provided read buffer.
 *
 * The read_buf is used for records larger than RDB_STACK_BUF_SIZE,
 * enabling zero-copy reads of large payloads.
 */
rdb_err_t rdb_tsdb_query_ex(rdb_tsdb_t* db, uint32_t from, uint32_t to,
    rdb_ts_cb_t cb, void* arg,
    void* read_buf, uint16_t buf_len) {
    return ts_query_impl(db, from, to, cb, arg, read_buf, buf_len);
}

/* ═══════════════════════════════════════════════════════════════════════════
 *  rdb_tsdb_get_latest — Retrieve the most recent record
 *
 *  Searches from the head sector backwards through the ring to find
 *  the last VALID record.  Verifies data CRC before returning.
 *
 *  Degraded ACTIVE sectors in the ring body are handled via
 *  ts_active_info() (T-6 fix).
 *
 *  @param db        Database handle.
 *  @param[out] time Receives the record's timestamp (may be NULL).
 *  @param buf       Buffer to receive the data payload (may be NULL).
 *  @param buf_len   Size of buf in bytes.
 *  @param[out] out_len  Receives the actual data length (may be NULL).
 *  @return          RDB_OK, RDB_ERR_NOT_FOUND, RDB_ERR_CRC, etc.
 * ═══════════════════════════════════════════════════════════════════════════ */

/** @brief Context for finding the last/first VALID record in a sector. */
typedef struct {
    uint32_t time;  /**< Absolute timestamp of the found record      */
    uint32_t addr;  /**< Flash address of the found record header    */
    uint16_t dlen;  /**< Data length of the found record             */
    int      found; /**< 1 if a matching record was found            */
} ts_lt_ctx_t;

/**
 * @brief Find the last VALID record in a sector (read-only scan).
 *
 * Scans forward through the sector keeping track of the last VALID
 * record seen.  Does NOT perform WRITING recovery.
 *
 * @param db         Database handle.
 * @param s          Sector index to scan.
 * @param time_base  Sector's time_base for absolute time computation.
 * @param end_off    Scan boundary (end_off for sealed, write frontier for active).
 * @param[out] c     Result context — c->found indicates success.
 * @return           1 if found, 0 otherwise.
 */
static int ts_find_last_valid(rdb_tsdb_t* db, uint8_t s,
    uint32_t time_base, uint32_t end_off,
    ts_lt_ctx_t* c) {
    uint32_t base = tsa(db, s);
    uint32_t off = tds(db);

    c->found = 0;

    while (off + TS_REC_SZ <= end_off) {
        rdb_ts_record_hdr_t rh;
        if (trd(db, base + off, &rh, sizeof(rh)) != 0)
            break;

        /* End of record chain */
        if (rh.magic == 0xFFu && rh.state == 0xFFu)
            break;

        /* Corrupt record — skip */
        if (rh.magic != RDB_TS_RECORD_MAGIC) {
            off += ts_corrupt_skip(db);
            continue;
        }

        uint32_t rsz = trs(db, rh.data_len);
        if (off + rsz > end_off)
            break;

        if (rh.state == RDB_STATE_VALID) {
            c->time = (time_base != RDB_TIME_INVALID) ? time_base + rh.time_delta : rh.time_delta;
            c->addr = base + off;
            c->dlen = rh.data_len;
            c->found = 1;
            /* Don't break — keep scanning to find the LAST valid record */
        }
        off += rsz;
    }
    return c->found;
}

rdb_err_t rdb_tsdb_get_latest(rdb_tsdb_t* db, uint32_t* time,
    void* buf, uint16_t buf_len, uint16_t* out_len) {
    if (!db || !db->initialized)
        return RDB_ERR_NOT_INIT;
    tlock(db);

    if (db->total_count == 0) {
        tunlock(db);
        return RDB_ERR_NOT_FOUND;
    }

    ts_lt_ctx_t c = {.found = 0};

    /* Try head sector first (most likely to contain the latest record) */
    if (db->head_count > 0 && db->head_time_base != RDB_TIME_INVALID)
        ts_find_last_valid(db, db->head_sec, db->head_time_base,
            db->head_off, &c);

    /* Walk backwards from head toward tail if not found in head */
    if (!c.found) {
        uint8_t prev = tprev(db, db->head_sec);

        for (uint8_t i = 0; i < db->sector_cnt - 1 && !c.found; i++) {
            rdb_ts_sector_hdr_t h;
            ts_cls_t            cls = ts_classify(db, prev, &h);

            if (cls == TS_SEALED && h.time_base != RDB_TIME_INVALID &&
                h.count > 0) {
                ts_find_last_valid(db, prev, h.time_base, h.end_off, &c);
            } else if (cls == TS_ACTIVE) {
                /* [T-6 fix] Degraded ACTIVE sector */
                uint32_t tb, eo;
                ts_active_info(db, prev, &tb, &eo);
                if (tb != RDB_TIME_INVALID && eo > tds(db))
                    ts_find_last_valid(db, prev, tb, eo, &c);
            }

            if (prev == db->tail_sec)
                break;
            prev = tprev(db, prev);
        }
    }

    if (!c.found) {
        tunlock(db);
        return RDB_ERR_NOT_FOUND;
    }

    if (time)
        *time = c.time;
    if (out_len)
        *out_len = c.dlen;

    /* Read and verify data payload */
    if (buf && buf_len > 0 && c.dlen > 0) {
        uint32_t da = c.addr + TS_REC_SZ;

        /* Read record header for CRC */
        rdb_ts_record_hdr_t rh;
        if (trd(db, c.addr, &rh, sizeof(rh)) != 0) {
            tunlock(db);
            return RDB_ERR_FLASH;
        }

        /* Verify data CRC */
        uint16_t calc;
        if (ts_data_crc(db, da, c.dlen, &calc) != 0) {
            tunlock(db);
            return RDB_ERR_FLASH;
        }
        if (calc != rh.data_crc) {
            db->stats.crc_errors++;
            tunlock(db);
            return RDB_ERR_CRC;
        }

        /* Copy data to caller buffer */
        uint16_t rd = (uint16_t)RDB_MIN(buf_len, c.dlen);
        if (trd(db, da, buf, rd) != 0) {
            tunlock(db);
            return RDB_ERR_FLASH;
        }
    }

    tunlock(db);
    return RDB_OK;
}

/* ═══════════════════════════════════════════════════════════════════════════
 *  [T-6 fix] rdb_tsdb_get_oldest — Retrieve the oldest record
 *
 *  Searches from the tail sector forward through the ring to find
 *  the first VALID record.  Verifies data CRC before returning.
 *
 *  T-6 fix: degraded ACTIVE sectors in the ring body are handled
 *  via ts_active_info() to recover time_base and write frontier,
 *  rather than being silently skipped.
 *
 *  @param db        Database handle.
 *  @param[out] time Receives the record's timestamp (may be NULL).
 *  @param buf       Buffer to receive the data payload (may be NULL).
 *  @param buf_len   Size of buf in bytes.
 *  @param[out] out_len  Receives the actual data length (may be NULL).
 *  @return          RDB_OK, RDB_ERR_NOT_FOUND, RDB_ERR_CRC, etc.
 * ═══════════════════════════════════════════════════════════════════════════ */

/**
 * @brief Scan callback for get_oldest — stops at the first VALID record.
 */
static int ts_old_cb(rdb_tsdb_t* db, const ts_rec_t* r, void* arg) {
    (void)db;
    ts_lt_ctx_t* c = (ts_lt_ctx_t*)arg;
    c->time = r->time;
    c->addr = r->addr;
    c->dlen = r->data_len;
    c->found = 1;
    return RDB_ITER_STOP; /* First VALID record is the oldest */
}

rdb_err_t rdb_tsdb_get_oldest(rdb_tsdb_t* db, uint32_t* time,
    void* buf, uint16_t buf_len, uint16_t* out_len) {
    if (!db || !db->initialized)
        return RDB_ERR_NOT_INIT;
    tlock(db);

    if (db->total_count == 0) {
        tunlock(db);
        return RDB_ERR_NOT_FOUND;
    }

    ts_lt_ctx_t c = {.found = 0};

    /* Walk from tail forward through the ring */
    uint8_t s = db->tail_sec;
    for (uint8_t i = 0; i < db->sector_cnt && !c.found; i++) {
        rdb_ts_sector_hdr_t h;
        ts_cls_t            cls = ts_classify(db, s, &h);

        if (cls == TS_SEALED && h.time_base != RDB_TIME_INVALID &&
            h.count > 0) {
            /* Sealed sector with records — scan for first VALID */
            ts_scan(db, s, h.time_base, h.end_off, ts_old_cb, &c, RDB_FALSE);

        } else if (cls == TS_ACTIVE && s == db->head_sec &&
                   db->head_count > 0 &&
                   db->head_time_base != RDB_TIME_INVALID) {
            /* Active head sector — scan up to head_off */
            ts_scan(db, s, db->head_time_base, db->head_off,
                ts_old_cb, &c, RDB_FALSE);

        } else if (cls == TS_ACTIVE && s != db->head_sec) {
            /* [T-6 fix] Degraded ACTIVE sector — recover and scan */
            uint32_t tb, eo;
            ts_active_info(db, s, &tb, &eo);
            if (tb != RDB_TIME_INVALID && eo > tds(db))
                ts_scan(db, s, tb, eo, ts_old_cb, &c, RDB_FALSE);
        }

        if (s == db->head_sec)
            break;
        s = tnext(db, s);
    }

    if (!c.found) {
        tunlock(db);
        return RDB_ERR_NOT_FOUND;
    }

    if (time)
        *time = c.time;
    if (out_len)
        *out_len = c.dlen;

    /* Read and verify data payload */
    if (buf && buf_len > 0 && c.dlen > 0) {
        uint32_t da = c.addr + TS_REC_SZ;

        /* Read record header for CRC */
        rdb_ts_record_hdr_t rh;
        if (trd(db, c.addr, &rh, sizeof(rh)) != 0) {
            tunlock(db);
            return RDB_ERR_FLASH;
        }

        /* Verify data CRC */
        uint16_t calc;
        if (ts_data_crc(db, da, c.dlen, &calc) != 0) {
            tunlock(db);
            return RDB_ERR_FLASH;
        }
        if (calc != rh.data_crc) {
            db->stats.crc_errors++;
            tunlock(db);
            return RDB_ERR_CRC;
        }

        /* Copy data to caller buffer */
        uint16_t rd = (uint16_t)RDB_MIN(buf_len, c.dlen);
        if (trd(db, da, buf, rd) != 0) {
            tunlock(db);
            return RDB_ERR_FLASH;
        }
    }

    tunlock(db);
    return RDB_OK;
}

/* ═══════════════════════════════════════════════════════════════════════════
 *  rdb_tsdb_count — Return total number of VALID records in the database
 *
 *  @param db  Database handle.
 *  @return    Total record count, or 0 if not initialised.
 * ═══════════════════════════════════════════════════════════════════════════ */

uint32_t rdb_tsdb_count(rdb_tsdb_t* db) {
    if (!db || !db->initialized)
        return 0;
    tlock(db);
    uint32_t c = db->total_count;
    tunlock(db);
    return c;
}

/* ═══════════════════════════════════════════════════════════════════════════
 *  [T-6 fix] rdb_tsdb_time_range — Report oldest and newest timestamps
 *
 *  For the oldest timestamp, walks from tail forward.  Degraded ACTIVE
 *  sectors use ts_active_info() (T-6 fix) to recover time_base.
 *
 *  For the newest timestamp, returns db->last_time which is maintained
 *  incrementally by append().
 *
 *  @param db          Database handle.
 *  @param[out] oldest Receives the oldest timestamp (may be NULL).
 *  @param[out] newest Receives the newest timestamp (may be NULL).
 * ═══════════════════════════════════════════════════════════════════════════ */

void rdb_tsdb_time_range(rdb_tsdb_t* db, uint32_t* oldest, uint32_t* newest) {
    if (!db || !db->initialized) {
        if (oldest)
            *oldest = RDB_TIME_INVALID;
        if (newest)
            *newest = RDB_TIME_INVALID;
        return;
    }

    tlock(db);

    if (db->total_count == 0) {
        if (oldest)
            *oldest = RDB_TIME_INVALID;
        if (newest)
            *newest = RDB_TIME_INVALID;
        tunlock(db);
        return;
    }

    if (newest)
        *newest = db->last_time;

    if (oldest) {
        *oldest = RDB_TIME_INVALID;
        uint8_t s = db->tail_sec;

        for (uint8_t i = 0; i < db->sector_cnt; i++) {
            rdb_ts_sector_hdr_t h;
            ts_cls_t            cls = ts_classify(db, s, &h);

            if (cls == TS_SEALED && h.time_base != RDB_TIME_INVALID) {
                /* Scan for the first VALID record to get its actual
                   timestamp (time_base + delta).  Using time_base alone
                   is inaccurate when the first record is DEAD. */
                *oldest = h.time_base; /* fallback */
                uint32_t base = tsa(db, s);
                uint32_t off  = tds(db);
                while (off + TS_REC_SZ <= h.end_off) {
                    rdb_ts_record_hdr_t rh;
                    if (trd(db, base + off, &rh, sizeof(rh)) != 0)
                        break;
                    if (rh.magic == 0xFFu && rh.state == 0xFFu)
                        break;
                    if (rh.magic != RDB_TS_RECORD_MAGIC) {
                        off += ts_corrupt_skip(db);
                        continue;
                    }
                    uint32_t rsz = trs(db, rh.data_len);
                    if (off + rsz > h.end_off)
                        break;
                    if (rh.state == RDB_STATE_VALID) {
                        *oldest = h.time_base + rh.time_delta;
                        break;
                    }
                    off += rsz;
                }
                break;
            }
            if (cls == TS_ACTIVE && s == db->head_sec &&
                db->head_time_base != RDB_TIME_INVALID) {
                *oldest = db->head_time_base;
                break;
            }
            if (cls == TS_ACTIVE && s != db->head_sec) {
                /* [T-6 fix] Degraded ACTIVE sector */
                uint32_t tb, eo;
                ts_active_info(db, s, &tb, &eo);
                if (tb != RDB_TIME_INVALID) {
                    *oldest = tb;
                    break;
                }
            }

            if (s == db->head_sec)
                break;
            s = tnext(db, s);
        }
    }

    tunlock(db);
}

/* ═══════════════════════════════════════════════════════════════════════════
 *  rdb_tsdb_wear_info — Query flash wear statistics
 *
 *  Reports the minimum, maximum, and average erase counts across
 *  all TSDB sectors.  If no erase count buffer was provided at init,
 *  all values report as 0.
 *
 *  @param db          Database handle.
 *  @param[out] min_ec Minimum erase count across all sectors (may be NULL).
 *  @param[out] max_ec Maximum erase count across all sectors (may be NULL).
 *  @param[out] avg_ec Average erase count across all sectors (may be NULL).
 * ═══════════════════════════════════════════════════════════════════════════ */

void rdb_tsdb_wear_info(rdb_tsdb_t* db,
    uint32_t* min_ec, uint32_t* max_ec, uint32_t* avg_ec) {
    if (!db || !db->initialized)
        return;
    tlock(db);

    uint32_t mn = 0xFFFFFFFFu, mx = 0;
    uint64_t sum = 0;
    for (uint8_t s = 0; s < db->sector_cnt; s++) {
        uint32_t ec = db->erase_cnts ? db->erase_cnts[s] : 0;
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

    tunlock(db);
}

/* ═══════════════════════════════════════════════════════════════════════════
 *  rdb_tsdb_get_stats / rdb_tsdb_reset_stats — Runtime statistics
 *
 *  get_stats copies the current statistics snapshot to a caller buffer.
 *  reset_stats zeroes all counters.
 * ═══════════════════════════════════════════════════════════════════════════ */

void rdb_tsdb_get_stats(rdb_tsdb_t* db, rdb_ts_stats_t* out) {
    if (!db || !out)
        return;
    tlock(db);
    *out = db->stats;
    tunlock(db);
}

void rdb_tsdb_reset_stats(rdb_tsdb_t* db) {
    if (!db)
        return;
    tlock(db);
    memset(&db->stats, 0, sizeof(db->stats));
    tunlock(db);
}