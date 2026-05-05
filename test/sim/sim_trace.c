/**
 * sim_trace.c — Flash data change tracing and parameter snapshot implementation
 *
 * Copyright (c) 2026 RocketDB Contributors
 * SPDX-License-Identifier: MIT
 */

#include "sim_trace.h"
#include <string.h>
#include <stdarg.h>
#include <inttypes.h>

/* ═══════════════════════════════════════════════════════════════════════════
 *  §1  Lifecycle
 * ═══════════════════════════════════════════════════════════════════════════ */

void trace_init(trace_ctx_t *t, FILE *fp, int level)
{
    if (!t) return;
    t->fp     = fp;
    t->level  = level;
    t->op_seq = 0;
}

void trace_set_level(trace_ctx_t *t, int level)
{
    if (!t) return;
    t->level = level;
}

/* ═══════════════════════════════════════════════════════════════════════════
 *  §2  Hex dump utilities
 * ═══════════════════════════════════════════════════════════════════════════ */

void trace_hex_dump(FILE *fp, const uint8_t *data, size_t len,
                    uint32_t base_addr)
{
    if (!fp || !data || !len) return;

    for (size_t offset = 0; offset < len; offset += 16) {
        fprintf(fp, "  %08" PRIx32 ": ", (uint32_t)(base_addr + offset));

        /* hex */
        for (size_t j = 0; j < 16; j++) {
            if (offset + j < len)
                fprintf(fp, "%02X ", data[offset + j]);
            else
                fprintf(fp, "   ");
        }

        /* ascii */
        fprintf(fp, " ");
        for (size_t j = 0; j < 16 && offset + j < len; j++) {
            uint8_t c = data[offset + j];
            fprintf(fp, "%c", (c >= 32 && c < 127) ? (char)c : '.');
        }
        fprintf(fp, "\n");
    }
}

void trace_hex_diff(FILE *fp, const uint8_t *before, const uint8_t *after,
                    size_t len, uint32_t base_addr)
{
    if (!fp || !before || !after || !len) return;

    size_t changed = 0;
    for (size_t i = 0; i < len; i++) {
        if (before[i] != after[i]) changed++;
    }

    if (changed == 0) {
        fprintf(fp, "  (no bytes changed)\n");
        return;
    }

    fprintf(fp, "  %zu bytes changed:\n", changed);

    for (size_t offset = 0; offset < len; offset += 16) {
        int has_change = 0;
        for (size_t j = 0; j < 16 && offset + j < len; j++) {
            if (before[offset + j] != after[offset + j]) {
                has_change = 1;
                break;
            }
        }
        if (!has_change) continue;

        /* before row */
        fprintf(fp, "  %08" PRIx32 "-: ", (uint32_t)(base_addr + offset));
        for (size_t j = 0; j < 16; j++) {
            if (offset + j < len)
                fprintf(fp, "%02X ", before[offset + j]);
            else
                fprintf(fp, "   ");
        }
        fprintf(fp, "\n");

        /* after row */
        fprintf(fp, "  %08" PRIx32 "+: ", (uint32_t)(base_addr + offset));
        for (size_t j = 0; j < 16; j++) {
            if (offset + j < len) {
                uint8_t b = before[offset + j];
                uint8_t a = after[offset + j];
                if (b != a)
                    fprintf(fp, "\033[33m%02X\033[0m ", a);
                else
                    fprintf(fp, "%02X ", a);
            } else
                fprintf(fp, "   ");
        }
        fprintf(fp, "\n");
    }
}

/* ═══════════════════════════════════════════════════════════════════════════
 *  §3  Flash operation tracing
 * ═══════════════════════════════════════════════════════════════════════════ */

static const char *k_op_labels[] = { "READ", "WRITE", "ERASE" };

static void trace_flash_op_impl(trace_ctx_t *t, int op, uint32_t addr,
                                 const uint8_t *before,
                                 const uint8_t *after,
                                 size_t len, uint32_t sector_size)
{
    if (!t || !t->fp) return;

    /* Writes at level 1 (key data changes), reads/erases at level 2 */
    int min_level = (op == 1) ? 1 : 2;
    if (t->level < min_level) return;

    t->op_seq++;

    if (op == 2) { /* ERASE */
        fprintf(t->fp, "[FLASH #%" PRIu32 "] ERASE addr=0x%08" PRIx32
                " sector=%" PRIu32 " size=%" PRIu32 "\n",
                t->op_seq, addr, addr / sector_size, sector_size);
        /* Pre-erase hex sample is dumped by sim_flash_erase at level >= 3 */
        return;
    }

    fprintf(t->fp, "[FLASH #%" PRIu32 "] %s addr=0x%08" PRIx32 " len=%zu\n",
            t->op_seq, k_op_labels[op], addr, len);

    if (t->level >= 3) {
        if (op == 0) { /* READ */
            trace_hex_dump(t->fp, after, len, addr);
        } else { /* WRITE */
            trace_hex_diff(t->fp, before, after, len, addr);
        }
    }
}

void trace_flash_read(trace_ctx_t *t, uint32_t addr,
                      const uint8_t *data, size_t len)
{
    trace_flash_op_impl(t, 0, addr, NULL, data, len, 0);
}

void trace_flash_write(trace_ctx_t *t, uint32_t addr,
                       const uint8_t *before, const uint8_t *after, size_t len)
{
    trace_flash_op_impl(t, 1, addr, before, after, len, 0);
}

void trace_flash_erase(trace_ctx_t *t, uint32_t addr, uint32_t sector_size)
{
    trace_flash_op_impl(t, 2, addr, NULL, NULL, 0, sector_size);
}

/* ═══════════════════════════════════════════════════════════════════════════
 *  §4  High-level events
 * ═══════════════════════════════════════════════════════════════════════════ */

void trace_event(trace_ctx_t *t, const char *fmt, ...)
{
    if (!t || !t->fp || t->level < 1) return;

    t->op_seq++;
    fprintf(t->fp, "[EVENT #%" PRIu32 "] ", t->op_seq);

    va_list ap;
    va_start(ap, fmt);
    vfprintf(t->fp, fmt, ap);
    va_end(ap);

    fprintf(t->fp, "\n");
}

void trace_kv_op(trace_ctx_t *t, const char *op, const char *key,
                 uint16_t key_len, uint16_t val_len, uint32_t seq, int result)
{
    if (!t || !t->fp || t->level < 1) return;

    t->op_seq++;
    fprintf(t->fp, "[OP #%" PRIu32 "] %s key=\"", t->op_seq, op);
    for (uint16_t i = 0; i < key_len && i < 64; i++) {
        char c = key[i];
        fprintf(t->fp, "%c", (c >= 32 && c < 127) ? c : '.');
    }
    fprintf(t->fp, "\" key_len=%u val_len=%u seq=%" PRIu32
            " → %s\n", key_len, val_len, seq,
            (result == RDB_OK) ? "OK" :
            (result == RDB_ERR_NOT_FOUND) ? "NOT_FOUND" :
            (result == RDB_ERR_TOO_LARGE) ? "TOO_LARGE" :
            (result == RDB_ERR_FLASH) ? "FLASH_ERR" :
            (result == RDB_ERR_BUSY) ? "BUSY" : "ERR");
}

/* ═══════════════════════════════════════════════════════════════════════════
 *  §5  Partition info
 * ═══════════════════════════════════════════════════════════════════════════ */

void trace_partition_info(trace_ctx_t *t,
                          const char *name, uint32_t base_addr,
                          uint32_t total_size, uint32_t sector_size,
                          uint8_t write_gran, uint8_t sector_cnt)
{
    if (!t || !t->fp || t->level < 1) return;

    fprintf(t->fp, "\n=== Partition: %s ===\n", name);
    fprintf(t->fp, "  base_addr:   0x%08" PRIx32 "\n", base_addr);
    fprintf(t->fp, "  total_size:  %" PRIu32 " (%" PRIu32 " KB)\n",
            total_size, total_size / 1024);
    fprintf(t->fp, "  sector_size: %" PRIu32 "\n", sector_size);
    fprintf(t->fp, "  write_gran:  %u (%u bytes)\n",
            write_gran, 1u << write_gran);
    fprintf(t->fp, "  sector_cnt:  %u\n", sector_cnt);
    fprintf(t->fp, "  gc_reserve:  %u\n",
            (uint8_t)(sector_cnt / RDB_KV_MIN_SECTORS));
    fprintf(t->fp, "\n");
}

/* ═══════════════════════════════════════════════════════════════════════════
 *  §6  KVDB parameter snapshots
 * ═══════════════════════════════════════════════════════════════════════════ */

static const char *k_status_names[] = {
    "ERASED", "ACTIVE", "SEALED", "CORRUPT"
};

void trace_kvdb_sectors(trace_ctx_t *t,
                        const rdb_kv_sector_meta_t *sectors,
                        uint8_t cnt)
{
    if (!t || !t->fp || t->level < 2) return;

    fprintf(t->fp, "\n--- KVDB Sector Metadata (%u sectors) ---\n", cnt);
    fprintf(t->fp, "%-6s %-8s %-12s %-10s %-10s %-10s\n",
            "Idx", "Status", "create_seq", "erase_cnt", "write_off", "garbage");
    fprintf(t->fp, "------ ------ ------------ ---------- ---------- ----------\n");

    for (uint8_t i = 0; i < cnt; i++) {
        const char *sts = (sectors[i].status <= 3)
                          ? k_status_names[sectors[i].status] : "???";
        fprintf(t->fp, "[%3u] %-8s %12" PRIu32 " %10" PRIu32
                " %10" PRIu32 " %10" PRIu32 "\n",
                i, sts,
                sectors[i].create_seq,
                sectors[i].erase_cnt,
                sectors[i].write_off,
                sectors[i].garbage_bytes);
    }
    fprintf(t->fp, "\n");
}

void trace_kvdb_stats(trace_ctx_t *t, const rdb_kvdb_t *db)
{
    if (!t || !t->fp || t->level < 1 || !db) return;

    fprintf(t->fp, "--- KVDB Runtime Stats ---\n");
    fprintf(t->fp, "  read_ops:        %" PRIu32 "\n", db->stats.read_ops);
    fprintf(t->fp, "  write_ops:       %" PRIu32 "\n", db->stats.write_ops);
    fprintf(t->fp, "  delete_ops:      %" PRIu32 "\n", db->stats.delete_ops);
    fprintf(t->fp, "  gc_runs:         %" PRIu32 "\n", db->stats.gc_runs);
    fprintf(t->fp, "  gc_reclaimed:    %" PRIu32 " bytes\n", db->stats.gc_reclaimed_bytes);
    fprintf(t->fp, "  gc_migrated:     %" PRIu32 " recs\n", db->stats.gc_migrated_recs);
    fprintf(t->fp, "  flash_errors:    %" PRIu32 "\n", db->stats.flash_errors);
    fprintf(t->fp, "  crc_errors:      %" PRIu32 "\n", db->stats.crc_errors);
    fprintf(t->fp, "  corrupt_sectors: %" PRIu32 "\n", db->stats.corrupt_sectors);
    fprintf(t->fp, "\n");
}

void trace_kvdb_snapshot(trace_ctx_t *t, const rdb_kvdb_t *db)
{
    if (!t || !t->fp || t->level < 1 || !db) return;

    fprintf(t->fp, "\n========== KVDB Snapshot ==========\n");

    fprintf(t->fp, "--- KVDB Runtime State ---\n");
    fprintf(t->fp, "  active_sec:   %u\n", db->active_sec);
    fprintf(t->fp, "  write_seq:    %" PRIu32 "\n", db->write_seq);
    fprintf(t->fp, "  live_bytes:   %" PRIu32 "\n", db->live_bytes);
    fprintf(t->fp, "  write_off:    %" PRIu32 "\n", db->write_off);
    fprintf(t->fp, "  iter_gen:     %" PRIu32 "\n", db->iter_gen);
    fprintf(t->fp, "  gc_reserve:   %u\n", db->gc_reserve);
    fprintf(t->fp, "  initialized:  %u\n", db->initialized);

    /* Detailed per-sector view at level >= 2 */
    if (t->level >= 2) {
        trace_kvdb_sectors(t, db->sectors, db->sector_cnt);
    }
    trace_kvdb_stats(t, db);
    fprintf(t->fp, "====================================\n\n");
}

void trace_kvdb_sector_summary(trace_ctx_t *t, const rdb_kvdb_t *db)
{
    if (!t || !t->fp || t->level < 1 || !db || !db->sectors) return;

    uint32_t ss = db->part ? db->part->sector_size : 4096;

    fprintf(t->fp, "--- KVDB Sector Summary (%u sectors, active=%u) ---\n",
            db->sector_cnt, db->active_sec);
    fprintf(t->fp, "%-5s %-8s %7s %7s %7s %7s\n",
            "Idx", "Status", "used%", "garb%", "erases", "seq");
    for (uint8_t i = 0; i < db->sector_cnt; i++) {
        const rdb_kv_sector_meta_t *s = &db->sectors[i];
        const char *sts = (s->status <= 3) ? k_status_names[s->status] : "???";
        uint32_t used_pct  = ss ? (s->write_off * 100u / ss) : 0;
        uint32_t garb_pct  = ss ? (s->garbage_bytes * 100u / ss) : 0;
        fprintf(t->fp, "[%2u]  %-8s %3u%%   %3u%%   %5u %5u\n",
                i, sts, used_pct, garb_pct, s->erase_cnt, s->create_seq);
    }
    fprintf(t->fp, "\n");
}

/* ═══════════════════════════════════════════════════════════════════════════
 *  §6.1  KVDB GC event — compact per-sector snapshot during GC
 * ═══════════════════════════════════════════════════════════════════════════ */

void trace_kvdb_gc_event(trace_ctx_t *t, const rdb_kvdb_t *db,
                          uint32_t gc_run, uint32_t loop)
{
    if (!t || !t->fp || t->level < 1 || !db || !db->sectors || !db->part) return;

    uint32_t ss = db->part->sector_size;
    uint32_t gran = 1u << db->part->write_gran;
    uint32_t ds = RDB_ALIGN_UP((uint32_t)sizeof(rdb_kv_sector_hdr_t), gran);
    uint32_t total_garbage = 0, total_free = 0;
    uint8_t  active = db->active_sec;

    /* Count totals first — ERASED sectors reserve ds bytes for the sector header */
    for (uint8_t i = 0; i < db->sector_cnt; i++) {
        total_garbage += db->sectors[i].garbage_bytes;
        if (db->sectors[i].status == RDB_SEC_ERASED)
            total_free += ss - ds;
        else if (db->sectors[i].status == RDB_SEC_ACTIVE)
            total_free += ss - db->sectors[i].write_off;
    }

    fprintf(t->fp, "\n┌─ GC #%" PRIu32 " (loop=%" PRIu32 ") ──────────────────────────────────────────────┐\n",
            gc_run, loop);
    fprintf(t->fp, "│ Partition: %u sectors × %" PRIu32 "B = %" PRIu32 " KB | live=%" PRIu32 "B garb=%" PRIu32 "B free=%" PRIu32 "B │\n",
            db->sector_cnt, ss, (db->sector_cnt * ss) / 1024,
            db->live_bytes, total_garbage, total_free);
    fprintf(t->fp, "│%5s %-8s %6s %6s %7s %8s %6s│\n",
            "Idx", "Status", "used%", "garb%", "freeB", "erases", "seq");
    fprintf(t->fp, "│%5s %-8s %6s %6s %7s %8s %6s│\n",
            "─────", "──────", "─────", "─────", "──────", "──────", "─────");

    for (uint8_t i = 0; i < db->sector_cnt; i++) {
        const rdb_kv_sector_meta_t *s = &db->sectors[i];
        const char *sts = (s->status <= 3) ? k_status_names[s->status] : "???";
        uint32_t used_pct = ss ? (s->write_off * 100u / ss) : 0;
        uint32_t garb_pct = ss ? (s->garbage_bytes * 100u / ss) : 0;
        uint32_t free_b = ss - s->write_off;
        if (s->status == RDB_SEC_ERASED)
            free_b = ss - ds;  /* sector header overhead deducted */

        const char *marker = (i == active) ? " ★" : "  ";
        fprintf(t->fp, "│[%2u] %-8s %3u%%  %3u%%  %6u %8u %6u%s│\n",
                i, sts, used_pct, garb_pct, free_b, s->erase_cnt, s->create_seq, marker);
    }
    fprintf(t->fp, "└──────────────────────────────────────────────────────────────────┘\n\n");
}

/* ═══════════════════════════════════════════════════════════════════════════
 *  §8  Sector geometry (max data length per configuration)
 * ═══════════════════════════════════════════════════════════════════════════ */

void trace_kvdb_geometry(trace_ctx_t *t, const rdb_kvdb_t *db)
{
    if (!t || !t->fp || t->level < 1 || !db || !db->part) return;

    uint32_t ss   = db->part->sector_size;
    uint8_t  wg   = db->part->write_gran;
    uint32_t gran = 1u << wg;
    uint32_t ds   = RDB_ALIGN_UP((uint32_t)sizeof(rdb_kv_sector_hdr_t), gran);
    uint32_t cap  = ss - ds;

    fprintf(t->fp, "\n┌─ KVDB Sector Geometry ──────────────────────────────────────────────┐\n");
    fprintf(t->fp, "│ sector_size=%-5" PRIu32 "B  write_gran=%u (%-2uB align)                         │\n",
            ss, wg, gran);
    fprintf(t->fp, "│ sector_hdr=%-2uB  data_start=%-5" PRIu32 "B  data_cap=%-5" PRIu32 "B                 │\n",
            (unsigned)sizeof(rdb_kv_sector_hdr_t), ds, cap);
    fprintf(t->fp, "│ record_hdr=%-2uB                                                       │\n",
            (unsigned)sizeof(rdb_kv_record_hdr_t));
    fprintf(t->fp, "│ %5s %8s %10s %13s │\n",
            "keyB", "key_align", "max_val", "rec_sz");
    fprintf(t->fp, "│ %5s %8s %10s %13s │\n",
            "─────", "────────", "───────", "──────");

    uint8_t kl_samples[] = { 1, 2, 63 };
    for (int i = 0; i < 3; i++) {
        uint16_t kl = kl_samples[i];
        uint32_t ka = RDB_ALIGN_UP(kl, gran);
        /* va_max = cap - 16 - ka; max_val = largest v where ALIGN_UP(v,gran) <= va_max */
        uint32_t va_max = cap - (uint32_t)sizeof(rdb_kv_record_hdr_t) - ka;
        uint32_t max_v  = va_max - (va_max % gran);
        uint32_t rec_sz = (uint32_t)sizeof(rdb_kv_record_hdr_t) + ka + RDB_ALIGN_UP(max_v, gran);

        fprintf(t->fp, "│ %5uB %8uB %10uB %13uB │\n",
                (unsigned)kl, ka, max_v, rec_sz);
    }
    fprintf(t->fp, "└─────────────────────────────────────────────────────────────────────────┘\n\n");
}

void trace_tsdb_geometry(trace_ctx_t *t, const rdb_tsdb_t *db)
{
    if (!t || !t->fp || t->level < 1 || !db || !db->part) return;

    uint32_t ss   = db->sector_size;
    uint8_t  wg   = db->part->write_gran;
    uint32_t gran = 1u << wg;
    uint32_t ds   = RDB_ALIGN_UP((uint32_t)sizeof(rdb_ts_sector_hdr_t), gran);
    uint32_t cap  = ss - ds;
    uint32_t max_dl = db->max_data_len;
    uint32_t rec_sz  = (uint32_t)sizeof(rdb_ts_record_hdr_t) + RDB_ALIGN_UP(max_dl, gran);

    fprintf(t->fp, "\n┌─ TSDB Sector Geometry ──────────────────────────────────────────────┐\n");
    fprintf(t->fp, "│ sector_size=%-5" PRIu32 "B  write_gran=%u (%-2uB align)                         │\n",
            ss, wg, gran);
    fprintf(t->fp, "│ sector_hdr=%-2uB  data_start=%-5" PRIu32 "B  data_cap=%-5" PRIu32 "B                 │\n",
            (unsigned)sizeof(rdb_ts_sector_hdr_t), ds, cap);
    fprintf(t->fp, "│ record_hdr=%-2uB                                                       │\n",
            (unsigned)sizeof(rdb_ts_record_hdr_t));
    fprintf(t->fp, "│ max_data_len=%-5" PRIu32 "B  (record_sz=%" PRIu32 "B, align=%uB)                 │\n",
            max_dl, rec_sz, gran);
    fprintf(t->fp, "└─────────────────────────────────────────────────────────────────────────┘\n\n");
}

/* ═══════════════════════════════════════════════════════════════════════════
 *  §7  TSDB parameter snapshots
 * ═══════════════════════════════════════════════════════════════════════════ */

void trace_tsdb_erase_counts(trace_ctx_t *t,
                             const uint32_t *ec, const uint8_t *status,
                             uint8_t cnt)
{
    if (!t || !t->fp || t->level < 2) return;

    fprintf(t->fp, "\n--- TSDB Sector Erase Counts (%u sectors) ---\n", cnt);
    fprintf(t->fp, "%-6s %-10s %s\n", "Idx", "erase_cnt", "status");
    fprintf(t->fp, "------ ---------- --------\n");

    for (uint8_t i = 0; i < cnt; i++) {
        const char *sts = "?";
        if (status) {
            sts = (status[i] <= 3) ? k_status_names[status[i]] : "?";
        }
        fprintf(t->fp, "[%3u] %10" PRIu32 " %s\n", i, ec ? ec[i] : 0, sts);
    }
    fprintf(t->fp, "\n");
}

void trace_tsdb_stats(trace_ctx_t *t, const rdb_tsdb_t *db)
{
    if (!t || !t->fp || t->level < 1 || !db) return;

    fprintf(t->fp, "--- TSDB Runtime Stats ---\n");
    fprintf(t->fp, "  write_ops:        %" PRIu32 "\n", db->stats.write_ops);
    fprintf(t->fp, "  read_ops:         %" PRIu32 "\n", db->stats.read_ops);
    fprintf(t->fp, "  sector_rotations: %" PRIu32 "\n", db->stats.sector_rotations);
    fprintf(t->fp, "  records_lost:     %" PRIu32 "\n", db->stats.records_lost);
    fprintf(t->fp, "  flash_errors:     %" PRIu32 "\n", db->stats.flash_errors);
    fprintf(t->fp, "  crc_errors:       %" PRIu32 "\n", db->stats.crc_errors);
    fprintf(t->fp, "  data_gaps:        %" PRIu32 "\n", db->stats.data_gaps);
    fprintf(t->fp, "\n");
}

void trace_tsdb_snapshot(trace_ctx_t *t, const rdb_tsdb_t *db)
{
    if (!t || !t->fp || t->level < 1 || !db) return;

    fprintf(t->fp, "\n========== TSDB Snapshot ==========\n");

    fprintf(t->fp, "--- TSDB Runtime State ---\n");
    fprintf(t->fp, "  head_sec:       %u\n", db->head_sec);
    fprintf(t->fp, "  tail_sec:       %u\n", db->tail_sec);
    fprintf(t->fp, "  head_seq:       %" PRIu32 "\n", db->head_seq);
    fprintf(t->fp, "  head_off:       %" PRIu32 "\n", db->head_off);
    fprintf(t->fp, "  head_count:     %u\n", db->head_count);
    fprintf(t->fp, "  head_time_base: %" PRIu32 "\n", db->head_time_base);
    fprintf(t->fp, "  last_time:      %" PRIu32 "\n", db->last_time);
    fprintf(t->fp, "  total_count:    %" PRIu32 "\n", db->total_count);
    fprintf(t->fp, "  max_data_len:   %u\n", db->max_data_len);
    fprintf(t->fp, "  sector_size:    %" PRIu32 "\n", db->sector_size);
    fprintf(t->fp, "  initialized:    %u\n", db->initialized);

    /* Detailed erase count table at level >= 2 */
    if (t->level >= 2) {
        trace_tsdb_erase_counts(t, db->erase_cnts, NULL, db->sector_cnt);
    }
    trace_tsdb_stats(t, db);
    fprintf(t->fp, "====================================\n\n");
}

void trace_tsdb_sector_summary(trace_ctx_t *t, const rdb_tsdb_t *db)
{
    if (!t || !t->fp || t->level < 1 || !db) return;

    fprintf(t->fp, "--- TSDB Sector Summary (%u sectors, head=%u, tail=%u) ---\n",
            db->sector_cnt, db->head_sec, db->tail_sec);
    fprintf(t->fp, "%-5s %-8s %8s %s\n", "Idx", "Role", "erases", "");
    for (uint8_t i = 0; i < db->sector_cnt; i++) {
        const char *role;
        if (i == db->head_sec && i == db->tail_sec)
            role = "HEAD+TL";
        else if (i == db->head_sec)
            role = "HEAD";
        else if (i == db->tail_sec)
            role = "TAIL";
        else {
            /* Between tail and head in ring: has data (SEALED). Outside: ERASED */
            uint8_t has_data = 0;
            if (db->tail_sec <= db->head_sec)
                has_data = (i >= db->tail_sec && i <= db->head_sec);
            else
                has_data = (i >= db->tail_sec || i <= db->head_sec);
            role = has_data ? "DATA" : "FREE";
        }
        uint32_t ec = db->erase_cnts ? db->erase_cnts[i] : 0;
        fprintf(t->fp, "[%2u]  %-8s %8u\n", i, role, ec);
    }
    fprintf(t->fp, "\n");
}

/* ═══════════════════════════════════════════════════════════════════════════
 *  §7.1  TSDB rotation event — compact per-sector snapshot during rotation
 * ═══════════════════════════════════════════════════════════════════════════ */

void trace_tsdb_rot_event(trace_ctx_t *t, const rdb_tsdb_t *db,
                           uint32_t rotation, uint32_t loop)
{
    if (!t || !t->fp || t->level < 1 || !db || !db->part) return;

    uint32_t ss   = db->sector_size;
    uint32_t gran = 1u << db->part->write_gran;
    uint32_t ds   = RDB_ALIGN_UP((uint32_t)sizeof(rdb_ts_sector_hdr_t), gran);
    uint32_t free_est = 0;

    fprintf(t->fp, "\n┌─ ROTATION #%" PRIu32 " (loop=%" PRIu32 ") ───────────────────────────────────────┐\n",
            rotation, loop);
    fprintf(t->fp, "│ Partition: %u sectors × %" PRIu32 "B = %" PRIu32 " KB | count=%" PRIu32 " last_time=%" PRIu32 " │\n",
            db->sector_cnt, ss, (db->sector_cnt * ss) / 1024,
            db->total_count, db->last_time);
    fprintf(t->fp, "│%5s %-8s %8s %8s %s│\n", "Idx", "Role", "erases", "freeB", "");
    fprintf(t->fp, "│%5s %-8s %8s %8s %s│\n", "─────", "──────", "──────", "──────", "");

    for (uint8_t i = 0; i < db->sector_cnt; i++) {
        const char *role;
        if (i == db->head_sec && i == db->tail_sec) role = "HEAD+TL";
        else if (i == db->head_sec)                role = "HEAD";
        else if (i == db->tail_sec)                role = "TAIL";
        else {
            uint8_t has_data;
            if (db->tail_sec <= db->head_sec)
                has_data = (i >= db->tail_sec && i <= db->head_sec);
            else
                has_data = (i >= db->tail_sec || i <= db->head_sec);
            role = has_data ? "DATA" : "FREE";
        }
        uint32_t ec = db->erase_cnts ? db->erase_cnts[i] : 0;
        uint32_t free_b;
        if (role[0] == 'F')           free_b = ss - ds;  /* header overhead deducted */
        else if (role[0] == 'H')      free_b = ss - db->head_off;
        else                          free_b = 0;
        free_est += free_b;
        fprintf(t->fp, "│[%2u] %-8s %8u %8u   │\n", i, role, ec, free_b);
    }
    fprintf(t->fp, "│ free_est=%" PRIu32 "B  head_off=%" PRIu32 "B  time_base=%" PRIu32 " │\n",
            free_est, db->head_off, db->head_time_base);
    fprintf(t->fp, "└──────────────────────────────────────────────────────────────────┘\n\n");
}
