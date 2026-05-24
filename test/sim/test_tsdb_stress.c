/**
 * test_tsdb_stress.c — TSDB stress and fault-recovery tests
 *
 * Covers: rotation stress, append failure handling,
 *         CRC corruption detection, degraded ACTIVE sector recovery
 *
 * References: T-312, T-313, T-315, T-316
 */

#include "sim_flash.h"
#include "sim_fault.h"
#include "test_framework.h"
#include "../../src/rocketdb.h"
#include <stdio.h>
#include <string.h>

#define FLASH_SIZE      (128u * 1024u)
#define SECTOR_SIZE     4096u
#define PAGE_SIZE       256u
#define DEFAULT_WG      0u
#define TSDB_PART_SIZE  (64u * 1024u)
#define TS_SECTOR_CNT   (TSDB_PART_SIZE / SECTOR_SIZE)

/* ── Shared flash environment ──────────────────────────────────────────── */

static uint8_t     g_flash_buf[FLASH_SIZE];
static sim_flash_t g_flash;
static fault_ctx_t g_fault;
static rdb_partition_t g_part;
static rdb_tsdb_t  g_db;
static uint32_t    g_ec[TS_SECTOR_CNT];
static trace_ctx_t g_trace;

static int fl_read(void *ctx, uint32_t addr, uint8_t *buf, size_t len) {
    (void)ctx;
    return sim_flash_read(&g_flash, addr, buf, len);
}
static int fl_write(void *ctx, uint32_t addr, const uint8_t *buf, size_t len) {
    (void)ctx;
    return sim_flash_write(&g_flash, addr, buf, len);
}
static int fl_erase(void *ctx, uint32_t addr) {
    (void)ctx;
    return sim_flash_erase(&g_flash, addr);
}
static void fl_lock(void *ctx) { (void)ctx; }
static void fl_unlock(void *ctx) { (void)ctx; }
static void fl_yield(void *ctx) { (void)ctx; }

static rdb_flash_ops_t g_ops = {
    .read = fl_read, .write = fl_write, .erase = fl_erase,
    .lock = fl_lock, .unlock = fl_unlock, .yield = fl_yield
};

/* ── Trace wrapper ─────────────────────────────────────────────────── */
static rdb_err_t trace_ts_append(rdb_tsdb_t *db, uint32_t ts,
                                  const void *data, uint16_t dlen)
{
    trace_event(&g_trace, "  [TS-APPEND] ts=%lu dlen=%u",
                (unsigned long)ts, (unsigned)dlen);
    return rdb_tsdb_append(db, ts, (const uint8_t *)data, dlen);
}

static rdb_err_t ts_reset(void)
{
    if (sim_flash_init(&g_flash, g_flash_buf, FLASH_SIZE,
                       SECTOR_SIZE, PAGE_SIZE, DEFAULT_WG) != 0)
        return RDB_ERR_FLASH;
    fault_init(&g_fault, 0xA5A5A5A5u);
    sim_flash_set_fault_ctx(&g_flash, &g_fault);
    g_part = (rdb_partition_t) {
        .name = "TSDB", .base_addr = 0, .total_size = TSDB_PART_SIZE,
        .sector_size = SECTOR_SIZE, .write_gran = DEFAULT_WG, .ops = &g_ops
    };
    g_db.part = &g_part;
    g_db.erase_cnts = g_ec;
    g_db.sector_cnt = (uint8_t)TS_SECTOR_CNT;
    trace_event(&g_trace, "TSDB format+init (stress)");
    rdb_err_t ret = rdb_tsdb_format(&g_db);
    if (ret != RDB_OK) return ret;
    ret = rdb_tsdb_init(&g_db, &g_part, g_ec);
    if (ret == RDB_OK) trace_tsdb_snapshot(&g_trace, &g_db);
    return ret;
}

static uint32_t ts_data_start(void)
{
    return RDB_ALIGN_UP((uint32_t)sizeof(rdb_ts_sector_hdr_t), 1u << DEFAULT_WG);
}

/* ── Size PRNG for rotation stress ─────────────────────────────────────── */

static uint32_t g_ts_sz_prng = 0xBEEF1234u;

static uint16_t ts_sz_next(void)
{
    g_ts_sz_prng ^= g_ts_sz_prng << 13;
    g_ts_sz_prng ^= g_ts_sz_prng >> 17;
    g_ts_sz_prng ^= g_ts_sz_prng << 5;
    return (uint16_t)(g_ts_sz_prng & 0xFFFFu);
}

/* ═══════════════════════════════════════════════════════════════════════════
 *  T-313: rotation stress
 * ═══════════════════════════════════════════════════════════════════════════ */

#define ROT_TARGET   100u
#define ROT_MAX_LOOPS 500000u

TEST_CASE(ts_rotation_stress, "TSDB", "Rotation stress >=100")
{
    (void)ctx;
    TEST_ASSERT_RDB_OK(ts_reset());
    trace_event(&g_trace, "  [ROT-STRESS] start: target=%u seed=0x%08X",
                ROT_TARGET, g_ts_sz_prng);

    uint8_t data[512];
    memset(data, 0x5A, sizeof(data));

    uint32_t loops = 0, time = 1, prev_rot = g_db.stats.sector_rotations;
    while (g_db.stats.sector_rotations < ROT_TARGET && loops < ROT_MAX_LOOPS) {
        uint16_t dsz = 1u + (uint16_t)(ts_sz_next() % 512u);
        TEST_ASSERT_RDB_OK(trace_ts_append(&g_db, time++, data, dsz));
        loops++;
        if (g_db.stats.sector_rotations != prev_rot) {
            prev_rot = g_db.stats.sector_rotations;
            trace_tsdb_rot_event(&g_trace, &g_db, prev_rot, loops);
        }
        if (time % 500 == 0)
            trace_event(&g_trace, "  [TS-ROT] time=%u loops=%u rotations=%u",
                        time - 1, loops, g_db.stats.sector_rotations);
    }
    TEST_ASSERT_GE(g_db.stats.sector_rotations, ROT_TARGET);
    trace_event(&g_trace, "TS-ROT done: time=%u loops=%u rotations=%u",
                time - 1, loops, g_db.stats.sector_rotations);
    return 0;
}

/* ═══════════════════════════════════════════════════════════════════════════
 *  T-312: append failure handling
 * ═══════════════════════════════════════════════════════════════════════════ */

TEST_CASE(ts_append_fail_once, "TSDB", "Append failure does not corrupt DB")
{
    (void)ctx;
    TEST_ASSERT_RDB_OK(ts_reset());

    uint8_t data[8] = { 1, 2, 3, 4, 5, 6, 7, 8 };

    trace_event(&g_trace, "  [TS-FAIL] appending ts=1 with write-fail injected");
    fault_quick_write_fail(&g_fault, g_fault.write_count + 1u);
    TEST_ASSERT_NE(trace_ts_append(&g_db, 1, data, sizeof(data)), RDB_OK);

    fault_clear_rules(&g_fault);
    trace_event(&g_trace, "  [TS-FAIL] appending ts=2 (should succeed)");
    TEST_ASSERT_RDB_OK(trace_ts_append(&g_db, 2, data, sizeof(data)));

    TEST_ASSERT_EQ(rdb_tsdb_count(&g_db), 1u);

    uint32_t time = 0; uint8_t out[8]; uint16_t out_len = 0;
    TEST_ASSERT_RDB_OK(rdb_tsdb_get_latest(&g_db, &time, out, sizeof(out), &out_len));
    TEST_ASSERT_EQ(time, 2u);
    TEST_ASSERT_EQ(out_len, (uint16_t)sizeof(data));
    trace_event(&g_trace, "  [TS-FAIL] get_latest: time=%u len=%u", time, out_len);
    return 0;
}

/* ═══════════════════════════════════════════════════════════════════════════
 *  T-315: CRC corruption detection
 * ═══════════════════════════════════════════════════════════════════════════ */

#define CRC_RECORDS 10u

static int find_last_record(uint32_t *out_addr, rdb_ts_record_hdr_t *out_hdr)
{
    if (!out_addr || !out_hdr) return -1;
    uint32_t base = g_db.head_sec * g_part.sector_size;
    uint32_t off = ts_data_start(), end = g_db.head_off;
    uint32_t g = 1u << DEFAULT_WG;
    int found = 0;

    while (off + sizeof(rdb_ts_record_hdr_t) <= end) {
        rdb_ts_record_hdr_t rh;
        sim_flash_read(&g_flash, base + off, (uint8_t*)&rh, sizeof(rh));
        if (rh.magic == 0xFFu && rh.state == 0xFFu) break;
        if (rh.magic != RDB_TS_RECORD_MAGIC || rh.data_len == 0 ||
            rh.data_len > g_db.max_data_len) {
            off += (uint16_t)RDB_ALIGN_UP(sizeof(rdb_ts_record_hdr_t), g);
            continue;
        }
        uint32_t rsz = (uint32_t)sizeof(rdb_ts_record_hdr_t) +
                       RDB_ALIGN_UP(rh.data_len, g);
        if (off + rsz > end) break;
        *out_addr = base + off;
        *out_hdr = rh;
        found = 1;
        off += rsz;
    }
    return found ? 0 : -1;
}

typedef struct { uint32_t total, null_count; uint16_t expect_len; } ts_crc_ctx_t;

static int ts_crc_cb(uint32_t t, const void *data, uint16_t len, void *arg)
{
    (void)t;
    ts_crc_ctx_t *ctx = (ts_crc_ctx_t *)arg;
    ctx->total++;
    if (!data) { ctx->null_count++; TEST_ASSERT_EQ(len, ctx->expect_len); }
    return RDB_ITER_CONTINUE;
}

TEST_CASE(ts_crc_corruption, "TSDB", "CRC corruption reported in query/get_latest")
{
    (void)ctx;
    TEST_ASSERT_RDB_OK(ts_reset());

    uint8_t payload[16];
    for (uint8_t i = 0; i < sizeof(payload); i++) payload[i] = (uint8_t)(0xC0u + i);
    for (uint32_t i = 1; i <= CRC_RECORDS; i++) {
        TEST_ASSERT_RDB_OK(trace_ts_append(&g_db, i, payload, (uint16_t)sizeof(payload)));
    }
    trace_event(&g_trace, "  [TS-CRC] wrote %u records ts=1..%u", CRC_RECORDS, CRC_RECORDS);

    uint32_t addr = 0; rdb_ts_record_hdr_t rh;
    TEST_ASSERT_EQ(find_last_record(&addr, &rh), 0);
    g_flash.mem[addr + (uint32_t)sizeof(rdb_ts_record_hdr_t)] ^= 0xFFu; /* intentional corruption: bypass NOR rules */
    trace_event(&g_trace, "  [TS-CRC] corrupted record at addr=0x%08X", addr);

    uint8_t out[16]; uint16_t out_len = 0; uint32_t out_time = 0;
    TEST_ASSERT_RDB_ERR(rdb_tsdb_get_latest(&g_db, &out_time, out, sizeof(out), &out_len),
                        RDB_ERR_CRC);
    trace_event(&g_trace, "  [TS-CRC] get_latest correctly returned RDB_ERR_CRC");

    ts_crc_ctx_t qctx = { 0 };
    qctx.expect_len = (uint16_t)sizeof(payload);
    TEST_ASSERT_RDB_OK(rdb_tsdb_query(&g_db, 1, CRC_RECORDS, ts_crc_cb, &qctx));
    TEST_ASSERT_EQ(qctx.total, CRC_RECORDS);
    TEST_ASSERT_GE(qctx.null_count, 1u);
    trace_event(&g_trace, "  [TS-CRC] query ts=1..%u: total=%u null=%u",
                CRC_RECORDS, qctx.total, qctx.null_count);
    return 0;
}

/* ═══════════════════════════════════════════════════════════════════════════
 *  T-316: degraded ACTIVE sector recovery
 * ═══════════════════════════════════════════════════════════════════════════ */

#define DG_TARGET_ROT 4u

static int corrupt_sealed_header(void)
{
    for (uint8_t s = 0; s < g_db.sector_cnt; s++) {
        if (s == g_db.head_sec) continue;
        uint32_t base = (uint32_t)s * g_part.sector_size;
        rdb_ts_sector_hdr_t h;
        sim_flash_read(&g_flash, base, (uint8_t*)&h, sizeof(h));
        if (h.magic != RDB_TS_SECTOR_MAGIC) continue;
        if (h.count == 0xFFFFu || h.end_off == 0xFFFFu) continue;
        uint16_t calc = rdb_crc16(&h, 18);
        if (calc != h.hdr_crc) continue;
        uint16_t bad = (uint16_t)(h.hdr_crc ^ 0xFFFFu);
        memcpy(g_flash.mem + base + 18, &bad, sizeof(bad)); /* intentional corruption: bypass NOR rules */
        return 0;
    }
    return -1;
}

TEST_CASE(ts_degraded_active_recovery, "TSDB", "Recover from degraded ACTIVE sector")
{
    (void)ctx;
    TEST_ASSERT_RDB_OK(ts_reset());

    uint8_t data[64];
    memset(data, 0x5Au, sizeof(data));

    uint32_t time = 1;
    while (g_db.stats.sector_rotations < DG_TARGET_ROT)
        TEST_ASSERT_RDB_OK(trace_ts_append(&g_db, time++, data, sizeof(data)));
    trace_event(&g_trace, "  [TS-DEGRADED] wrote ts=1..%u rotations=%u count=%u",
                time - 1, g_db.stats.sector_rotations, g_db.total_count);

    uint32_t expected_oldest = 1u;
    uint32_t expected_newest = g_db.last_time;
    uint32_t expected_count = g_db.total_count;

    TEST_ASSERT_EQ(corrupt_sealed_header(), 0);
    trace_event(&g_trace, "  [TS-DEGRADED] corrupted sealed sector header, re-initing");
    TEST_ASSERT_RDB_OK(rdb_tsdb_init(&g_db, &g_part, g_ec));

    uint32_t oldest = 0, newest = 0;
    rdb_tsdb_time_range(&g_db, &oldest, &newest);
    TEST_ASSERT_EQ(oldest, expected_oldest);
    TEST_ASSERT_EQ(newest, expected_newest);
    TEST_ASSERT_EQ(rdb_tsdb_count(&g_db), expected_count);
    trace_event(&g_trace, "  [TS-DEGRADED] recovered: oldest=%u newest=%u count=%u",
                oldest, newest, g_db.total_count);
    return 0;
}

/* ═══════════════════════════════════════════════════════════════════════════
 *  TC-X-05: Mixed-length payload rotation stress (1..max_data_len random)
 *
 *  Uses a deterministic PRNG to generate payload sizes from 1..max_data_len
 *  with exponential bias (small values more common, but full range exercised).
 *  Each payload is stamped with (timestamp & 0xFFFF) in the first 2 bytes
 *  for verification.  Drives sector rotations with realistic size diversity.
 * ═══════════════════════════════════════════════════════════════════════════ */

#define MIX_TS_ROT_TARGET  100u
#define MIX_TS_MAX_LOOPS   500000u
#define MIX_TS_SEED        0xBEEF5678u

static uint32_t ts_xorshift(void)
{
    static uint32_t s = MIX_TS_SEED;
    s ^= s << 13; s ^= s >> 17; s ^= s << 5;
    return s;
}

static uint16_t ts_rand_size(uint32_t max)
{
    /* §4.2.1 piecewise: 40% 1-32, 40% 33-256, 15% 257-1024, 4% 1025-max-1, 1% max */
    uint32_t r = ts_xorshift() % 100u;
    uint32_t sz;
    if (r < 40u) {
        sz = 1u + (ts_xorshift() % 32u);
    } else if (r < 80u) {
        sz = 33u + (ts_xorshift() % 224u);
    } else if (r < 95u) {
        sz = 257u + (ts_xorshift() % 768u);
    } else if (r < 99u) {
        if (max > 1025u) sz = 1025u + (ts_xorshift() % (max - 1025u));
        else             sz = max;
    } else {
        sz = max;
    }
    if (sz > max) sz = max;
    if (sz < 1u) sz = 1u;
    return (uint16_t)sz;
}

typedef struct {
    uint32_t count;
    uint32_t time_first;
    uint32_t time_last;
    uint32_t stamp_mismatches;
    uint32_t oversize;
    uint16_t max_len;
} ts_mix_verify_t;

static int ts_mix_verify_cb(uint32_t t, const void *data, uint16_t len, void *arg)
{
    ts_mix_verify_t *c = (ts_mix_verify_t *)arg;
    if (c->count == 0) c->time_first = t;
    c->time_last = t;
    if (len > c->max_len) c->oversize++;
    if (len >= 2 && data) {
        const uint8_t *p = (const uint8_t *)data;
        uint16_t expect = (uint16_t)(t & 0xFFFFu);
        uint16_t actual = (uint16_t)(((uint16_t)p[0]) | ((uint16_t)p[1] << 8));
        if (actual != expect) c->stamp_mismatches++;
    }
    /* Log first 3 and every 20th record */
    if (c->count < 3 || c->count % 20 == 0)
        trace_event(&g_trace, "  [TS-QUERY #%u] time=%u len=%u",
                    c->count, t, len);
    c->count++;
    return RDB_ITER_CONTINUE;
}

TEST_CASE(ts_mixed_payload_stress, "TSDB", "Random mixed-length payload rotation stress (1..max)")
{
    (void)ctx;
    TEST_ASSERT_RDB_OK(ts_reset());
    trace_event(&g_trace, "  [MIXED-PL] start: seed=0x%08X", MIX_TS_SEED);
    trace_tsdb_geometry(&g_trace, &g_db);

    uint16_t max_dl = g_db.max_data_len;
    TEST_ASSERT_GT(max_dl, 0u);

    uint8_t data[4096];

    uint16_t max_size = 0;
    uint32_t size_bins[5] = {0};
    uint32_t large_writes = 0;
    uint32_t loops = 0, time = 1, prev_rot = g_db.stats.sector_rotations;
    while (g_db.stats.sector_rotations < MIX_TS_ROT_TARGET && loops < MIX_TS_MAX_LOOPS) {
        uint16_t sz = ts_rand_size(max_dl);
        if (sz > max_size) max_size = sz;
        if      (sz <= 32)   size_bins[0]++;
        else if (sz <= 256)  size_bins[1]++;
        else if (sz <= 1024) size_bins[2]++;
        else if (sz < max_dl) size_bins[3]++;
        else                  size_bins[4]++;
        if (sz >= 1000) {
            trace_event(&g_trace, "  [LARGE-TS #%u] payload_len=%u time=%u rot=%u",
                        large_writes, sz, time, g_db.stats.sector_rotations);
            large_writes++;
        }
        data[0] = (uint8_t)(time & 0xFFu);
        data[1] = (uint8_t)((time >> 8) & 0xFFu);
        for (uint16_t b = 2; b < sz; b++)
            data[b] = (uint8_t)(ts_xorshift() & 0xFFu);
        rdb_err_t rc = trace_ts_append(&g_db, time, data, sz);
        TEST_ASSERT(rc == RDB_OK || rc == RDB_ERR_TOO_LARGE);
        time++; loops++;
        if (g_db.stats.sector_rotations != prev_rot) {
            prev_rot = g_db.stats.sector_rotations;
            trace_tsdb_rot_event(&g_trace, &g_db, prev_rot, loops);
        }
    }
    TEST_ASSERT_GE(g_db.stats.sector_rotations, MIX_TS_ROT_TARGET);

    trace_event(&g_trace, "Mixed-TS size distribution (records=%u large=%u): "
                "1..32=%u 33..256=%u 257..1024=%u 1025..%u=%u max=%u",
                time - 1, large_writes, size_bins[0], size_bins[1],
                size_bins[2], max_dl - 1u, size_bins[3], size_bins[4]);
    trace_event(&g_trace, "Mixed-TS max_val=%uB loops=%u rotations=%u",
                max_size, loops, g_db.stats.sector_rotations);

    TEST_ASSERT_GE(max_size, (uint16_t)(max_dl * 4u / 5u));

    /* Query surviving range: ring buffer wraps with >=100 rotations */
    uint32_t oldest = 0, newest = 0;
    rdb_tsdb_time_range(&g_db, &oldest, &newest);
    TEST_ASSERT_GT(oldest, 0u);
    TEST_ASSERT_GE(newest, oldest);
    trace_event(&g_trace, "  [TS-QUERY] range ts=%u..%u (db_count=%u)",
                oldest, newest, rdb_tsdb_count(&g_db));

    ts_mix_verify_t vfy = { 0, 0, 0, 0, 0, max_dl };
    TEST_ASSERT_RDB_OK(rdb_tsdb_query(&g_db, oldest, newest,
                                       ts_mix_verify_cb, &vfy));
    TEST_ASSERT_EQ(vfy.count, rdb_tsdb_count(&g_db));
    TEST_ASSERT_EQ(vfy.oversize, 0u);
    TEST_ASSERT_EQ(vfy.stamp_mismatches, 0u);
    trace_event(&g_trace, "  [TS-QUERY] result: count=%u stamp_mismatches=%u",
                vfy.count, vfy.stamp_mismatches);

    return 0;
}

/* ═══════════════════════════════════════════════════════════════════════════
 *  Entry point
 * ═══════════════════════════════════════════════════════════════════════════ */

static void post_test_tsdb_sectors(const char *name, int result, void *ctx)
{
    (void)name; (void)result; (void)ctx;
    trace_tsdb_sector_summary(&g_trace, &g_db);
}

int main(void)
{
    test_config_t config = {
        .log_file = fopen(test_make_log_path("tsdb_stress"), "w"),
        .verbose = 1, .stop_on_fail = 0, .filter = NULL,
        .post_test_hook = post_test_tsdb_sectors, .hook_ctx = NULL
    };
    test_framework_init(&config);

    trace_init(&g_trace, config.log_file, config.verbose);
    sim_flash_set_trace(&g_flash, &g_trace);
    trace_event(&g_trace, "=== TSDB Stress Test Suite Start ===");

    test_suite_t *s = test_get_default_suite();
    test_register_case(s, &test_case_ts_rotation_stress);
    test_register_case(s, &test_case_ts_append_fail_once);
    test_register_case(s, &test_case_ts_crc_corruption);
    test_register_case(s, &test_case_ts_degraded_active_recovery);
    test_register_case(s, &test_case_ts_mixed_payload_stress);

    test_run_all(NULL);

    trace_event(&g_trace, "=== TSDB Stress Test Suite End ===\n");
    trace_tsdb_stats(&g_trace, &g_db);

    test_print_report();
    if (config.log_file) fclose(config.log_file);

    test_stats_t stats; test_get_stats(&stats);
    return (stats.failed_cases == 0) ? 0 : 1;
}
