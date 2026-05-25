/**
 * sim_trace.h — Flash data change tracing and parameter snapshot API
 *
 * Integrates with sim_flash and test_framework to record:
 *  - Flash read/write/erase operations with hex dumps
 *  - KVDB/TSDB parameter snapshots (sector metadata, DB state, stats)
 *  - High-level events (format, GC, record operations)
 *
 * Control via test_config_t.verbose:
 *  level 0: minimal (test names only)
 *  level 1: normal (test results + key events)
 *  level 2: detailed (+ flash ops, sector snapshots)
 *  level 3: trace (+ full hex dumps of flash data)
 *
 * Copyright (c) 2026 RocketDB Contributors
 * SPDX-License-Identifier: MIT
 */

#ifndef SIM_TRACE_H
#define SIM_TRACE_H

#include <stdint.h>
#include <stddef.h>
#include <stdio.h>

/* Need KVDB/TSDB struct definitions for snapshot function signatures */
#include "../../src/rocketdb.h"

#ifdef __cplusplus
extern "C" {
#endif

/* fwd — defined in rocketdb.h included above */

typedef struct {
    FILE    *fp;           /* output file (NULL = disabled)      */
    int      level;        /* 0=off, 1=events, 2=ops, 3=hex     */
    uint32_t op_seq;       /* monotonic operation counter        */
} trace_ctx_t;

/* ── Lifecycle ──────────────────────────────────────────────────────────── */

void trace_init(trace_ctx_t *t, FILE *fp, int level);
void trace_set_level(trace_ctx_t *t, int level);

/* ── Flash operation tracing (called from sim_flash) ────────────────────── */

void trace_flash_read(trace_ctx_t *t, uint32_t addr,
                      const uint8_t *data, size_t len);
void trace_flash_write(trace_ctx_t *t, uint32_t addr,
                       const uint8_t *before, const uint8_t *after, size_t len);
void trace_flash_erase(trace_ctx_t *t, uint32_t addr, uint32_t sector_size);

/* ── High-level event tracing ───────────────────────────────────────────── */

void trace_event(trace_ctx_t *t, const char *fmt, ...);
void trace_kv_op(trace_ctx_t *t, const char *op, const char *key,
                 uint16_t key_len, uint16_t val_len, uint32_t seq, int result);

/* ── KVDB parameter snapshots ───────────────────────────────────────────── */

void trace_kvdb_snapshot(trace_ctx_t *t, const rdb_kvdb_t *db);
void trace_kvdb_sectors(trace_ctx_t *t,
                        const rdb_kv_sector_meta_t *sectors,
                        uint8_t cnt);
void trace_kvdb_stats(trace_ctx_t *t, const rdb_kvdb_t *db);
void trace_kvdb_sector_summary(trace_ctx_t *t, const rdb_kvdb_t *db);

/* ── KVDB GC event (compact per-GC sector view) ─────────────────────────── */

void trace_kvdb_gc_event(trace_ctx_t *t, const rdb_kvdb_t *db,
                          uint32_t gc_run, uint32_t loop);

/* ── TSDB parameter snapshots ───────────────────────────────────────────── */

void trace_tsdb_snapshot(trace_ctx_t *t, const rdb_tsdb_t *db);
void trace_tsdb_erase_counts(trace_ctx_t *t,
                             const uint32_t *ec, const uint8_t *status,
                             uint8_t cnt);
void trace_tsdb_stats(trace_ctx_t *t, const rdb_tsdb_t *db);
void trace_tsdb_sector_summary(trace_ctx_t *t, const rdb_tsdb_t *db);

/* ── TSDB rotation event (compact per-rotation sector view) ──────────────── */

void trace_tsdb_rot_event(trace_ctx_t *t, const rdb_tsdb_t *db,
                           uint32_t rotation, uint32_t loop);

/* ── Sector geometry (max data length per configuration) ─────────────────── */

void trace_kvdb_geometry(trace_ctx_t *t, const rdb_kvdb_t *db);
void trace_tsdb_geometry(trace_ctx_t *t, const rdb_tsdb_t *db);

/* ── Hex dump utility ───────────────────────────────────────────────────── */

void trace_hex_dump(FILE *fp, const uint8_t *data, size_t len,
                    uint32_t base_addr);
void trace_hex_diff(FILE *fp, const uint8_t *before, const uint8_t *after,
                    size_t len, uint32_t base_addr);

/* ── Partition info ─────────────────────────────────────────────────────── */

void trace_partition_info(trace_ctx_t *t,
                          const char *name, uint32_t base_addr,
                          uint32_t total_size, uint32_t sector_size,
                          uint8_t write_gran, uint8_t sector_cnt);

#ifdef __cplusplus
}
#endif

#endif /* SIM_TRACE_H */
