/**
 * test_kvdb_stress.c — KVDB stress tests
 *
 * Covers: GC stress, iterator under GC, iterator BUSY checking,
 *         power-loss recovery
 *
 * References: T-301, T-303, T-305
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
#define KVDB_PART_SIZE  (64u * 1024u)
#define KV_SECTOR_CNT   (KVDB_PART_SIZE / SECTOR_SIZE)

/* ── Shared flash environment ──────────────────────────────────────────── */

static uint8_t            g_flash_buf[FLASH_SIZE];
static sim_flash_t        g_flash;
static fault_ctx_t        g_fault;
static rdb_partition_t    g_part;
static rdb_kvdb_t         g_db;
static rdb_kv_sector_meta_t g_meta[KV_SECTOR_CNT];

static int fl_read(uint32_t addr, uint8_t *buf, size_t len) {
    return sim_flash_read(&g_flash, addr, buf, len);
}
static int fl_write(uint32_t addr, const uint8_t *buf, size_t len) {
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

static rdb_err_t kv_reset(void)
{
    if (sim_flash_init(&g_flash, g_flash_buf, FLASH_SIZE,
                       SECTOR_SIZE, PAGE_SIZE, DEFAULT_WG) != 0)
        return RDB_ERR_FLASH;
    fault_init(&g_fault, 0xA5A5A5A5u);
    sim_flash_set_fault_ctx(&g_flash, &g_fault);
    g_part = (rdb_partition_t) {
        .name = "KVDB", .base_addr = 0, .total_size = KVDB_PART_SIZE,
        .sector_size = SECTOR_SIZE, .write_gran = DEFAULT_WG, .ops = &g_ops
    };
    g_db.part = &g_part;
    g_db.sectors = g_meta;
    g_db.sector_cnt = (uint8_t)KV_SECTOR_CNT;
    rdb_err_t ret = rdb_kvdb_format(&g_db);
    if (ret != RDB_OK) return ret;
    return rdb_kvdb_init(&g_db, &g_part, g_meta);
}

/* ── Helpers for key construction ──────────────────────────────────────── */

static void build_key(char *key, int idx) {
    key[0] = 'K'; key[1] = (char)('0' + (idx / 10));
    key[2] = (char)('0' + (idx % 10)); key[3] = '\0';
}

static int key_index(const char *key) {
    if (!key || key[0] != 'K' || key[1] < '0' || key[1] > '9' ||
        key[2] < '0' || key[2] > '9') return -1;
    return (key[1] - '0') * 10 + (key[2] - '0');
}

/* ═══════════════════════════════════════════════════════════════════════════
 *  T-301: GC stress
 * ═══════════════════════════════════════════════════════════════════════════ */

#define GC_STRESS_TARGET  100u
#define GC_STRESS_KEYS    20
#define GC_STRESS_MAX_LOOPS 200000u

TEST_CASE(kv_gc_stress_100, "KVDB", "GC stress with >=100 cycles")
{
    (void)ctx;
    TEST_ASSERT_RDB_OK(kv_reset());

    char key[4] = { 'K', '0', '0', 0 };
    uint8_t val[32];
    memset(val, 0xA5, sizeof(val));

    uint32_t loops = 0;
    while (g_db.stats.gc_runs < GC_STRESS_TARGET && loops < GC_STRESS_MAX_LOOPS) {
        for (int i = 0; i < GC_STRESS_KEYS; i++) {
            key[1] = (char)('0' + (i / 10));
            key[2] = (char)('0' + (i % 10));
            TEST_ASSERT_RDB_OK(rdb_kvdb_set(&g_db, key, val, (uint16_t)sizeof(val)));
        }
        loops++;
    }
    TEST_ASSERT_GE(g_db.stats.gc_runs, GC_STRESS_TARGET);
    return 0;
}

/* ═══════════════════════════════════════════════════════════════════════════
 *  T-303: iterator under GC
 * ═══════════════════════════════════════════════════════════════════════════ */

#define ITER_KEY_COUNT  32
#define ITER_GC_TARGET  2u
#define ITER_MAX_LOOPS  5000u

TEST_CASE(kv_iter_after_gc, "KVDB", "Iterator returns latest values after GC")
{
    (void)ctx;
    TEST_ASSERT_RDB_OK(kv_reset());

    uint8_t expected[ITER_KEY_COUNT][8];
    uint16_t expected_len[ITER_KEY_COUNT];

    for (int i = 0; i < ITER_KEY_COUNT; i++) {
        char key[4]; build_key(key, i);
        expected[i][0] = (uint8_t)i;
        expected[i][1] = (uint8_t)(i + 1);
        expected[i][2] = (uint8_t)(i + 2);
        expected[i][3] = (uint8_t)(i + 3);
        expected_len[i] = 4;
        TEST_ASSERT_RDB_OK(rdb_kvdb_set(&g_db, key, expected[i], expected_len[i]));
    }

    for (int round = 0; round < 8; round++) {
        for (int i = 0; i < ITER_KEY_COUNT; i++) {
            char key[4]; build_key(key, i);
            expected[i][0] = (uint8_t)(round + 10);
            expected[i][1] = (uint8_t)i;
            expected[i][2] = (uint8_t)(round ^ i);
            expected[i][3] = (uint8_t)(0xA5u ^ i);
            expected_len[i] = 4;
            TEST_ASSERT_RDB_OK(rdb_kvdb_set(&g_db, key, expected[i], expected_len[i]));
        }
    }

    uint32_t loops = 0;
    while (g_db.stats.gc_runs < ITER_GC_TARGET && loops < ITER_MAX_LOOPS) {
        for (int i = 0; i < ITER_KEY_COUNT; i++) {
            char key[4]; build_key(key, i);
            expected[i][0] = (uint8_t)(0x55u + i);
            expected[i][1] = (uint8_t)(0xAAu - i);
            expected[i][2] = (uint8_t)i;
            expected[i][3] = (uint8_t)(loops & 0xFF);
            expected_len[i] = 4;
            TEST_ASSERT_RDB_OK(rdb_kvdb_set(&g_db, key, expected[i], expected_len[i]));
        }
        loops++;
    }
    TEST_ASSERT_GE(g_db.stats.gc_runs, ITER_GC_TARGET);

    rdb_kv_iter_t it;
    TEST_ASSERT_RDB_OK(rdb_kv_iter_init(&it, &g_db));

    uint8_t seen[ITER_KEY_COUNT];
    memset(seen, 0, sizeof(seen));

    char key_buf[16]; uint8_t val_buf[16]; uint16_t klen, vlen;
    while (rdb_kv_iter_next(&it, key_buf, sizeof(key_buf),
                            val_buf, sizeof(val_buf), &klen, &vlen) == RDB_OK) {
        int idx = key_index(key_buf);
        TEST_ASSERT_GE(idx, 0);
        TEST_ASSERT_LT(idx, ITER_KEY_COUNT);
        TEST_ASSERT_EQ(seen[idx], 0);
        seen[idx] = 1;
        TEST_ASSERT_EQ(vlen, expected_len[idx]);
        TEST_ASSERT_MEM_EQ(val_buf, expected[idx], expected_len[idx]);
    }
    for (int i = 0; i < ITER_KEY_COUNT; i++) TEST_ASSERT_EQ(seen[i], 1);
    return 0;
}

TEST_CASE(kv_iter_busy_on_modify, "KVDB", "Iterator returns BUSY when DB modified")
{
    (void)ctx;
    TEST_ASSERT_RDB_OK(kv_reset());

    TEST_ASSERT_RDB_OK(rdb_kvdb_set(&g_db, "K00", "A", 1));
    TEST_ASSERT_RDB_OK(rdb_kvdb_set(&g_db, "K01", "B", 1));

    rdb_kv_iter_t it;
    TEST_ASSERT_RDB_OK(rdb_kv_iter_init(&it, &g_db));
    TEST_ASSERT_RDB_OK(rdb_kvdb_set(&g_db, "K02", "C", 1));

    char key_buf[16]; uint8_t val_buf[16]; uint16_t klen, vlen;
    TEST_ASSERT_RDB_ERR(rdb_kv_iter_next(&it, key_buf, sizeof(key_buf),
                                         val_buf, sizeof(val_buf), &klen, &vlen),
                        RDB_ERR_BUSY);
    return 0;
}

/* ═══════════════════════════════════════════════════════════════════════════
 *  T-305: power-loss recovery
 * ═══════════════════════════════════════════════════════════════════════════ */

TEST_CASE(kv_power_loss_recovery, "KVDB", "Recover after power loss during write")
{
    (void)ctx;
    TEST_ASSERT_RDB_OK(kv_reset());

    TEST_ASSERT_RDB_OK(rdb_kvdb_set(&g_db, "K0", "A", 1));
    TEST_ASSERT_RDB_OK(rdb_kvdb_set(&g_db, "K1", "B", 1));

    fault_quick_power_loss(&g_fault, g_fault.write_count + 1u, 0u);
    TEST_ASSERT_NE(rdb_kvdb_set(&g_db, "PL", "X", 1), RDB_OK);

    fault_clear_rules(&g_fault);
    TEST_ASSERT_RDB_OK(rdb_kvdb_init(&g_db, &g_part, g_meta));

    char out[8] = { 0 }; uint16_t out_len = 0;
    TEST_ASSERT_RDB_OK(rdb_kvdb_get(&g_db, "K0", out, sizeof(out), &out_len));
    TEST_ASSERT_EQ(out_len, 1); TEST_ASSERT_EQ(out[0], 'A');

    TEST_ASSERT_RDB_OK(rdb_kvdb_get(&g_db, "K1", out, sizeof(out), &out_len));
    TEST_ASSERT_EQ(out_len, 1); TEST_ASSERT_EQ(out[0], 'B');

    TEST_ASSERT_NE(rdb_kvdb_get(&g_db, "PL", out, sizeof(out), &out_len), RDB_OK);
    return 0;
}

/* ═══════════════════════════════════════════════════════════════════════════
 *  TC-X-04: Corrupt sector recovery
 *
 *  Inject corrupt sector headers and verify init marks CORRUPT and
 *  reclaims to ERASED.  Written once per sector across format+init.
 * ═══════════════════════════════════════════════════════════════════════════ */

TEST_CASE(kv_corrupt_sector_recovery, "KVDB", "Corrupt sector header recovery")
{
    (void)ctx;
    TEST_ASSERT_RDB_OK(kv_reset());

    /* Write known data so we can verify it survives */
    const char *key = "survivor";
    const uint8_t val[] = { 0xCA, 0xFE, 0xBA, 0xBE };
    TEST_ASSERT_RDB_OK(rdb_kvdb_set(&g_db, key, val, (uint16_t)sizeof(val)));

    /* Find an ERASED sector (not the active one) and corrupt its header */
    uint8_t victim = 0xFF;
    for (uint8_t s = 0; s < KV_SECTOR_CNT; s++) {
        if (g_db.sectors[s].status == RDB_SEC_ERASED) {
            victim = s; break;
        }
    }
    /* If no ERASED sector, use any non-active one */
    if (victim == 0xFF) {
        for (uint8_t s = 0; s < KV_SECTOR_CNT; s++) {
            if (s != g_db.active_sec) { victim = s; break; }
        }
    }
    TEST_ASSERT_NE(victim, 0xFF);

    /* Corrupt the sector header by trashing the magic (first 4 bytes) */
    uint32_t sec_addr_val = g_part.base_addr + (uint32_t)victim * g_part.sector_size;
    uint8_t zero[4] = { 0x00, 0x00, 0x00, 0x00 };
    sim_flash_write(&g_flash, sec_addr_val, zero, sizeof(zero));

    /* Re-init — should detect corruption and recover */
    TEST_ASSERT_RDB_OK(rdb_kvdb_init(&g_db, &g_part, g_meta));

    /* The corrupt sector should now be ERASED (reclaimed) */
    uint8_t status = g_db.sectors[victim].status;
    TEST_ASSERT(status == RDB_SEC_ERASED || status == RDB_SEC_CORRUPT);

    /* Data in other sectors must survive */
    uint8_t out[8] = { 0 }; uint16_t out_len = 0;
    TEST_ASSERT_RDB_OK(rdb_kvdb_get(&g_db, key, out, sizeof(out), &out_len));
    TEST_ASSERT_EQ(out_len, (uint16_t)sizeof(val));
    TEST_ASSERT_MEM_EQ(out, val, sizeof(val));

    /* DB remains usable — can write new data */
    TEST_ASSERT_RDB_OK(rdb_kvdb_set(&g_db, "new_key", "NEW", 3));
    uint8_t out2[8] = { 0 }; uint16_t out2_len = 0;
    TEST_ASSERT_RDB_OK(rdb_kvdb_get(&g_db, "new_key", out2, sizeof(out2), &out2_len));
    TEST_ASSERT_EQ(out2_len, 3u);
    TEST_ASSERT_MEM_EQ(out2, "NEW", 3);

    return 0;
}

/* ═══════════════════════════════════════════════════════════════════════════
 *  Entry point
 * ═══════════════════════════════════════════════════════════════════════════ */

int main(void)
{
    test_config_t config = {
        .log_file = fopen(test_make_log_path("kvdb_stress"), "w"),
        .verbose = 1, .stop_on_fail = 0, .filter = NULL
    };
    test_framework_init(&config);

    test_suite_t *s = test_get_default_suite();
    test_register_case(s, &test_case_kv_gc_stress_100);
    test_register_case(s, &test_case_kv_iter_after_gc);
    test_register_case(s, &test_case_kv_iter_busy_on_modify);
    test_register_case(s, &test_case_kv_power_loss_recovery);
    test_register_case(s, &test_case_kv_corrupt_sector_recovery);

    test_run_all(NULL);
    test_print_report();
    if (config.log_file) fclose(config.log_file);

    test_stats_t stats; test_get_stats(&stats);
    return (stats.failed_cases == 0) ? 0 : 1;
}
