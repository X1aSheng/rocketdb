/**
 * test_integration.c — KVDB + TSDB integration tests
 *
 * Covers: GC/rotation cycle stress, mixed workload,
 *         power-loss injection (KV+TS), wear-leveling heatmap
 *
 * References: T-400, T-401, T-402, T-403, T-404
 */

#include "sim_flash.h"
#include "sim_fault.h"
#include "sim_dist.h"
#include "test_framework.h"
#include "../../src/rocketdb.h"
#include <stdio.h>
#include <string.h>

#define FLASH_SIZE      (128u * 1024u)
#define SECTOR_SIZE     4096u
#define PAGE_SIZE       256u
#define DEFAULT_WG      0u
#define KVDB_PART_SIZE  (64u * 1024u)
#define TSDB_PART_SIZE  (64u * 1024u)
#define KV_SECTOR_CNT   (KVDB_PART_SIZE / SECTOR_SIZE)
#define TS_SECTOR_CNT   (TSDB_PART_SIZE / SECTOR_SIZE)

/* ── KV flash ──────────────────────────────────────────────────────────── */

static uint8_t            g_kv_flash_buf[FLASH_SIZE];
static sim_flash_t        g_kv_flash;
static fault_ctx_t        g_kv_fault;
static rdb_partition_t    g_kv_part;
static rdb_kvdb_t         g_kv_db;
static rdb_kv_sector_meta_t g_kv_meta[KV_SECTOR_CNT];

static int kv_read(uint32_t addr, uint8_t *buf, size_t len) {
    return sim_flash_read(&g_kv_flash, addr, buf, len);
}
static int kv_write(uint32_t addr, const uint8_t *buf, size_t len) {
    return sim_flash_write(&g_kv_flash, addr, buf, len);
}
static int kv_erase(uint32_t addr) {
    return sim_flash_erase(&g_kv_flash, addr);
}
static void kv_lock(void) { }
static void kv_unlock(void) { }
static void kv_yield(void) { }

static rdb_flash_ops_t g_kv_ops = {
    .read = kv_read, .write = kv_write, .erase = kv_erase,
    .lock = kv_lock, .unlock = kv_unlock, .yield = kv_yield
};

/* ── TS flash ──────────────────────────────────────────────────────────── */

static uint8_t     g_ts_flash_buf[FLASH_SIZE];
static sim_flash_t g_ts_flash;
static fault_ctx_t g_ts_fault;
static rdb_partition_t g_ts_part;
static rdb_tsdb_t  g_ts_db;
static uint32_t    g_ts_ec[TS_SECTOR_CNT];
static trace_ctx_t g_trace;

static int ts_read(uint32_t addr, uint8_t *buf, size_t len) {
    return sim_flash_read(&g_ts_flash, addr, buf, len);
}
static int ts_write(uint32_t addr, const uint8_t *buf, size_t len) {
    return sim_flash_write(&g_ts_flash, addr, buf, len);
}
static int ts_erase(uint32_t addr) {
    return sim_flash_erase(&g_ts_flash, addr);
}
static void ts_lock(void) { }
static void ts_unlock(void) { }
static void ts_yield(void) { }

static rdb_flash_ops_t g_ts_ops = {
    .read = ts_read, .write = ts_write, .erase = ts_erase,
    .lock = ts_lock, .unlock = ts_unlock, .yield = ts_yield
};

/* ── Reset helpers ─────────────────────────────────────────────────────── */

static rdb_err_t kv_reset(void)
{
    if (sim_flash_init(&g_kv_flash, g_kv_flash_buf, FLASH_SIZE,
                       SECTOR_SIZE, PAGE_SIZE, DEFAULT_WG) != 0)
        return RDB_ERR_FLASH;
    fault_init(&g_kv_fault, 0xA5A5A5A5u);
    sim_flash_set_fault_ctx(&g_kv_flash, &g_kv_fault);
    g_kv_part = (rdb_partition_t) {
        .name = "KVDB", .base_addr = 0, .total_size = KVDB_PART_SIZE,
        .sector_size = SECTOR_SIZE, .write_gran = DEFAULT_WG, .ops = &g_kv_ops
    };
    g_kv_db.part = &g_kv_part;
    g_kv_db.sectors = g_kv_meta;
    g_kv_db.sector_cnt = (uint8_t)KV_SECTOR_CNT;
    trace_event(&g_trace, "KVDB format+init (integration)");
    rdb_err_t ret = rdb_kvdb_format(&g_kv_db);
    if (ret != RDB_OK) return ret;
    ret = rdb_kvdb_init(&g_kv_db, &g_kv_part, g_kv_meta);
    if (ret == RDB_OK) trace_kvdb_snapshot(&g_trace, &g_kv_db);
    return ret;
}

static rdb_err_t ts_reset(void)
{
    if (sim_flash_init(&g_ts_flash, g_ts_flash_buf, FLASH_SIZE,
                       SECTOR_SIZE, PAGE_SIZE, DEFAULT_WG) != 0)
        return RDB_ERR_FLASH;
    fault_init(&g_ts_fault, 0x5A5A5A5Au);
    sim_flash_set_fault_ctx(&g_ts_flash, &g_ts_fault);
    g_ts_part = (rdb_partition_t) {
        .name = "TSDB", .base_addr = 0, .total_size = TSDB_PART_SIZE,
        .sector_size = SECTOR_SIZE, .write_gran = DEFAULT_WG, .ops = &g_ts_ops
    };
    g_ts_db.part = &g_ts_part;
    g_ts_db.erase_cnts = g_ts_ec;
    g_ts_db.sector_cnt = (uint8_t)TS_SECTOR_CNT;
    trace_event(&g_trace, "TSDB format+init (integration)");
    rdb_err_t ret = rdb_tsdb_format(&g_ts_db);
    if (ret != RDB_OK) return ret;
    ret = rdb_tsdb_init(&g_ts_db, &g_ts_part, g_ts_ec);
    if (ret == RDB_OK) trace_tsdb_snapshot(&g_trace, &g_ts_db);
    return ret;
}

/* ═══════════════════════════════════════════════════════════════════════════
 *  T-400: GC/Rotation cycle stress
 * ═══════════════════════════════════════════════════════════════════════════ */

#define CYCLE_GC_TARGET   200u
#define CYCLE_ROT_TARGET  200u
#define CYCLE_MAX_LOOPS   400000u

TEST_CASE(kv_gc_cycles_stress, "KVDB", "GC cycles >=200")
{
    (void)ctx;
    TEST_ASSERT_RDB_OK(kv_reset());

    char key[4] = { 'K', '0', '0', 0 };
    uint8_t val[64];
    memset(val, 0xA5, sizeof(val));

    uint32_t loops = 0;
    while (g_kv_db.stats.gc_runs < CYCLE_GC_TARGET && loops < CYCLE_MAX_LOOPS) {
        for (int i = 0; i < 50; i++) {
            key[1] = (char)('0' + (i / 10));
            key[2] = (char)('0' + (i % 10));
            TEST_ASSERT_RDB_OK(rdb_kvdb_set(&g_kv_db, key, val, (uint16_t)sizeof(val)));
        }
        loops++;
    }
    printf("KVDB GC runs: %u (loops=%u)\n", g_kv_db.stats.gc_runs, loops);
    TEST_ASSERT_GE(g_kv_db.stats.gc_runs, CYCLE_GC_TARGET);
    return 0;
}

TEST_CASE(ts_rotation_cycles_stress, "TSDB", "Rotation cycles >=200")
{
    (void)ctx;
    TEST_ASSERT_RDB_OK(ts_reset());

    uint8_t data[128];
    memset(data, 0x5A, sizeof(data));

    uint32_t loops = 0, time = 1;
    while (g_ts_db.stats.sector_rotations < CYCLE_ROT_TARGET && loops < CYCLE_MAX_LOOPS) {
        TEST_ASSERT_RDB_OK(rdb_tsdb_append(&g_ts_db, time++, data, (uint16_t)sizeof(data)));
        loops++;
    }
    printf("TSDB rotations: %u (loops=%u)\n", g_ts_db.stats.sector_rotations, loops);
    TEST_ASSERT_GE(g_ts_db.stats.sector_rotations, CYCLE_ROT_TARGET);
    return 0;
}

/* ═══════════════════════════════════════════════════════════════════════════
 *  T-401: mixed workload
 * ═══════════════════════════════════════════════════════════════════════════ */

#define MIX_KEY_POOL     64u
#define MIX_MAX_VAL      64u
#define MIX_TOTAL_OPS    10000u
#define MIX_QUERY_INTV   250u

static uint8_t  g_mix_kv_vals[MIX_KEY_POOL][MIX_MAX_VAL];
static uint16_t g_mix_kv_lens[MIX_KEY_POOL];
static uint8_t  g_mix_kv_present[MIX_KEY_POOL];
static uint32_t g_mix_prng = 0x13579BDFu;

static uint32_t mix_rand(void)
{
    g_mix_prng = (g_mix_prng * 1664525u) + 1013904223u;
    return g_mix_prng;
}

static rdb_err_t mix_reset(void)
{
    rdb_err_t ret = kv_reset();
    if (ret != RDB_OK) return ret;
    return ts_reset();
}

typedef struct { uint32_t count; } mix_ts_ctx_t;

static int mix_ts_cb(uint32_t ts, const void *data, uint16_t len, void *arg)
{
    (void)ts; (void)data; (void)len;
    ((mix_ts_ctx_t *)arg)->count++;
    return RDB_ITER_CONTINUE;
}

TEST_CASE(mixed_workload, "MIXED", "KVDB/TSDB mixed workload")
{
    (void)ctx;
    TEST_ASSERT_RDB_OK(mix_reset());
    memset(g_mix_kv_present, 0, sizeof(g_mix_kv_present));

    sim_dist_t kv_len_dist, ts_len_dist;
    sim_dist_init_uniform(&kv_len_dist, 0x11111111u, 1, MIX_MAX_VAL);
    sim_dist_init_uniform(&ts_len_dist, 0x22222222u, 1, MIX_MAX_VAL);

    uint32_t time = 1, kv_sets = 0, kv_gets = 0, kv_dels = 0;
    uint32_t ts_appends = 0, ts_queries = 0;

    for (uint32_t op = 1; op <= MIX_TOTAL_OPS; op++) {
        uint32_t roll = mix_rand() % 100u;
        uint32_t key_idx = mix_rand() % MIX_KEY_POOL;
        char key[8];
        snprintf(key, sizeof(key), "K%03u", (unsigned)key_idx);

        if (roll < 55u) {
            uint16_t vlen = (uint16_t)sim_dist_next(&kv_len_dist);
            for (uint16_t i = 0; i < vlen; i++)
                g_mix_kv_vals[key_idx][i] = (uint8_t)(i + (uint8_t)op);
            g_mix_kv_lens[key_idx] = vlen;
            g_mix_kv_present[key_idx] = 1;
            TEST_ASSERT_RDB_OK(rdb_kvdb_set(&g_kv_db, key, g_mix_kv_vals[key_idx], vlen));
            kv_sets++;
        } else if (roll < 75u) {
            uint8_t buf[MIX_MAX_VAL]; uint16_t out_len = 0;
            rdb_err_t ret = rdb_kvdb_get(&g_kv_db, key, buf, sizeof(buf), &out_len);
            if (g_mix_kv_present[key_idx]) {
                TEST_ASSERT_RDB_OK(ret);
                TEST_ASSERT_EQ(out_len, g_mix_kv_lens[key_idx]);
                TEST_ASSERT_MEM_EQ(buf, g_mix_kv_vals[key_idx], out_len);
            } else {
                TEST_ASSERT_RDB_ERR(ret, RDB_ERR_NOT_FOUND);
            }
            kv_gets++;
        } else if (roll < 85u) {
            rdb_err_t ret = rdb_kvdb_delete(&g_kv_db, key);
            if (g_mix_kv_present[key_idx]) {
                TEST_ASSERT_RDB_OK(ret);
                g_mix_kv_present[key_idx] = 0;
            } else {
                TEST_ASSERT_RDB_ERR(ret, RDB_ERR_NOT_FOUND);
            }
            kv_dels++;
        } else {
            uint16_t len = (uint16_t)sim_dist_next(&ts_len_dist);
            uint8_t data[MIX_MAX_VAL];
            for (uint16_t i = 0; i < len; i++)
                data[i] = (uint8_t)(0x55u + i + (uint8_t)op);
            TEST_ASSERT_RDB_OK(rdb_tsdb_append(&g_ts_db, time++, data, len));
            ts_appends++;
        }

        if ((op % MIX_QUERY_INTV) == 0u && ts_appends > 0u) {
            uint32_t from = (time > 200u) ? (time - 200u) : 1u;
            mix_ts_ctx_t qctx = { 0 };
            TEST_ASSERT_RDB_OK(rdb_tsdb_query(&g_ts_db, from, time - 1u, mix_ts_cb, &qctx));
            ts_queries++;
        }
    }

    printf("Mixed workload: set=%u get=%u del=%u ts_append=%u ts_query=%u\n",
           kv_sets, kv_gets, kv_dels, ts_appends, ts_queries);
    TEST_ASSERT_GT(kv_sets, 0u);
    TEST_ASSERT_GT(ts_appends, 0u);
    return 0;
}

/* ═══════════════════════════════════════════════════════════════════════════
 *  T-402/T-403: power-loss injection (KV + TS)
 * ═══════════════════════════════════════════════════════════════════════════ */

#define PL_ITERATIONS 50u

TEST_CASE(kv_power_loss_stress, "KVDB", "KV power-loss stress over 50 iterations")
{
    (void)ctx;
    uint8_t val[32];
    memset(val, 0x3Cu, sizeof(val));

    for (uint32_t i = 0; i < PL_ITERATIONS; i++) {
        TEST_ASSERT_RDB_OK(kv_reset());
        fault_reset_stats(&g_kv_fault);
        fault_quick_power_loss(&g_kv_fault, 1u, (i % 4u));
        rdb_err_t ret = rdb_kvdb_set(&g_kv_db, "KPL", val, (uint16_t)sizeof(val));
        TEST_ASSERT_NE(ret, RDB_OK);

        fault_init(&g_kv_fault, 0xA5A5A5A5u);
        TEST_ASSERT_RDB_OK(rdb_kvdb_init(&g_kv_db, &g_kv_part, g_kv_meta));
        TEST_ASSERT_RDB_OK(rdb_kvdb_set(&g_kv_db, "KOK", val, (uint16_t)sizeof(val)));

        uint8_t out[32]; uint16_t out_len = 0;
        TEST_ASSERT_RDB_OK(rdb_kvdb_get(&g_kv_db, "KOK", out, sizeof(out), &out_len));
        TEST_ASSERT_EQ(out_len, (uint16_t)sizeof(val));
        TEST_ASSERT_MEM_EQ(out, val, sizeof(val));
    }
    return 0;
}

TEST_CASE(ts_power_loss_stress, "TSDB", "TS power-loss stress over 50 iterations")
{
    (void)ctx;
    uint8_t data[16];
    memset(data, 0x7Bu, sizeof(data));

    for (uint32_t i = 0; i < PL_ITERATIONS; i++) {
        TEST_ASSERT_RDB_OK(ts_reset());
        fault_reset_stats(&g_ts_fault);
        fault_quick_power_loss(&g_ts_fault, 1u, (i % 4u));
        rdb_err_t ret = rdb_tsdb_append(&g_ts_db, 1u, data, (uint16_t)sizeof(data));
        TEST_ASSERT_NE(ret, RDB_OK);

        fault_init(&g_ts_fault, 0x5A5A5A5Au);
        TEST_ASSERT_RDB_OK(rdb_tsdb_init(&g_ts_db, &g_ts_part, g_ts_ec));
        TEST_ASSERT_RDB_OK(rdb_tsdb_append(&g_ts_db, 2u, data, (uint16_t)sizeof(data)));

        uint32_t out_time = 0; uint8_t out_buf[16]; uint16_t out_len = 0;
        TEST_ASSERT_RDB_OK(rdb_tsdb_get_latest(&g_ts_db, &out_time, out_buf, sizeof(out_buf), &out_len));
        TEST_ASSERT_EQ(out_time, 2u);
        TEST_ASSERT_EQ(out_len, (uint16_t)sizeof(data));
        TEST_ASSERT_MEM_EQ(out_buf, data, sizeof(data));
    }
    return 0;
}

/* ═══════════════════════════════════════════════════════════════════════════
 *  T-404: wear-leveling heatmap
 * ═══════════════════════════════════════════════════════════════════════════ */

#define WEAR_KV_GC_TGT   80u
#define WEAR_TS_ROT_TGT  80u
#define WEAR_MAX_LOOPS   200000u

static void print_kv_heatmap(void)
{
    printf("\n── KV Wear Heatmap ──────────────────────────────\n");
    printf("%-6s %11s %6s %13s\n", "sector", "erase_count", "status", "garbage_bytes");
    for (uint32_t i = 0; i < KV_SECTOR_CNT; i++)
        printf("%-6u %11u %6u %13u\n", i, g_kv_meta[i].erase_cnt,
               g_kv_meta[i].status, g_kv_meta[i].garbage_bytes);
}

static void print_ts_heatmap(void)
{
    printf("\n── TS Wear Heatmap ──────────────────────────────\n");
    printf("%-6s %11s\n", "sector", "erase_count");
    for (uint32_t i = 0; i < TS_SECTOR_CNT; i++)
        printf("%-6u %11u\n", i, g_ts_ec[i]);
}

TEST_CASE(wear_heatmap, "WEAR", "Wear-leveling heatmap")
{
    (void)ctx;
    TEST_ASSERT_RDB_OK(kv_reset());
    TEST_ASSERT_RDB_OK(ts_reset());

    char key[4] = { 'W', '0', '0', 0 };
    uint8_t val[64];
    memset(val, 0x9Au, sizeof(val));

    uint32_t kv_loops = 0;
    while (g_kv_db.stats.gc_runs < WEAR_KV_GC_TGT && kv_loops < WEAR_MAX_LOOPS) {
        for (int i = 0; i < 30; i++) {
            key[1] = (char)('0' + (i / 10));
            key[2] = (char)('0' + (i % 10));
            TEST_ASSERT_RDB_OK(rdb_kvdb_set(&g_kv_db, key, val, (uint16_t)sizeof(val)));
        }
        kv_loops++;
    }

    uint8_t data[128];
    memset(data, 0x6Cu, sizeof(data));

    uint32_t ts_loops = 0, time = 1;
    while (g_ts_db.stats.sector_rotations < WEAR_TS_ROT_TGT && ts_loops < WEAR_MAX_LOOPS) {
        TEST_ASSERT_RDB_OK(rdb_tsdb_append(&g_ts_db, time++, data, (uint16_t)sizeof(data)));
        ts_loops++;
    }

    uint32_t kv_min = 0xFFFFFFFFu, kv_max = 0, kv_sum = 0;
    for (uint32_t i = 0; i < KV_SECTOR_CNT; i++) {
        uint32_t ec = g_kv_meta[i].erase_cnt;
        if (ec < kv_min) kv_min = ec;
        if (ec > kv_max) kv_max = ec;
        kv_sum += ec;
    }

    uint32_t ts_min = 0xFFFFFFFFu, ts_max = 0, ts_sum = 0;
    for (uint32_t i = 0; i < TS_SECTOR_CNT; i++) {
        uint32_t ec = g_ts_ec[i];
        if (ec < ts_min) ts_min = ec;
        if (ec > ts_max) ts_max = ec;
        ts_sum += ec;
    }

    printf("KV wear: min=%u max=%u avg=%u\n", kv_min, kv_max, kv_sum / KV_SECTOR_CNT);
    printf("TS wear: min=%u max=%u avg=%u\n", ts_min, ts_max, ts_sum / TS_SECTOR_CNT);
    TEST_ASSERT_GE(kv_max, kv_min);
    TEST_ASSERT_GE(ts_max, ts_min);

    print_kv_heatmap();
    print_ts_heatmap();
    return 0;
}

/* ═══════════════════════════════════════════════════════════════════════════
 *  Entry point
 * ═══════════════════════════════════════════════════════════════════════════ */

int main(void)
{
    test_config_t config = {
        .log_file = fopen(test_make_log_path("integration"), "w"),
        .verbose = 1, .stop_on_fail = 0, .filter = NULL
    };
    test_framework_init(&config);

    trace_init(&g_trace, config.log_file, config.verbose);
    sim_flash_set_trace(&g_kv_flash, &g_trace);
    sim_flash_set_trace(&g_ts_flash, &g_trace);
    trace_event(&g_trace, "=== Integration Test Suite Start ===");

    test_suite_t *s = test_get_default_suite();
    test_register_case(s, &test_case_kv_gc_cycles_stress);
    test_register_case(s, &test_case_ts_rotation_cycles_stress);
    test_register_case(s, &test_case_mixed_workload);
    test_register_case(s, &test_case_kv_power_loss_stress);
    test_register_case(s, &test_case_ts_power_loss_stress);
    test_register_case(s, &test_case_wear_heatmap);

    test_run_all(NULL);

    trace_event(&g_trace, "=== Integration Test Suite End ===\n");
    trace_kvdb_stats(&g_trace, &g_kv_db);
    trace_tsdb_stats(&g_trace, &g_ts_db);

    test_print_report();
    if (config.log_file) fclose(config.log_file);

    test_stats_t stats; test_get_stats(&stats);
    return (stats.failed_cases == 0) ? 0 : 1;
}
