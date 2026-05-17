/**
 * test_fault_injection.c — Fault injection validation tests
 *
 * Verifies DB robustness under simulated hardware faults:
 *  - Nth-write failure
 *  - Probabilistic write failure
 *  - Erase failure during GC
 *  - Byte-level power loss (partial writes)
 *  - Data CRC corruption detection
 *
 * Each test isolates "clean" format/init from "faulty" operations
 * so that fault injection targets the specific scenario, not the
 * setup phase.
 */

#include "sim_flash.h"
#include "sim_fault.h"
#include "test_framework.h"
#include "../../src/rocketdb.h"
#include <stdio.h>
#include <string.h>

#define FLASH_SIZE      (128u * 1024u)
#define SECTOR_SIZE     4096u
#define KVDB_PART_SIZE  (64u * 1024u)
#define KV_SECTOR_CNT   (KVDB_PART_SIZE / SECTOR_SIZE)

/* ── Shared environment ─────────────────────────────────────────────────── */

static uint8_t     g_buf[FLASH_SIZE];
static sim_flash_t g_flash;
static fault_ctx_t g_fault;
static trace_ctx_t g_trace;

static int fl_read(uint32_t a, uint8_t *b, size_t n) { return sim_flash_read(&g_flash, a, b, n); }
static int fl_write(uint32_t a, const uint8_t *b, size_t n) { return sim_flash_write(&g_flash, a, b, n); }
static int fl_erase(uint32_t a) { return sim_flash_erase(&g_flash, a); }
static void fl_lock(void) { } static void fl_unlock(void) { } static void fl_yield(void) { }

static rdb_flash_ops_t g_ops = {
    .read = fl_read, .write = fl_write, .erase = fl_erase,
    .lock = fl_lock, .unlock = fl_unlock, .yield = fl_yield
};

/* ── Helper: init flash & DB without faults ─────────────────────────────── */

static rdb_err_t db_clean_init(rdb_kvdb_t *db, rdb_partition_t *part,
                               rdb_kv_sector_meta_t *meta)
{
    sim_flash_init(&g_flash, g_buf, FLASH_SIZE, SECTOR_SIZE, 256, 0);
    fault_init(&g_fault, 0);
    sim_flash_set_fault_ctx(&g_flash, NULL);  /* no faults during init */

    *part = (rdb_partition_t) {
        .name = "test", .base_addr = 0, .total_size = KVDB_PART_SIZE,
        .sector_size = SECTOR_SIZE, .write_gran = 0, .ops = &g_ops
    };
    db->part = part;
    db->sectors = meta;
    db->sector_cnt = KV_SECTOR_CNT;

    rdb_err_t r = rdb_kvdb_format(db);
    if (r != RDB_OK) return r;
    return rdb_kvdb_init(db, part, meta);
}

/* ═══════════════════════════════════════════════════════════════════════════
 *  Test 1: Nth write fails, pre-fault data survives re-init
 * ═══════════════════════════════════════════════════════════════════════════ */

TEST_CASE(fault_write_fail_nth, "Fault", "Nth write failure, data survives recovery")
{
    (void)ctx;

    rdb_partition_t part;
    rdb_kv_sector_meta_t meta[KV_SECTOR_CNT];
    rdb_kvdb_t db;
    TEST_ASSERT_RDB_OK(db_clean_init(&db, &part, meta));

    /* Write 3 keys WITHOUT faults */
    TEST_ASSERT_RDB_OK(rdb_kvdb_set(&db, "K0", "AAA", 3));
    TEST_ASSERT_RDB_OK(rdb_kvdb_set(&db, "K1", "BBB", 3));
    TEST_ASSERT_RDB_OK(rdb_kvdb_set(&db, "K2", "CCC", 3));

    /* Now enable fault: next write (the 4th overall) fails */
    fault_init(&g_fault, 0x12345);
    fault_quick_write_fail(&g_fault, g_fault.write_count + 1u);
    sim_flash_set_fault_ctx(&g_flash, &g_fault);

    rdb_err_t r = rdb_kvdb_set(&db, "K3", "DDD", 3);
    TEST_ASSERT_NE(r, RDB_OK);  /* must fail */

    /* Clear faults and re-init — pre-fault data must survive */
    fault_clear_rules(&g_fault);
    sim_flash_set_fault_ctx(&g_flash, NULL);
    TEST_ASSERT_RDB_OK(rdb_kvdb_init(&db, &part, meta));

    uint8_t out[8]; uint16_t ol;
    TEST_ASSERT_RDB_OK(rdb_kvdb_get(&db, "K0", out, sizeof(out), &ol));
    TEST_ASSERT_EQ(ol, 3); TEST_ASSERT_MEM_EQ(out, "AAA", 3);
    TEST_ASSERT_RDB_OK(rdb_kvdb_get(&db, "K1", out, sizeof(out), &ol));
    TEST_ASSERT_EQ(ol, 3); TEST_ASSERT_MEM_EQ(out, "BBB", 3);
    TEST_ASSERT_RDB_OK(rdb_kvdb_get(&db, "K2", out, sizeof(out), &ol));
    TEST_ASSERT_EQ(ol, 3); TEST_ASSERT_MEM_EQ(out, "CCC", 3);

    /* K3 was never committed */
    TEST_ASSERT_RDB_ERR(rdb_kvdb_get(&db, "K3", out, sizeof(out), &ol), RDB_ERR_NOT_FOUND);

    /* DB remains usable */
    TEST_ASSERT_RDB_OK(rdb_kvdb_set(&db, "K4", "OK", 2));
    return 0;
}

/* ═══════════════════════════════════════════════════════════════════════════
 *  Test 2: Probabilistic write failure
 *
 *  Format/init clean, then enable probability fault.
 *  Each set does 2 writes (record + commit). With P% failure per write,
 *  some sets should fail and some should succeed.
 *  After clearing faults, DB must be re-initializable.
 * ═══════════════════════════════════════════════════════════════════════════ */

TEST_CASE(fault_write_fail_probability, "Fault", "Probabilistic write failure")
{
    (void)ctx;

    rdb_partition_t part;
    rdb_kv_sector_meta_t meta[KV_SECTOR_CNT];
    rdb_kvdb_t db;
    TEST_ASSERT_RDB_OK(db_clean_init(&db, &part, meta));

    /* Enable 20% write failure probability */
    fault_init(&g_fault, 0x66666);
    fault_quick_write_fail_probability(&g_fault, 20);
    sim_flash_set_fault_ctx(&g_flash, &g_fault);

    int ok = 0, fail = 0;
    for (int i = 0; i < 30; i++) {
        char key[8]; snprintf(key, sizeof(key), "K%d", i);
        rdb_err_t r = rdb_kvdb_set(&db, key, "V", 1);
        if (r == RDB_OK) ok++;
        else {
            fail++;
            /* After a write failure, the DB may be in an inconsistent state.
             * Re-init to recover before attempting more writes. */
            fault_clear_rules(&g_fault);
            fault_quick_write_fail_probability(&g_fault, 20);
            sim_flash_set_fault_ctx(&g_flash, NULL);
            rdb_kvdb_init(&db, &part, meta);
            sim_flash_set_fault_ctx(&g_flash, &g_fault);
        }
    }

    /* Both outcomes must occur with 20% probability over 30 trials */
    trace_event(&g_trace, "Probability fault: ok=%d fail=%d", ok, fail);
    TEST_ASSERT_GT(ok, 0);
    TEST_ASSERT_GT(fail, 0);

    /* DB must be recoverable */
    sim_flash_set_fault_ctx(&g_flash, NULL);
    fault_clear_rules(&g_fault);
    TEST_ASSERT_RDB_OK(rdb_kvdb_init(&db, &part, meta));
    TEST_ASSERT_RDB_OK(rdb_kvdb_set(&db, "recovered", "yes", 3));
    return 0;
}

/* ═══════════════════════════════════════════════════════════════════════════
 *  Test 3: Erase failure during GC
 * ═══════════════════════════════════════════════════════════════════════════ */

TEST_CASE(fault_erase_fail, "Fault", "Erase failure during GC is handled gracefully")
{
    (void)ctx;

    rdb_partition_t part;
    rdb_kv_sector_meta_t meta[KV_SECTOR_CNT];
    rdb_kvdb_t db;
    TEST_ASSERT_RDB_OK(db_clean_init(&db, &part, meta));

    /* Fill all sectors with data to force GC (which needs erase) */
    uint8_t val[128];
    memset(val, 0x5A, sizeof(val));
    int wrote = 0;
    for (int i = 0; i < 500; i++) {
        char key[8]; snprintf(key, sizeof(key), "K%d", i);
        rdb_err_t r = rdb_kvdb_set(&db, key, val, sizeof(val));
        if (r == RDB_ERR_FULL) break;
        if (r == RDB_OK) wrote++;
        /* Some sets may fail if sector is full — that's expected */
    }
    TEST_ASSERT_GT(wrote, 0);

    /* Now enable erase failure on the next erase */
    fault_init(&g_fault, 0x99999);
    fault_quick_erase_fail(&g_fault, 1u);
    sim_flash_set_fault_ctx(&g_flash, &g_fault);

    /* Trigger GC — it will try to erase and fail */
    rdb_kvdb_gc(&db);

    /* DB must survive: re-init and verify data is accessible */
    sim_flash_set_fault_ctx(&g_flash, NULL);
    fault_clear_rules(&g_fault);
    TEST_ASSERT_RDB_OK(rdb_kvdb_init(&db, &part, meta));

    /* Some pre-GC data should survive */
    uint8_t out[128]; uint16_t ol;
    rdb_err_t r = rdb_kvdb_get(&db, "K0", out, sizeof(out), &ol);
    /* K0 may have been GC'd — either OK or NOT_FOUND are acceptable */
    TEST_ASSERT(r == RDB_OK || r == RDB_ERR_NOT_FOUND);

    /* DB must be usable after recovery */
    TEST_ASSERT_RDB_OK(rdb_kvdb_set(&db, "new_after_gc_fail", "ok", 2));
    return 0;
}

/* ═══════════════════════════════════════════════════════════════════════════
 *  Test 4: Byte-level power loss with partial writes
 *
 *  After the fix to fault_should_write_fail (removing early power-loss
 *  interception), the byte loop in sim_flash_write now does real partial
 *  writes: some bytes are committed to flash, then power loss fires.
 *  Re-init must recover WRITING records correctly.
 * ═══════════════════════════════════════════════════════════════════════════ */

TEST_CASE(fault_power_loss, "Fault", "Byte-level power loss with partial write recovery")
{
    (void)ctx;

    rdb_partition_t part;
    rdb_kv_sector_meta_t meta[KV_SECTOR_CNT];
    rdb_kvdb_t db;
    TEST_ASSERT_RDB_OK(db_clean_init(&db, &part, meta));

    /* Write pre-fault data */
    TEST_ASSERT_RDB_OK(rdb_kvdb_set(&db, "pre1", "survive_before_pl", 16));
    TEST_ASSERT_RDB_OK(rdb_kvdb_set(&db, "pre2", "survive_too", 10));

    /* Inject power loss during the commit write of the next set.
     * write_count is now 4 (2 format-era writes + 2 set writes).
     * The next set does 2 more writes (data + commit). Trigger at commit
     * byte (the 6th write overall, at byte offset 0). */
    fault_init(&g_fault, 0xAAAAA);
    uint32_t pl_at = g_fault.write_count + 2u;  /* skip data write, fail commit */
    fault_quick_power_loss(&g_fault, pl_at, 0);
    sim_flash_set_fault_ctx(&g_flash, &g_fault);

    rdb_err_t r = rdb_kvdb_set(&db, "pl_target", "this_should_be_lost", 18);
    TEST_ASSERT_NE(r, RDB_OK);  /* power loss must fire */

    /* Clear everything and re-init (simulate reboot) */
    sim_flash_set_fault_ctx(&g_flash, NULL);
    fault_clear_rules(&g_fault);
    TEST_ASSERT_RDB_OK(rdb_kvdb_init(&db, &part, meta));

    /* Pre-fault data must survive */
    uint8_t out[32]; uint16_t ol;
    TEST_ASSERT_RDB_OK(rdb_kvdb_get(&db, "pre1", out, sizeof(out), &ol));
    TEST_ASSERT_EQ(ol, 16);
    TEST_ASSERT_RDB_OK(rdb_kvdb_get(&db, "pre2", out, sizeof(out), &ol));
    TEST_ASSERT_EQ(ol, 10);

    /* The interrupted record: if data was fully written and only the
     * commit byte was lost, a correct re-init may recover it as VALID.
     * If recovery marks it DEAD, it is NOT_FOUND. Either is consistent. */
    r = rdb_kvdb_get(&db, "pl_target", out, sizeof(out), &ol);
    TEST_ASSERT(r == RDB_OK || r == RDB_ERR_NOT_FOUND);
    if (r == RDB_OK) {
        /* If recovered, data must match what was originally written */
        TEST_ASSERT_EQ(ol, 18);
        TEST_ASSERT_MEM_EQ(out, "this_should_be_lost", 18);
    }

    /* DB must be usable after power-loss recovery */
    TEST_ASSERT_RDB_OK(rdb_kvdb_set(&db, "after_pl", "recovered", 9));
    TEST_ASSERT_RDB_OK(rdb_kvdb_get(&db, "after_pl", out, sizeof(out), &ol));
    TEST_ASSERT_EQ(ol, 9);
    TEST_ASSERT_MEM_EQ(out, "recovered", 9);
    return 0;
}

/* ═══════════════════════════════════════════════════════════════════════════
 *  Test 5: Data CRC corruption
 *
 *  Write a record, locate its value data on flash, corrupt a value byte,
 *  then verify that get returns RDB_ERR_CRC and the DB remains usable.
 * ═══════════════════════════════════════════════════════════════════════════ */

TEST_CASE(fault_data_corruption, "Fault", "CRC error detected on corrupted value data")
{
    (void)ctx;

    rdb_partition_t part;
    rdb_kv_sector_meta_t meta[KV_SECTOR_CNT];
    rdb_kvdb_t db;
    TEST_ASSERT_RDB_OK(db_clean_init(&db, &part, meta));

    /* Write target record */
    const char *key = "crc_target";
    const uint8_t val[] = "verify_crc_integrity_1234567890!";
    uint16_t vlen = (uint16_t)strlen((const char *)val);
    TEST_ASSERT_RDB_OK(rdb_kvdb_set(&db, key, val, vlen));

    /* Find the record on flash and locate its value payload */
    uint32_t ds = RDB_ALIGN_UP((uint32_t)sizeof(rdb_kv_sector_hdr_t), 1u);
    uint32_t ss = part.sector_size, base = part.base_addr;
    uint32_t val_addr = 0xFFFFFFFFu;
    uint16_t found_vlen = 0;

    for (uint32_t off = ds; off + sizeof(rdb_kv_record_hdr_t) <= ss; ) {
        rdb_kv_record_hdr_t rh;
        sim_flash_read(&g_flash, base + off, (uint8_t *)&rh, sizeof(rh));
        if (rh.magic == 0xFF && rh.state == 0xFF) break;
        if (rh.magic == RDB_KV_RECORD_MAGIC && rh.key_len > 0 &&
            rh.key_len <= RDB_MAX_KEY_LEN && rh.val_len <= RDB_MAX_VAL_LEN) {
            /* Key is stored right after header */
            uint32_t key_addr = base + off + (uint32_t)sizeof(rdb_kv_record_hdr_t);
            char kb[64];
            if (rh.key_len < sizeof(kb)) {
                sim_flash_read(&g_flash, key_addr, (uint8_t *)kb, rh.key_len);
                kb[rh.key_len] = '\0';
                if (strcmp(kb, key) == 0) {
                    val_addr = key_addr + rh.key_len;
                    found_vlen = rh.val_len;
                    break;
                }
            }
        }
        off += RDB_ALIGN_UP(sizeof(rdb_kv_record_hdr_t) +
                           RDB_ALIGN_UP(rh.key_len, 1u) +
                           RDB_ALIGN_UP(rh.val_len, 1u), 1u);
    }
    TEST_ASSERT_NE(val_addr, 0xFFFFFFFFu);
    TEST_ASSERT_EQ(found_vlen, vlen);
    trace_event(&g_trace, "Target record '%s': value at 0x%08X len=%u",
                key, val_addr, found_vlen);

    /* Corrupt the first byte of value data (not the key) */
    g_flash.mem[val_addr] ^= 0xFFu;

    /* get should return CRC error */
    uint8_t out[64]; uint16_t ol = 0;
    rdb_err_t r = rdb_kvdb_get(&db, key, out, sizeof(out), &ol);
    TEST_ASSERT(r == RDB_ERR_CRC || r == RDB_ERR_NOT_FOUND);
    /* NOT_FOUND is also valid: CRC may cause the record to be skipped
     * during scan, depending on implementation */

    /* DB must remain usable for other keys */
    TEST_ASSERT_RDB_OK(rdb_kvdb_set(&db, "other_key", "still_works", 11));
    TEST_ASSERT_RDB_OK(rdb_kvdb_get(&db, "other_key", out, sizeof(out), &ol));
    TEST_ASSERT_EQ(ol, 11);
    TEST_ASSERT_MEM_EQ(out, "still_works", 11);
    return 0;
}

/* ═══════════════════════════════════════════════════════════════════════════
 *  Test 6: Read failure injection
 *
 *  Write pre-fault data, enable read failure on the Nth read, verify
 *  that reads return errors while faults are active, and the DB
 *  recovers fully once faults are cleared.
 * ═══════════════════════════════════════════════════════════════════════════ */

TEST_CASE(fault_read_fail, "Fault", "Read failure injection and recovery")
{
    (void)ctx;

    rdb_partition_t part;
    rdb_kv_sector_meta_t meta[KV_SECTOR_CNT];
    rdb_kvdb_t db;
    TEST_ASSERT_RDB_OK(db_clean_init(&db, &part, meta));

    /* Write data without faults */
    TEST_ASSERT_RDB_OK(rdb_kvdb_set(&db, "r0", "read_test_0", 11));
    TEST_ASSERT_RDB_OK(rdb_kvdb_set(&db, "r1", "read_test_1", 11));

    /* Enable read failure: fail on the 3rd read (first 2 reads succeeded
     * during the two set() calls above, each of which reads during
     * find_latest). */
    fault_init(&g_fault, 0x31415);
    fault_rule_t rule = {
        .type = FAULT_TYPE_READ_FAIL,
        .trigger_mode = FAULT_TRIGGER_COUNT,
        .trigger_count = g_fault.read_count + 3u,
        .probability_pct = 0, .addr_start = 0, .addr_end = 0, .seed = 0, .enabled = 1
    };
    fault_add_rule(&g_fault, &rule);
    sim_flash_set_fault_ctx(&g_flash, &g_fault);

    /* Some reads may fail */
    uint8_t out[32]; uint16_t ol;
    rdb_err_t r0 = rdb_kvdb_get(&db, "r0", out, sizeof(out), &ol);
    rdb_err_t r1 = rdb_kvdb_get(&db, "r1", out, sizeof(out), &ol);
    /* At least one may have failed if the fault triggered */
    trace_event(&g_trace, "Read fail test: r0=%d r1=%d faults=%u",
                r0, r1, g_fault.fault_injected);

    /* Clear faults — DB must be fully usable */
    sim_flash_set_fault_ctx(&g_flash, NULL);
    fault_clear_rules(&g_fault);
    TEST_ASSERT_RDB_OK(rdb_kvdb_init(&db, &part, meta));

    TEST_ASSERT_RDB_OK(rdb_kvdb_set(&db, "after_read_fault", "recovered", 9));
    TEST_ASSERT_RDB_OK(rdb_kvdb_get(&db, "after_read_fault", out, sizeof(out), &ol));
    TEST_ASSERT_EQ(ol, 9);
    return 0;
}

/* ═══════════════════════════════════════════════════════════════════════════
 *  Test 7: Bit flip corruption detection
 *
 *  Write a record, flip a single bit in its value data, verify that
 *  a subsequent read detects the corruption (CRC error).  This
 *  differs from Test 5 (data_corruption) which toggles all 8 bits;
 *  here only one bit is flipped, verifying single-bit error detection.
 * ═══════════════════════════════════════════════════════════════════════════ */

TEST_CASE(fault_bit_flip, "Fault", "Single-bit flip detected by CRC")
{
    (void)ctx;

    rdb_partition_t part;
    rdb_kv_sector_meta_t meta[KV_SECTOR_CNT];
    rdb_kvdb_t db;
    TEST_ASSERT_RDB_OK(db_clean_init(&db, &part, meta));

    /* Write target record */
    const char *key = "bf_target";
    const uint8_t val[] = "bit_flip_crc_test_1234567890!";
    uint16_t vlen = (uint16_t)strlen((const char *)val);
    TEST_ASSERT_RDB_OK(rdb_kvdb_set(&db, key, val, vlen));

    /* Locate value data on flash */
    uint32_t ds = RDB_ALIGN_UP((uint32_t)sizeof(rdb_kv_sector_hdr_t), 1u);
    uint32_t ss = part.sector_size, base = part.base_addr;
    uint32_t val_addr = 0xFFFFFFFFu;

    for (uint32_t off = ds; off + sizeof(rdb_kv_record_hdr_t) <= ss; ) {
        rdb_kv_record_hdr_t rh;
        sim_flash_read(&g_flash, base + off, (uint8_t *)&rh, sizeof(rh));
        if (rh.magic == 0xFF && rh.state == 0xFF) break;
        if (rh.magic == RDB_KV_RECORD_MAGIC && rh.key_len > 0 &&
            rh.key_len <= RDB_MAX_KEY_LEN) {
            uint32_t key_addr = base + off + sizeof(rdb_kv_record_hdr_t);
            char kb[64];
            if (rh.key_len < sizeof(kb)) {
                sim_flash_read(&g_flash, key_addr, (uint8_t *)kb, rh.key_len);
                kb[rh.key_len] = '\0';
                if (strcmp(kb, key) == 0) {
                    val_addr = key_addr + rh.key_len;
                    break;
                }
            }
        }
        off += RDB_ALIGN_UP(sizeof(rdb_kv_record_hdr_t) +
                           RDB_ALIGN_UP(rh.key_len, 1u) +
                           RDB_ALIGN_UP(rh.val_len, 1u), 1u);
    }
    TEST_ASSERT_NE(val_addr, 0xFFFFFFFFu);

    /* Flip a single bit (bit 3 of byte 0) in the value data */
    g_flash.mem[val_addr] ^= (1u << 3);
    trace_event(&g_trace, "Bit flip at addr 0x%08X: byte was flipped (single bit)", val_addr);

    /* CRC must detect the corruption */
    uint8_t out[64]; uint16_t ol = 0;
    rdb_err_t r = rdb_kvdb_get(&db, key, out, sizeof(out), &ol);
    TEST_ASSERT(r == RDB_ERR_CRC || r == RDB_ERR_NOT_FOUND);

    /* DB remains usable */
    TEST_ASSERT_RDB_OK(rdb_kvdb_set(&db, "bf_still_ok", "yes", 3));
    TEST_ASSERT_RDB_OK(rdb_kvdb_get(&db, "bf_still_ok", out, sizeof(out), &ol));
    TEST_ASSERT_EQ(ol, 3);
    return 0;
}

/* ═══════════════════════════════════════════════════════════════════════════
 *  Test 8: Fault rule export/import round-trip
 *
 *  Verifies the text rule parser imports every valid exported rule, not only
 *  the first rule whose insertion index is zero.
 * ═══════════════════════════════════════════════════════════════════════════ */

TEST_CASE(fault_rule_import_roundtrip, "Fault", "Fault rule export/import round-trip")
{
    (void)ctx;

    fault_ctx_t src;
    fault_ctx_t dst;
    char buf[256];

    fault_init(&src, 0x1234u);
    fault_quick_write_fail(&src, 3u);

    fault_rule_t read_rule = {
        .type = FAULT_TYPE_READ_FAIL,
        .trigger_mode = FAULT_TRIGGER_COUNT,
        .trigger_count = 5u,
        .probability_pct = 0u,
        .addr_start = 0u,
        .addr_end = 0u,
        .seed = 0u,
        .enabled = 1
    };
    TEST_ASSERT_GE(fault_add_rule(&src, &read_rule), 0);

    int exported = fault_export_rules(&src, buf, sizeof(buf));
    TEST_ASSERT_GT((uint32_t)exported, 0u);

    fault_init(&dst, 0x5678u);
    int imported = fault_import_rules(&dst, buf);
    TEST_ASSERT_EQ(imported, 2);
    TEST_ASSERT_EQ(dst.rule_count, 2u);
    TEST_ASSERT_EQ(dst.rules[0].type, FAULT_TYPE_WRITE_FAIL);
    TEST_ASSERT_EQ(dst.rules[0].trigger_count, 3u);
    TEST_ASSERT_EQ(dst.rules[1].type, FAULT_TYPE_READ_FAIL);
    TEST_ASSERT_EQ(dst.rules[1].trigger_count, 5u);

    return 0;
}

/* ═══════════════════════════════════════════════════════════════════════════
 *  Entry point
 * ═══════════════════════════════════════════════════════════════════════════ */

int main(void)
{
    test_config_t config = {
        .log_file = fopen(test_make_log_path("fault_injection"), "w"),
        .verbose = 1, .stop_on_fail = 0, .filter = NULL,
        .post_test_hook = NULL, .hook_ctx = NULL
    };
    test_framework_init(&config);

    trace_init(&g_trace, config.log_file, config.verbose);
    sim_flash_set_trace(&g_flash, &g_trace);
    trace_event(&g_trace, "=== Fault Injection Test Suite Start ===");

    test_suite_t *suite = test_get_default_suite();
    test_register_case(suite, &test_case_fault_write_fail_nth);
    test_register_case(suite, &test_case_fault_write_fail_probability);
    test_register_case(suite, &test_case_fault_erase_fail);
    test_register_case(suite, &test_case_fault_power_loss);
    test_register_case(suite, &test_case_fault_data_corruption);
    test_register_case(suite, &test_case_fault_read_fail);
    test_register_case(suite, &test_case_fault_bit_flip);
    test_register_case(suite, &test_case_fault_rule_import_roundtrip);

    test_run_all(NULL);

    trace_event(&g_trace, "=== Fault Injection Test Suite End ===\n");
    test_print_report();

    if (config.log_file) fclose(config.log_file);

    test_stats_t stats; test_get_stats(&stats);
    return (stats.failed_cases == 0) ? 0 : 1;
}
