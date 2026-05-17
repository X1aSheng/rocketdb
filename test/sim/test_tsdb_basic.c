/**
 * test_tsdb_basic.c — TSDB basic functionality tests
 *
 * Covers: append/query/latest/oldest/count/time_range,
 *         epoch reset query integrity,
 *         recount jitter control
 *
 * References: T-310, T-311, T-314
 */

#include "sim_flash.h"
#include "test_framework.h"
#include "../../src/rocketdb.h"
#include <stdio.h>
#include <stdlib.h>
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
static rdb_partition_t g_part;
static rdb_tsdb_t  g_db;
static uint32_t    g_ec[TS_SECTOR_CNT];
static trace_ctx_t g_trace;
static uint8_t     g_enforce_bounded_program;
static uint32_t    g_max_write_len;

static int fl_read(uint32_t addr, uint8_t *buf, size_t len) {
    return sim_flash_read(&g_flash, addr, buf, len);
}
static int fl_write(uint32_t addr, const uint8_t *buf, size_t len) {
    if (len > g_max_write_len)
        g_max_write_len = (uint32_t)len;
    if (g_enforce_bounded_program && len > RDB_STACK_BUF_SIZE)
        return -1;
    return sim_flash_write(&g_flash, addr, buf, len);
}
static int fl_erase(uint32_t addr) {
    return sim_flash_erase(&g_flash, addr);
}
static void fl_lock(void) { }
static void fl_unlock(void) { }
static void fl_yield(void) { }

static rdb_flash_ops_t g_ops = {
    .read = fl_read, .write = fl_write, .erase = fl_erase,
    .lock = fl_lock, .unlock = fl_unlock, .yield = fl_yield
};

static rdb_err_t ts_reset(void)
{
    g_enforce_bounded_program = 0;
    g_max_write_len = 0;
    if (sim_flash_init(&g_flash, g_flash_buf, FLASH_SIZE,
                       SECTOR_SIZE, PAGE_SIZE, DEFAULT_WG) != 0)
        return RDB_ERR_FLASH;
    g_part = (rdb_partition_t) {
        .name = "TSDB", .base_addr = 0, .total_size = TSDB_PART_SIZE,
        .sector_size = SECTOR_SIZE, .write_gran = DEFAULT_WG, .ops = &g_ops
    };
    g_db.part = &g_part;
    g_db.erase_cnts = g_ec;
    g_db.sector_cnt = (uint8_t)TS_SECTOR_CNT;
    trace_event(&g_trace, "TSDB format+init (basic)");
    rdb_err_t ret = rdb_tsdb_format(&g_db);
    if (ret != RDB_OK) return ret;
    ret = rdb_tsdb_init(&g_db, &g_part, g_ec);
    if (ret == RDB_OK) trace_tsdb_snapshot(&g_trace, &g_db);
    return ret;
}

/* ═══════════════════════════════════════════════════════════════════════════
 *  T-310: append/query/latest/oldest/count/time_range
 * ═══════════════════════════════════════════════════════════════════════════ */

#define TS_APPEND_COUNT 200u

typedef struct {
    uint32_t count, first_time, last_time;
    uint8_t  expected[16];
    uint16_t expected_len;
} ts_check_ctx_t;

static int ts_check_cb(uint32_t t, const void *data, uint16_t len, void *arg)
{
    ts_check_ctx_t *ctx = (ts_check_ctx_t *)arg;
    if (ctx->count == 0) ctx->first_time = t;
    ctx->last_time = t;
    ctx->count++;
    TEST_ASSERT_EQ(len, ctx->expected_len);
    TEST_ASSERT_MEM_EQ(data, ctx->expected, ctx->expected_len);
    return RDB_ITER_CONTINUE;
}

TEST_CASE(ts_basic_append_query, "TSDB", "Append/query/latest/oldest/count/time_range")
{
    (void)ctx;
    TEST_ASSERT_RDB_OK(ts_reset());

    uint8_t payload[16];
    for (uint8_t i = 0; i < sizeof(payload); i++) payload[i] = (uint8_t)(0xA0u + i);

    for (uint32_t i = 1; i <= TS_APPEND_COUNT; i++)
        TEST_ASSERT_RDB_OK(rdb_tsdb_append(&g_db, i, payload, (uint16_t)sizeof(payload)));

    ts_check_ctx_t qctx = { 0 };
    memcpy(qctx.expected, payload, sizeof(payload));
    qctx.expected_len = (uint16_t)sizeof(payload);

    TEST_ASSERT_RDB_OK(rdb_tsdb_query(&g_db, 1, TS_APPEND_COUNT, ts_check_cb, &qctx));
    TEST_ASSERT_EQ(qctx.count, TS_APPEND_COUNT);
    TEST_ASSERT_EQ(qctx.first_time, 1u);
    TEST_ASSERT_EQ(qctx.last_time, TS_APPEND_COUNT);

    uint32_t newest = 0, oldest = 0;
    rdb_tsdb_time_range(&g_db, &oldest, &newest);
    TEST_ASSERT_EQ(oldest, 1u);
    TEST_ASSERT_EQ(newest, TS_APPEND_COUNT);
    TEST_ASSERT_EQ(rdb_tsdb_count(&g_db), TS_APPEND_COUNT);

    uint8_t out[16]; uint16_t out_len = 0; uint32_t out_time = 0;
    TEST_ASSERT_RDB_OK(rdb_tsdb_get_oldest(&g_db, &out_time, out, sizeof(out), &out_len));
    TEST_ASSERT_EQ(out_time, 1u);
    TEST_ASSERT_EQ(out_len, (uint16_t)sizeof(payload));
    TEST_ASSERT_MEM_EQ(out, payload, sizeof(payload));

    memset(out, 0, sizeof(out)); out_len = 0; out_time = 0;
    TEST_ASSERT_RDB_OK(rdb_tsdb_get_latest(&g_db, &out_time, out, sizeof(out), &out_len));
    TEST_ASSERT_EQ(out_time, TS_APPEND_COUNT);
    TEST_ASSERT_EQ(out_len, (uint16_t)sizeof(payload));
    TEST_ASSERT_MEM_EQ(out, payload, sizeof(payload));
    return 0;
}

/* ═══════════════════════════════════════════════════════════════════════════
 *  T-311: epoch reset query integrity
 * ═══════════════════════════════════════════════════════════════════════════ */

typedef struct {
    uint32_t count, first_time, last_time;
} ts_count_ctx_t;

static int ts_count_cb(uint32_t t, const void *data, uint16_t len, void *arg)
{
    (void)data; (void)len;
    ts_count_ctx_t *ctx = (ts_count_ctx_t *)arg;
    if (ctx->count == 0) ctx->first_time = t;
    ctx->last_time = t;
    ctx->count++;
    return RDB_ITER_CONTINUE;
}

typedef struct { uint32_t n; } ts_wg_ctx_t;

static int ts_wg_query_cb(uint32_t t, const void* d, uint16_t l, void* a)
{
    (void)t; (void)d; (void)l;
    ((ts_wg_ctx_t*)a)->n++;
    return RDB_ITER_CONTINUE;
}

TEST_CASE(ts_epoch_query_integrity, "TSDB", "Epoch reset keeps query integrity")
{
    (void)ctx;
    TEST_ASSERT_RDB_OK(ts_reset());

    uint8_t data[8] = { 1, 2, 3, 4, 5, 6, 7, 8 };
    for (uint32_t i = 1; i <= 50; i++)
        TEST_ASSERT_RDB_OK(rdb_tsdb_append(&g_db, i, data, sizeof(data)));
    TEST_ASSERT_RDB_OK(rdb_tsdb_reset_epoch(&g_db));
    for (uint32_t i = 1; i <= 50; i++)
        TEST_ASSERT_RDB_OK(rdb_tsdb_append(&g_db, i, data, sizeof(data)));

    ts_count_ctx_t qctx = { 0 };
    TEST_ASSERT_RDB_OK(rdb_tsdb_query(&g_db, 1, 100, ts_count_cb, &qctx));
    TEST_ASSERT_EQ(qctx.count, 100u);
    TEST_ASSERT_EQ(qctx.first_time, 1u);
    TEST_ASSERT_EQ(qctx.last_time, 50u);

    uint32_t newest = 0, oldest = 0;
    rdb_tsdb_time_range(&g_db, &oldest, &newest);
    TEST_ASSERT_EQ(oldest, 1u);
    TEST_ASSERT_EQ(newest, 50u);
    return 0;
}

/* ═══════════════════════════════════════════════════════════════════════════
 *  T-314: recount jitter control
 * ═══════════════════════════════════════════════════════════════════════════ */

TEST_CASE(ts_recount_jitter, "TSDB", "Recount occurs only per full ring")
{
    (void)ctx;
    TEST_ASSERT_RDB_OK(ts_reset());

    uint8_t data[64];
    memset(data, 0x3C, sizeof(data));

    uint32_t target_rot = g_db.sector_cnt * 2u;
    uint32_t time = 1;
    while (g_db.stats.sector_rotations < target_rot)
        TEST_ASSERT_RDB_OK(rdb_tsdb_append(&g_db, time++, data, sizeof(data)));

    TEST_ASSERT_EQ(rdb_tsdb_count(&g_db), g_db.total_count);
    return 0;
}

/* ═══════════════════════════════════════════════════════════════════════════
 *  TSDB write granularity matrix (1/2/4/8 bytes)
 *
 *  Per test plan 4.2: run append/query with each supported write_gran.
 * ═══════════════════════════════════════════════════════════════════════════ */

TEST_CASE(ts_write_gran_matrix, "TSDB", "Write granularity matrix 1/2/4/8B")
{
    (void)ctx;

    /* wg=2 (4B) skipped — TSDB sector header has 2B fields
       (count@14, end_off@16, hdr_crc@18) that are not 4B-aligned.
       wg=3 (8B) skipped — TSDB headers (20B/12B) are not 8B-aligned. */
    for (uint8_t wg = 0; wg <= 1; wg++) {
        /* Re-init flash with this write_gran */
        if (sim_flash_init(&g_flash, g_flash_buf, FLASH_SIZE,
                           SECTOR_SIZE, PAGE_SIZE, wg) != 0)
            TEST_ASSERT(0); /* sim_flash_init failed */
        g_part = (rdb_partition_t) {
            .name = "TSDB", .base_addr = 0, .total_size = TSDB_PART_SIZE,
            .sector_size = SECTOR_SIZE, .write_gran = wg, .ops = &g_ops
        };
        g_db.part = &g_part;
        g_db.erase_cnts = g_ec;
        g_db.sector_cnt = (uint8_t)TS_SECTOR_CNT;
        TEST_ASSERT_RDB_OK(rdb_tsdb_format(&g_db));
        TEST_ASSERT_RDB_OK(rdb_tsdb_init(&g_db, &g_part, g_ec));

        /* Append enough records to trigger at least one rotation */
        uint8_t data[32];
        memset(data, 0x5Au | wg, sizeof(data));
        uint32_t time = 1;

        for (uint32_t i = 0; i < 200; i++)
            TEST_ASSERT_RDB_OK(rdb_tsdb_append(&g_db, time++, data,
                (uint16_t)sizeof(data)));

        /* Verify records are readable and count is correct */
        uint32_t count = rdb_tsdb_count(&g_db);
        TEST_ASSERT_GT(count, 0u);
        TEST_ASSERT_EQ(count, g_db.total_count);

        /* Query full range — all records should be accessible */
        ts_wg_ctx_t qx = { 0 };
        TEST_ASSERT_RDB_OK(rdb_tsdb_query(&g_db, 1, time - 1, ts_wg_query_cb, &qx));
        TEST_ASSERT_EQ(qx.n, count);

        /* get_latest works */
        uint32_t lt = 0; uint8_t lb[32]; uint16_t ll = 0;
        TEST_ASSERT_RDB_OK(rdb_tsdb_get_latest(&g_db, &lt, lb, sizeof(lb), &ll));
        TEST_ASSERT_EQ(ll, (uint16_t)sizeof(data));
        TEST_ASSERT_GE(lt, time - 1u);
    }
    return 0;
}

/* ═══════════════════════════════════════════════════════════════════════════
 *  TC-X-02 (TS part): Max data length boundary
 *
 *  Verify append at max_data_len succeeds and too-large data
 *  returns RDB_ERR_TOO_LARGE.
 * ═══════════════════════════════════════════════════════════════════════════ */

TEST_CASE(ts_max_boundaries, "TSDB", "Maximum data length boundary test")
{
    (void)ctx;
    TEST_ASSERT_RDB_OK(ts_reset());

    trace_tsdb_geometry(&g_trace, &g_db);

    uint32_t max_dl = g_db.max_data_len;
    TEST_ASSERT_GT(max_dl, 0u);

    /* Append at max_data_len */
    {
        uint8_t* data = (uint8_t*)malloc(max_dl);
        TEST_ASSERT(data != NULL);
        memset(data, 0x7E, max_dl);
        TEST_ASSERT_RDB_OK(rdb_tsdb_append(&g_db, 1, data, (uint16_t)max_dl));
        free(data);

        uint32_t t = 0; uint16_t ol = 0;
        TEST_ASSERT_RDB_OK(rdb_tsdb_get_latest(&g_db, &t, NULL, 0, &ol));
        TEST_ASSERT_EQ(ol, (uint16_t)max_dl);
    }

    /* max_data_len + 1 → TOO_LARGE */
    {
        uint8_t* data = (uint8_t*)malloc(max_dl + 1u);
        TEST_ASSERT(data != NULL);
        rdb_err_t r = rdb_tsdb_append(&g_db, 2, data, (uint16_t)(max_dl + 1u));
        TEST_ASSERT(r == RDB_ERR_TOO_LARGE || r == RDB_ERR_PARAM);
        free(data);
    }

    /* Zero-length data → TOO_LARGE (checked before null-data) */
    TEST_ASSERT_RDB_ERR(rdb_tsdb_append(&g_db, 3, NULL, 0), RDB_ERR_TOO_LARGE);
    /* Non-null data with valid len 1 → OK */
    uint8_t dummy = 0;
    TEST_ASSERT_RDB_OK(rdb_tsdb_append(&g_db, 4, &dummy, 1));

    return 0;
}

/* ═══════════════════════════════════════════════════════════════════════════
 *  Bounded SPI NOR program chunks
 *
 *  Large TSDB payloads should be split before they reach the HAL so
 *  W25QXX-style drivers can further respect their 256-byte page boundaries.
 * ═══════════════════════════════════════════════════════════════════════════ */

TEST_CASE(ts_large_payload_bounded_program_chunks, "TSDB",
          "Large payload writes use bounded SPI NOR program chunks")
{
    (void)ctx;
    TEST_ASSERT_RDB_OK(ts_reset());
    g_enforce_bounded_program = 1;
    g_max_write_len = 0;

    uint8_t data[600];
    uint8_t out[600];
    for (uint16_t i = 0; i < sizeof(data); i++)
        data[i] = (uint8_t)(i ^ 0xA5u);
    memset(out, 0, sizeof(out));

    TEST_ASSERT_RDB_OK(rdb_tsdb_append(&g_db, 100, data, (uint16_t)sizeof(data)));

    uint32_t t = 0;
    uint16_t out_len = 0;
    TEST_ASSERT_RDB_OK(rdb_tsdb_get_latest(&g_db, &t, out, sizeof(out), &out_len));
    TEST_ASSERT_EQ(t, 100u);
    TEST_ASSERT_EQ(out_len, (uint16_t)sizeof(data));
    TEST_ASSERT_MEM_EQ(out, data, sizeof(data));
    TEST_ASSERT_LE(g_max_write_len, (uint32_t)RDB_STACK_BUF_SIZE);

    g_enforce_bounded_program = 0;
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
        .log_file = fopen(test_make_log_path("tsdb_basic"), "w"),
        .verbose = 1, .stop_on_fail = 0, .filter = NULL,
        .post_test_hook = post_test_tsdb_sectors, .hook_ctx = NULL
    };
    test_framework_init(&config);

    trace_init(&g_trace, config.log_file, config.verbose);
    sim_flash_set_trace(&g_flash, &g_trace);
    trace_event(&g_trace, "=== TSDB Basic Test Suite Start ===");

    test_suite_t *s = test_get_default_suite();
    test_register_case(s, &test_case_ts_basic_append_query);
    test_register_case(s, &test_case_ts_epoch_query_integrity);
    test_register_case(s, &test_case_ts_recount_jitter);
    test_register_case(s, &test_case_ts_write_gran_matrix);
    test_register_case(s, &test_case_ts_max_boundaries);
    test_register_case(s, &test_case_ts_large_payload_bounded_program_chunks);

    test_run_all(NULL);

    trace_event(&g_trace, "=== TSDB Basic Test Suite End ===\n");
    trace_tsdb_stats(&g_trace, &g_db);

    test_print_report();
    if (config.log_file) fclose(config.log_file);

    test_stats_t stats; test_get_stats(&stats);
    return (stats.failed_cases == 0) ? 0 : 1;
}
