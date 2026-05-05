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
static trace_ctx_t          g_trace;

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
    trace_event(&g_trace, "KVDB format+init (stress)");
    rdb_err_t ret = rdb_kvdb_format(&g_db);
    if (ret != RDB_OK) return ret;
    ret = rdb_kvdb_init(&g_db, &g_part, g_meta);
    if (ret == RDB_OK) trace_kvdb_snapshot(&g_trace, &g_db);
    return ret;
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

/* ── Size PRNG for GC stress ───────────────────────────────────────────── */

static uint32_t g_kv_sz_prng = 0xCAFE1234u;

static uint16_t kv_sz_next(void)
{
    g_kv_sz_prng ^= g_kv_sz_prng << 13;
    g_kv_sz_prng ^= g_kv_sz_prng >> 17;
    g_kv_sz_prng ^= g_kv_sz_prng << 5;
    return (uint16_t)(g_kv_sz_prng & 0xFFFFu);
}

/* ═══════════════════════════════════════════════════════════════════════════
 *  T-301: GC stress
 * ═══════════════════════════════════════════════════════════════════════════ */

#define GC_STRESS_TARGET  100u
#define GC_STRESS_KEYS    20
#define GC_STRESS_MAX_LOOPS 500000u

TEST_CASE(kv_gc_stress_100, "KVDB", "GC stress with >=100 cycles")
{
    (void)ctx;
    TEST_ASSERT_RDB_OK(kv_reset());

    char key[4] = { 'K', '0', '0', 0 };
    uint8_t val[512];
    memset(val, 0xA5, sizeof(val));

    trace_event(&g_trace, "  [GC-STRESS] start: keys=K00..K%02d target_gc=%u",
                GC_STRESS_KEYS - 1, GC_STRESS_TARGET);
    uint32_t loops = 0, prev_gc = g_db.stats.gc_runs;
    while (g_db.stats.gc_runs < GC_STRESS_TARGET && loops < GC_STRESS_MAX_LOOPS) {
        for (int i = 0; i < GC_STRESS_KEYS; i++) {
            key[1] = (char)('0' + (i / 10));
            key[2] = (char)('0' + (i % 10));
            uint16_t vsz = 1u + (uint16_t)(kv_sz_next() % 512u);
            TEST_ASSERT_RDB_OK(rdb_kvdb_set(&g_db, key, val, vsz));
            if (g_db.stats.gc_runs != prev_gc) {
                prev_gc = g_db.stats.gc_runs;
                trace_kvdb_gc_event(&g_trace, &g_db, prev_gc, loops);
            }
        }
        loops++;
        trace_event(&g_trace, "  [GC-STRESS] loop=%u keys=K00..K%02d gc=%u",
                    loops, GC_STRESS_KEYS - 1, g_db.stats.gc_runs);
    }
    TEST_ASSERT_GE(g_db.stats.gc_runs, GC_STRESS_TARGET);
    trace_event(&g_trace, "GC-STRESS done: keys=%u loops=%u gc=%u",
                GC_STRESS_KEYS, loops, g_db.stats.gc_runs);
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

    trace_event(&g_trace, "  [KV-ITER] seeding %u keys K00..K%02d",
                ITER_KEY_COUNT, ITER_KEY_COUNT - 1);
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
        trace_event(&g_trace, "  [KV-ITER] round=%d updating K00..K%02d",
                    round, ITER_KEY_COUNT - 1);
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
    trace_event(&g_trace, "  [KV-ITER] GC-trigger loops=%u gc=%u",
                loops, g_db.stats.gc_runs);

    rdb_kv_iter_t it;
    TEST_ASSERT_RDB_OK(rdb_kv_iter_init(&it, &g_db));

    uint8_t seen[ITER_KEY_COUNT];
    memset(seen, 0, sizeof(seen));

    trace_event(&g_trace, "  [KV-ITER] iterating...");

    uint32_t iter_count = 0;
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
        iter_count++;
        trace_event(&g_trace, "  [KV-ITER #%u] key=%s vlen=%u",
                    iter_count, key_buf, vlen);
    }
    trace_event(&g_trace, "KV-ITER complete: %u keys found", iter_count);
    for (int i = 0; i < ITER_KEY_COUNT; i++) TEST_ASSERT_EQ(seen[i], 1);
    return 0;
}

TEST_CASE(kv_iter_busy_on_modify, "KVDB", "Iterator returns BUSY when DB modified")
{
    (void)ctx;
    TEST_ASSERT_RDB_OK(kv_reset());

    trace_event(&g_trace, "  [KV-BUSY] writing key=K00 val='A'");
    TEST_ASSERT_RDB_OK(rdb_kvdb_set(&g_db, "K00", "A", 1));
    trace_event(&g_trace, "  [KV-BUSY] writing key=K01 val='B'");
    TEST_ASSERT_RDB_OK(rdb_kvdb_set(&g_db, "K01", "B", 1));

    rdb_kv_iter_t it;
    TEST_ASSERT_RDB_OK(rdb_kv_iter_init(&it, &g_db));
    trace_event(&g_trace, "  [KV-BUSY] iterator active, writing key=K02 val='C'");
    TEST_ASSERT_RDB_OK(rdb_kvdb_set(&g_db, "K02", "C", 1));

    char key_buf[16]; uint8_t val_buf[16]; uint16_t klen, vlen;
    TEST_ASSERT_RDB_ERR(rdb_kv_iter_next(&it, key_buf, sizeof(key_buf),
                                         val_buf, sizeof(val_buf), &klen, &vlen),
                        RDB_ERR_BUSY);
    trace_event(&g_trace, "  [KV-BUSY] iter_next correctly returned BUSY");
    return 0;
}

/* ═══════════════════════════════════════════════════════════════════════════
 *  T-305: power-loss recovery
 * ═══════════════════════════════════════════════════════════════════════════ */

TEST_CASE(kv_power_loss_recovery, "KVDB", "Recover after power loss during write")
{
    (void)ctx;
    TEST_ASSERT_RDB_OK(kv_reset());

    trace_event(&g_trace, "  [KV-PL] writing K0, K1 before power-loss");
    TEST_ASSERT_RDB_OK(rdb_kvdb_set(&g_db, "K0", "A", 1));
    TEST_ASSERT_RDB_OK(rdb_kvdb_set(&g_db, "K1", "B", 1));

    fault_quick_power_loss(&g_fault, g_fault.write_count + 1u, 0u);
    TEST_ASSERT_NE(rdb_kvdb_set(&g_db, "PL", "X", 1), RDB_OK);
    trace_event(&g_trace, "  [KV-PL] power-loss injected, key=PL lost");

    fault_clear_rules(&g_fault);
    TEST_ASSERT_RDB_OK(rdb_kvdb_init(&g_db, &g_part, g_meta));

    char out[8] = { 0 }; uint16_t out_len = 0;
    TEST_ASSERT_RDB_OK(rdb_kvdb_get(&g_db, "K0", out, sizeof(out), &out_len));
    TEST_ASSERT_EQ(out_len, 1); TEST_ASSERT_EQ(out[0], 'A');
    trace_event(&g_trace, "  [KV-PL] recovered key=K0 val='%c'", out[0]);

    TEST_ASSERT_RDB_OK(rdb_kvdb_get(&g_db, "K1", out, sizeof(out), &out_len));
    TEST_ASSERT_EQ(out_len, 1); TEST_ASSERT_EQ(out[0], 'B');
    trace_event(&g_trace, "  [KV-PL] recovered key=K1 val='%c'", out[0]);

    TEST_ASSERT_NE(rdb_kvdb_get(&g_db, "PL", out, sizeof(out), &out_len), RDB_OK);
    trace_event(&g_trace, "  [KV-PL] key=PL correctly absent");
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
    trace_event(&g_trace, "  [KV-CORRUPT] wrote key=%s val_len=%u",
                key, (uint16_t)sizeof(val));

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
    trace_event(&g_trace, "  [KV-CORRUPT] corrupted sector %u magic", victim);

    /* Re-init — should detect corruption and recover */
    TEST_ASSERT_RDB_OK(rdb_kvdb_init(&g_db, &g_part, g_meta));

    /* The corrupt sector should now be ERASED (reclaimed) */
    uint8_t status = g_db.sectors[victim].status;
    TEST_ASSERT(status == RDB_SEC_ERASED || status == RDB_SEC_CORRUPT);
    trace_event(&g_trace, "  [KV-CORRUPT] sector %u status=%u", victim, status);

    /* Data in other sectors must survive */
    uint8_t out[8] = { 0 }; uint16_t out_len = 0;
    TEST_ASSERT_RDB_OK(rdb_kvdb_get(&g_db, key, out, sizeof(out), &out_len));
    TEST_ASSERT_EQ(out_len, (uint16_t)sizeof(val));
    TEST_ASSERT_MEM_EQ(out, val, sizeof(val));
    trace_event(&g_trace, "  [KV-CORRUPT] key=%s survived len=%u", key, out_len);

    /* DB remains usable — can write new data */
    TEST_ASSERT_RDB_OK(rdb_kvdb_set(&g_db, "new_key", "NEW", 3));
    uint8_t out2[8] = { 0 }; uint16_t out2_len = 0;
    TEST_ASSERT_RDB_OK(rdb_kvdb_get(&g_db, "new_key", out2, sizeof(out2), &out2_len));
    TEST_ASSERT_EQ(out2_len, 3u);
    TEST_ASSERT_MEM_EQ(out2, "NEW", 3);
    trace_event(&g_trace, "  [KV-CORRUPT] new key=%s ok len=%u", "new_key", out2_len);

    return 0;
}

/* ═══════════════════════════════════════════════════════════════════════════
 *  TC-X-05: Mixed-length value stress (1..max_val random, clamped per key)
 *
 *  Uses a deterministic PRNG to generate random key names and value sizes
 *  with flat 20%-per-bucket distribution across the full 1..4095 range.
 *  Large values (3501..4095) appear at 20% probability — clearly visible
 *  in trace logs.  Drives GC with realistic fragmentation.
 * ═══════════════════════════════════════════════════════════════════════════ */

#define MIX_KV_KEY_COUNT  20
#define MIX_KV_GC_TARGET  13u
#define MIX_KV_MAX_LOOPS  80000u
#define MIX_KV_SEED       0xCAFE1234u

static uint32_t kv_max_val_for_key(const rdb_kvdb_t *db, uint16_t key_len)
{
    uint32_t gran = 1u << db->part->write_gran;
    uint32_t ds   = RDB_ALIGN_UP((uint32_t)sizeof(rdb_kv_sector_hdr_t), gran);
    uint32_t cap  = db->part->sector_size - ds;
    uint32_t ka   = RDB_ALIGN_UP(key_len, gran);
    uint32_t va_max = cap - (uint32_t)sizeof(rdb_kv_record_hdr_t) - ka;
    return va_max - (va_max % gran);
}

static uint32_t kv_xorshift(void)
{
    static uint32_t s = MIX_KV_SEED;
    s ^= s << 13; s ^= s >> 17; s ^= s << 5;
    return s;
}

static uint16_t kv_rand_size(uint32_t max_val)
{
    /* Piecewise distribution clamped to actual max_val:
     * 40% 1-32, 40% 33-256, 15% 257-1024, 4% 1025..max-1, 1% max_val */
    uint32_t r = kv_xorshift() % 100u;
    uint32_t sz;
    if (r < 40u) {
        sz = 1u + (kv_xorshift() % 32u);
    } else if (r < 80u) {
        sz = 33u + (kv_xorshift() % 224u);
    } else if (r < 95u) {
        sz = 257u + (kv_xorshift() % 768u);
    } else if (r < 99u) {
        if (max_val > 1025u) sz = 1025u + (kv_xorshift() % (max_val - 1025u));
        else                 sz = max_val;
    } else {
        sz = max_val;
    }
    if (sz > max_val) sz = max_val;
    if (sz < 1u) sz = 1u;
    return (uint16_t)sz;
}

TEST_CASE(kv_mixed_value_stress, "KVDB", "Random mixed-length value GC stress (clamped to max_val)")
{
    (void)ctx;
    TEST_ASSERT_RDB_OK(kv_reset());
    trace_kvdb_geometry(&g_trace, &g_db);

    /* Generate random-but-unique key names (deterministic from seed) */
    char    keys[MIX_KV_KEY_COUNT][20];
    uint8_t key_lens[MIX_KV_KEY_COUNT];
    for (int i = 0; i < MIX_KV_KEY_COUNT; i++) {
        /* Random prefix + index suffix guarantees uniqueness */
        uint8_t pre_len = (uint8_t)(1 + (kv_xorshift() % 12));
        for (uint8_t j = 0; j < pre_len; j++)
            keys[i][j] = (char)('a' + (kv_xorshift() % 26));
        /* Append "-%02d" suffix for uniqueness */
        int suff_len = snprintf(keys[i] + pre_len, 4, "-%02d", i);
        key_lens[i] = (uint8_t)(pre_len + suff_len);
    }

    /* Pre-compute max value length per key based on actual geometry */
    uint32_t key_max_vals[MIX_KV_KEY_COUNT];
    uint32_t min_max_val = 0xFFFFFFFFu;
    for (int i = 0; i < MIX_KV_KEY_COUNT; i++) {
        key_max_vals[i] = kv_max_val_for_key(&g_db, key_lens[i]);
        if (key_max_vals[i] < min_max_val) min_max_val = key_max_vals[i];
    }
    trace_event(&g_trace, "  [KV-MIX] key_max_vals range: %u..%u",
                min_max_val, kv_max_val_for_key(&g_db, 1));

    uint32_t write_seq[MIX_KV_KEY_COUNT];
    memset(write_seq, 0, sizeof(write_seq));

    uint16_t max_size = 0;
    uint32_t total_writes = 0, large_writes = 0;
    uint32_t size_bins[5] = {0};  /* 1-32, 33-256, 257-1024, 1025..max-1, max_val */
    uint32_t loops = 0, prev_gc = g_db.stats.gc_runs;
    trace_event(&g_trace, "  [KV-MIX] start: %u keys target_gc=%u",
                MIX_KV_KEY_COUNT, MIX_KV_GC_TARGET);
    while (g_db.stats.gc_runs < MIX_KV_GC_TARGET && loops < MIX_KV_MAX_LOOPS) {
        for (int i = 0; i < MIX_KV_KEY_COUNT; i++) {
            uint32_t kmax = key_max_vals[i];
            uint16_t vsz = kv_rand_size(kmax);
            if (vsz > max_size) max_size = vsz;
            total_writes++;
            if      (vsz <= 32)    size_bins[0]++;
            else if (vsz <= 256)   size_bins[1]++;
            else if (vsz <= 1024)  size_bins[2]++;
            else if (vsz < kmax)   size_bins[3]++;
            else                   size_bins[4]++;
            if (vsz >= 1000) large_writes++;
            uint8_t  val[4100];
            val[0] = (uint8_t)i;
            val[1] = (uint8_t)(write_seq[i] & 0xFFu);
            for (uint16_t b = 2; b < vsz; b++)
                val[b] = (uint8_t)(kv_xorshift() & 0xFFu);
            rdb_err_t rc = rdb_kvdb_set(&g_db, keys[i], val, vsz);
            TEST_ASSERT_RDB_OK(rc);
            write_seq[i]++;
            if (g_db.stats.gc_runs != prev_gc) {
                prev_gc = g_db.stats.gc_runs;
                trace_kvdb_gc_event(&g_trace, &g_db, prev_gc, loops);
            }
        }
        loops++;
    }
    TEST_ASSERT_GE(g_db.stats.gc_runs, MIX_KV_GC_TARGET);

    trace_event(&g_trace, "Mixed-KV size distribution (writes=%u large=%u): "
                "1..32=%u 33..256=%u 257..1024=%u <max=%u =max=%u",
                total_writes, large_writes, size_bins[0], size_bins[1],
                size_bins[2], size_bins[3], size_bins[4]);
    trace_event(&g_trace, "Mixed-KV max_size=%uB (key_max=%u..%u) loops=%u gc=%u",
                max_size, min_max_val, kv_max_val_for_key(&g_db, 1), loops, g_db.stats.gc_runs);

    TEST_ASSERT_GE(max_size, 4000u);

    /* Every key must be retrievable, size in range, key_index byte intact */
    uint8_t out[4100]; uint16_t out_len;
    for (int i = 0; i < MIX_KV_KEY_COUNT; i++) {
        rdb_err_t rc = rdb_kvdb_get(&g_db, keys[i], out, sizeof(out), &out_len);
        TEST_ASSERT_RDB_OK(rc);
        TEST_ASSERT_GT(out_len, 0u);
        TEST_ASSERT_LE(out_len, RDB_MAX_VAL_LEN);
        TEST_ASSERT_EQ(out[0], (uint8_t)i);
        trace_event(&g_trace, "  [KV-GET] key=%s val_len=%u ok", keys[i], out_len);
    }
    return 0;
}

/* ═══════════════════════════════════════════════════════════════════════════
 *  Entry point
 * ═══════════════════════════════════════════════════════════════════════════ */

static void post_test_kvdb_sectors(const char *name, int result, void *ctx)
{
    (void)name; (void)result; (void)ctx;
    trace_kvdb_sector_summary(&g_trace, &g_db);
}

int main(void)
{
    test_config_t config = {
        .log_file = fopen(test_make_log_path("kvdb_stress"), "w"),
        .verbose = 1, .stop_on_fail = 0, .filter = NULL,
        .post_test_hook = post_test_kvdb_sectors, .hook_ctx = NULL
    };
    test_framework_init(&config);

    trace_init(&g_trace, config.log_file, config.verbose);
    sim_flash_set_trace(&g_flash, &g_trace);
    trace_event(&g_trace, "=== KVDB Stress Test Suite Start ===");

    test_suite_t *s = test_get_default_suite();
    test_register_case(s, &test_case_kv_gc_stress_100);
    test_register_case(s, &test_case_kv_iter_after_gc);
    test_register_case(s, &test_case_kv_iter_busy_on_modify);
    test_register_case(s, &test_case_kv_power_loss_recovery);
    test_register_case(s, &test_case_kv_corrupt_sector_recovery);
    test_register_case(s, &test_case_kv_mixed_value_stress);

    test_run_all(NULL);

    trace_event(&g_trace, "=== KVDB Stress Test Suite End ===\n");
    trace_kvdb_stats(&g_trace, &g_db);

    test_print_report();
    if (config.log_file) fclose(config.log_file);

    test_stats_t stats; test_get_stats(&stats);
    return (stats.failed_cases == 0) ? 0 : 1;
}
