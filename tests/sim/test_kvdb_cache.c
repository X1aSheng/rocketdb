/**
 * test_kvdb_cache.c — KVDB key cache + TSDB safety fix tests
 *
 * Covers: KVDB key cache (controlled by RDB_KV_CACHE_SIZE macro),
 *         GC batch migration, TSDB mark_dead, commit-failure head_off
 *         advancement, recount accuracy.
 *
 * References: Phase 1.1 (TSDB mark_dead), Phase 1.2 (TSDB head_off),
 *             Phase 2.1 (KVDB key cache), Phase 2.3 (TSDB recount).
 *
 * Uses 64 KVDB sectors and 32 TSDB sectors for stress coverage.
 */

#include "sim_flash.h"
#include "sim_fault.h"
#include "sim_trace.h"
#include "test_framework.h"
#include "../../src/rocketdb.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ── Large-sector KVDB config (64 sectors) ────────────────────────────── */

#define KV_FLASH_SIZE      (512u * 1024u)
#define KV_SECTOR_SIZE     4096u
#define KV_PAGE_SIZE       256u
#define KVDB_PART_SIZE     (256u * 1024u)  /* 64 sectors x 4 KB */
#define KV_SECTOR_CNT      (KVDB_PART_SIZE / KV_SECTOR_SIZE)
#define KV_WG              0u

/* ── Large-sector TSDB config (32 sectors) ────────────────────────────── */

#define TS_FLASH_SIZE      (256u * 1024u)
#define TS_SECTOR_SIZE     4096u
#define TS_PAGE_SIZE       256u
#define TSDB_PART_SIZE     (128u * 1024u)  /* 32 sectors x 4 KB */
#define TS_SECTOR_CNT      (TSDB_PART_SIZE / TS_SECTOR_SIZE)
#define TS_WG              0u

/* ── Shared trace context ─────────────────────────────────────────────── */

static trace_ctx_t g_trace;

/* ── KVDB shared state ─────────────────────────────────────────────────── */

static uint8_t              g_kv_flash[KV_FLASH_SIZE];
static sim_flash_t          g_kv_sim;
static rdb_partition_t      g_kv_part;
static rdb_kvdb_t           g_kv_db;
/* Meta buffer sized for sector metadata + bloom filter bitmaps */
#define META_BUF_SZ (KV_SECTOR_CNT * (sizeof(rdb_kv_sector_meta_t) + RDB_BLOOM_BYTES))
static uint8_t               g_kv_meta_buf[META_BUF_SZ];
static rdb_kv_sector_meta_t *g_kv_meta = (rdb_kv_sector_meta_t *)g_kv_meta_buf;

static int kv_read(void *ctx, uint32_t addr, uint8_t *buf, size_t len) {
    (void)ctx; return sim_flash_read(&g_kv_sim, addr, buf, len);
}
static int kv_write(void *ctx, uint32_t addr, const uint8_t *buf, size_t len) {
    (void)ctx; return sim_flash_write(&g_kv_sim, addr, buf, len);
}
static int kv_erase(void *ctx, uint32_t addr) {
    (void)ctx; return sim_flash_erase(&g_kv_sim, addr);
}
static void kv_lock(void *ctx) { (void)ctx; }
static void kv_unlock(void *ctx) { (void)ctx; }
static void kv_yield(void *ctx) { (void)ctx; }

static rdb_flash_ops_t g_kv_ops = {
    .read = kv_read, .write = kv_write, .erase = kv_erase,
    .lock = kv_lock, .unlock = kv_unlock, .yield = kv_yield
};

static int kv_setup(void) {
    if (sim_flash_init(&g_kv_sim, g_kv_flash, KV_FLASH_SIZE,
                       KV_SECTOR_SIZE, KV_PAGE_SIZE, KV_WG) != 0)
        return -1;
    sim_flash_set_trace(&g_kv_sim, &g_trace);
    g_kv_part = (rdb_partition_t) {
        .name = "KVDB", .base_addr = 0, .total_size = KVDB_PART_SIZE,
        .sector_size = KV_SECTOR_SIZE, .write_gran = KV_WG, .ops = &g_kv_ops
    };
    memset(&g_kv_db, 0, sizeof(g_kv_db));
    g_kv_db.part = &g_kv_part;
    g_kv_db.sectors = g_kv_meta;
    g_kv_db.sector_cnt = KV_SECTOR_CNT;

    trace_event(&g_trace, "KVDB format+init (64 sectors, %u KB)",
                (unsigned)(KVDB_PART_SIZE / 1024u));
    rdb_err_t rc = rdb_kvdb_format(&g_kv_db);
    if (rc != RDB_OK) return -1;
    rc = rdb_kvdb_init(&g_kv_db, &g_kv_part, g_kv_meta);
    if (rc == RDB_OK) {
        trace_kvdb_snapshot(&g_trace, &g_kv_db);
#if RDB_KV_CACHE_SIZE > 0
        trace_event(&g_trace, "  [CACHE] %u slots (%u bytes) enabled",
                    (unsigned)RDB_KV_CACHE_SIZE,
                    (unsigned)sizeof(rdb_kv_cache_t));
#else
        trace_event(&g_trace, "  [CACHE] disabled (RDB_KV_CACHE_SIZE=0)");
#endif
    }
    return (rc == RDB_OK) ? 0 : -1;
}

/* ── TSDB shared state ─────────────────────────────────────────────────── */

static uint8_t          g_ts_flash[TS_FLASH_SIZE];
static sim_flash_t      g_ts_sim;
static rdb_partition_t  g_ts_part;
static rdb_tsdb_t       g_ts_db;
static uint32_t         g_ts_ec[TS_SECTOR_CNT];

static int ts_read(void *ctx, uint32_t addr, uint8_t *buf, size_t len) {
    (void)ctx; return sim_flash_read(&g_ts_sim, addr, buf, len);
}
static int ts_write(void *ctx, uint32_t addr, const uint8_t *buf, size_t len) {
    (void)ctx; return sim_flash_write(&g_ts_sim, addr, buf, len);
}
static int ts_erase(void *ctx, uint32_t addr) {
    (void)ctx; return sim_flash_erase(&g_ts_sim, addr);
}
static void ts_lock(void *ctx) { (void)ctx; }
static void ts_unlock(void *ctx) { (void)ctx; }
static void ts_yield(void *ctx) { (void)ctx; }

static rdb_flash_ops_t g_ts_ops = {
    .read = ts_read, .write = ts_write, .erase = ts_erase,
    .lock = ts_lock, .unlock = ts_unlock, .yield = ts_yield
};

static int ts_setup(void) {
    if (sim_flash_init(&g_ts_sim, g_ts_flash, TS_FLASH_SIZE,
                       TS_SECTOR_SIZE, TS_PAGE_SIZE, TS_WG) != 0)
        return -1;
    sim_flash_set_trace(&g_ts_sim, &g_trace);
    g_ts_part = (rdb_partition_t) {
        .name = "TSDB", .base_addr = 0, .total_size = TSDB_PART_SIZE,
        .sector_size = TS_SECTOR_SIZE, .write_gran = TS_WG, .ops = &g_ts_ops
    };
    memset(&g_ts_db, 0, sizeof(g_ts_db));
    memset(g_ts_ec, 0, sizeof(g_ts_ec));
    g_ts_db.part = &g_ts_part;
    g_ts_db.erase_cnts = g_ts_ec;
    g_ts_db.sector_cnt = TS_SECTOR_CNT;

    trace_event(&g_trace, "TSDB format+init (32 sectors, %u KB)",
                (unsigned)(TSDB_PART_SIZE / 1024u));
    rdb_err_t rc = rdb_tsdb_format(&g_ts_db);
    if (rc != RDB_OK) return -1;
    rc = rdb_tsdb_init(&g_ts_db, &g_ts_part, g_ts_ec);
    if (rc == RDB_OK) {
        trace_tsdb_snapshot(&g_trace, &g_ts_db);
        trace_tsdb_sector_summary(&g_trace, &g_ts_db);
    }
    return (rc == RDB_OK) ? 0 : -1;
}

/* ── Trace wrappers for KVDB/TSDB operations ───────────────────────────── */

static rdb_err_t trace_kv_set(const char *key, const void *val, uint16_t vlen) {
    trace_event(&g_trace, "  [KV-WRITE] key=%s vsz=%u", key, (unsigned)vlen);
    return rdb_kvdb_set(&g_kv_db, key, val, vlen);
}

static rdb_err_t trace_kv_get(const char *key, void *buf,
                              uint16_t buf_len, uint16_t *out_len) {
    trace_event(&g_trace, "  [KV-READ]  key=%s", key);
    return rdb_kvdb_get(&g_kv_db, key, buf, buf_len, out_len);
}

static rdb_err_t trace_kv_del(const char *key) {
    trace_event(&g_trace, "  [KV-DEL]   key=%s", key);
    return rdb_kvdb_delete(&g_kv_db, key);
}

static rdb_err_t trace_ts_append(uint32_t time, const void *data, uint16_t len) {
    trace_event(&g_trace, "  [TS-APPEND] time=%u dlen=%u", (unsigned)time, (unsigned)len);
    return rdb_tsdb_append(&g_ts_db, time, data, len);
}

/* ── Cache state tracing ──────────────────────────────────────────────── */

#if RDB_KV_CACHE_SIZE > 0
static int cache_used(void) {
    int n = 0;
    for (unsigned i = 0; i < RDB_KV_CACHE_SIZE; i++) {
        if (g_kv_db.cache.slots[i].klen != 0) n++;
    }
    return n;
}

static void trace_cache_stats(const char *label) {
    int used = cache_used();
    trace_event(&g_trace, "  [CACHE] %s — %d/%u slots used (%u%%)",
                label, used, (unsigned)RDB_KV_CACHE_SIZE,
                (unsigned)(used * 100u / RDB_KV_CACHE_SIZE));
}
#else
static void trace_cache_stats(const char *label) {
    (void)label;
    trace_event(&g_trace, "  [CACHE] disabled (RDB_KV_CACHE_SIZE=0)");
}
#endif

/* ── Deterministic value generator (varied length distribution) ──────────── */

/* Generate a deterministic value for key index i.  Length distribution:
 *   i%5==0: tiny   1-4B
 *   i%5==1: small  8-32B
 *   i%5==2: medium 64-128B
 *   i%5==3: large  200-255B
 *   i%5==4: max    250-255B (near sector capacity boundary)
 * Marker bytes at positions [0] and [vlen-1] enable quick verification. */
static uint16_t make_value(int i, uint8_t *buf, size_t buf_sz) {
    int cat = i % 5;
    uint16_t vlen;
    if (cat == 0)      vlen = (uint16_t)(1  + (i % 4));
    else if (cat == 1) vlen = (uint16_t)(8  + (i % 25));
    else if (cat == 2) vlen = (uint16_t)(64 + (i % 65));
    else if (cat == 3) vlen = (uint16_t)(200 + (i % 56));
    else               vlen = (uint16_t)(250 + (i % 6));
    if (vlen > buf_sz) vlen = (uint16_t)buf_sz;
    memset(buf, (uint8_t)(i & 0xFF), vlen);
    buf[0]     = (uint8_t)(i & 0xFF);
    buf[vlen - 1] = (uint8_t)((i >> 8) & 0xFF);
    return vlen;
}

/* ═══════════════════════════════════════════════════════════════════════════
 *  KVDB Key Cache: many keys, varied lengths, repeated gets
 *
 *  Write 200 keys with 5 length categories (tiny/small/medium/large/max)
 *  across 64 sectors.  Repeated gets exercise cache hit and miss paths.
 * ═══════════════════════════════════════════════════════════════════════════ */

static int kv_cache_write_read_delete(void *ctx) {
    (void)ctx;
    if (kv_setup() != 0) return -1;

    enum { N_KEYS = 200, BUF_SZ = 256 };

    trace_event(&g_trace, "[kv-cache-wr-del] Write %d keys (5 length categories)", N_KEYS);
    for (int i = 0; i < N_KEYS; i++) {
        char key[32];
        uint8_t val[BUF_SZ];
        snprintf(key, sizeof(key), "k%03d", i);
        uint16_t vlen = make_value(i, val, sizeof(val));
        TEST_ASSERT_RDB_OK(trace_kv_set(key, val, vlen));
    }
    { char label[48]; snprintf(label, sizeof(label), "after %d writes", N_KEYS);
      trace_cache_stats(label); }

    trace_event(&g_trace, "[kv-cache-wr-del] First read — verify all %d keys", N_KEYS);
    for (int i = 0; i < N_KEYS; i++) {
        char key[32];
        uint8_t expected[BUF_SZ];
        snprintf(key, sizeof(key), "k%03d", i);
        uint16_t exp_len = make_value(i, expected, sizeof(expected));
        uint8_t buf[BUF_SZ];
        uint16_t olen = 0;
        TEST_ASSERT_RDB_OK(trace_kv_get(key, buf, sizeof(buf), &olen));
        TEST_ASSERT_EQ(olen, exp_len);
        TEST_ASSERT_MEM_EQ(buf, expected, exp_len);
    }
    trace_cache_stats("after 1st read (all warm)");

    trace_event(&g_trace, "[kv-cache-wr-del] Second read — cache hit path");
    for (int i = 0; i < N_KEYS; i++) {
        char key[32];
        uint8_t expected[BUF_SZ];
        snprintf(key, sizeof(key), "k%03d", i);
        uint16_t exp_len = make_value(i, expected, sizeof(expected));
        uint8_t buf[BUF_SZ];
        uint16_t olen = 0;
        TEST_ASSERT_RDB_OK(trace_kv_get(key, buf, sizeof(buf), &olen));
        TEST_ASSERT_EQ(olen, exp_len);
        TEST_ASSERT_MEM_EQ(buf, expected, exp_len);
    }
    trace_cache_stats("after 2nd read (all cache hits)");

    trace_event(&g_trace, "[kv-cache-wr-del] Delete first 40 keys");
    for (int i = 0; i < 40; i++) {
        char key[32];
        snprintf(key, sizeof(key), "k%03d", i);
        TEST_ASSERT_RDB_OK(trace_kv_del(key));
    }
    trace_cache_stats("after 40 deletes");

    trace_event(&g_trace, "[kv-cache-wr-del] Verify deletes + remaining %d keys", N_KEYS - 40);
    for (int i = 0; i < 40; i++) {
        char key[32];
        snprintf(key, sizeof(key), "k%03d", i);
        TEST_ASSERT_RDB_ERR(rdb_kvdb_get(&g_kv_db, key, NULL, 0, NULL),
                            RDB_ERR_NOT_FOUND);
    }
    for (int i = 40; i < N_KEYS; i++) {
        char key[32];
        uint8_t expected[BUF_SZ];
        snprintf(key, sizeof(key), "k%03d", i);
        uint16_t exp_len = make_value(i, expected, sizeof(expected));
        uint8_t buf[BUF_SZ];
        uint16_t olen = 0;
        TEST_ASSERT_RDB_OK(trace_kv_get(key, buf, sizeof(buf), &olen));
        TEST_ASSERT_EQ(olen, exp_len);
        TEST_ASSERT_MEM_EQ(buf, expected, exp_len);
    }

    trace_kvdb_stats(&g_trace, &g_kv_db);
    return 0;
}

/* ═══════════════════════════════════════════════════════════════════════════
 *  KVDB Key Cache: hot-key update consistency
 *
 *  Repeatedly update the same key 500 times with cycling value sizes
 *  (1B → 255B → 8B → 128B → 64B ...).  Cache must track the latest
 *  record address across every size change.
 * ═══════════════════════════════════════════════════════════════════════════ */

static int kv_cache_hot_key_update(void *ctx) {
    (void)ctx;
    if (kv_setup() != 0) return -1;

    enum { N_UPDATES = 500, BUF_SZ = 256 };

    trace_event(&g_trace, "[kv-cache-hot-upd] Hot-key update: %d iterations (varied sizes)", N_UPDATES);
    const char *key = "hot_key";
    for (int i = 0; i < N_UPDATES; i++) {
        uint8_t val[BUF_SZ];
        uint16_t vlen = make_value(i, val, sizeof(val));
        TEST_ASSERT_RDB_OK(trace_kv_set(key, val, vlen));
    }
    { char label[48]; snprintf(label, sizeof(label), "after %d hot-key updates", N_UPDATES);
      trace_cache_stats(label); }

    /* Verify final value (last iteration = N_UPDATES-1) */
    uint8_t buf[BUF_SZ];
    uint8_t expected[BUF_SZ];
    uint16_t exp_len = make_value(N_UPDATES - 1, expected, sizeof(expected));
    uint16_t olen = 0;
    TEST_ASSERT_RDB_OK(trace_kv_get(key, buf, sizeof(buf), &olen));
    TEST_ASSERT_EQ(olen, exp_len);
    TEST_ASSERT_MEM_EQ(buf, expected, exp_len);

    TEST_ASSERT_RDB_OK(trace_kv_del(key));
    TEST_ASSERT_RDB_ERR(rdb_kvdb_get(&g_kv_db, key, NULL, 0, NULL),
                        RDB_ERR_NOT_FOUND);
    trace_cache_stats("after hot-key delete");

    trace_kvdb_stats(&g_trace, &g_kv_db);
    return 0;
}

/* ═══════════════════════════════════════════════════════════════════════════
 *  KVDB GC with 64 sectors: varied-length data integrity
 *
 *  Write+overwrite keys with mixed value sizes across 64 sectors to
 *  trigger GC, then verify data integrity.  Exercises GC migration
 *  with cache updates under varied record sizes.
 * ═══════════════════════════════════════════════════════════════════════════ */

static int kv_cache_gc_stress(void *ctx) {
    (void)ctx;
    if (kv_setup() != 0) return -1;

    enum { N_KEYS = 120, N_ROUNDS = 5, BUF_SZ = 256 };

    trace_event(&g_trace, "[kv-cache-gc-stress] GC stress: %d rounds x %d keys (varied sizes)",
                N_ROUNDS, N_KEYS);
    for (int round = 0; round < N_ROUNDS; round++) {
        trace_event(&g_trace, "  [GC-ROUND] round=%d", round);
        for (int i = 0; i < N_KEYS; i++) {
            char key[32];
            uint8_t val[BUF_SZ];
            snprintf(key, sizeof(key), "gk%03d", i);
            /* Each round gets different values via index = round*N_KEYS + i */
            uint16_t vlen = make_value(round * N_KEYS + i, val, sizeof(val));
            rdb_err_t rc = trace_kv_set(key, val, vlen);
            if (rc != RDB_OK && rc != RDB_ERR_FULL)
                TEST_ASSERT_RDB_OK(rc);
        }
        { char label[48]; snprintf(label, sizeof(label), "after round %d", round);
          trace_cache_stats(label); }
    }

    trace_event(&g_trace, "[kv-cache-gc-stress] Verify latest values after GC");
    int found = 0;
    for (int i = 0; i < N_KEYS; i++) {
        char key[32];
        uint8_t expected[BUF_SZ];
        snprintf(key, sizeof(key), "gk%03d", i);
        (void)make_value((N_ROUNDS - 1) * N_KEYS + i,
                          expected, sizeof(expected));
        uint8_t buf[BUF_SZ];
        uint16_t olen = 0;
        rdb_err_t rc = trace_kv_get(key, buf, sizeof(buf), &olen);
        if (rc == RDB_OK) {
            TEST_ASSERT_GT(olen, 0u);
            /* First byte marker: value[0] = (index & 0xFF) */
            TEST_ASSERT_EQ(buf[0], expected[0]);
            found++;
        }
    }
    TEST_ASSERT_GT(found, 0);
    trace_event(&g_trace, "  [GC-RESULT] found=%d keys after GC (varied sizes)", found);

    trace_kvdb_stats(&g_trace, &g_kv_db);
    return 0;
}

/* ═══════════════════════════════════════════════════════════════════════════
 *  TSDB append/query across 32-sector ring with varied data sizes
 *
 *  Append 2000 records with cycling data sizes (1, 2, 4, 8, 16, 32 B).
 *  Verify time-range queries work and recount accuracy holds.
 * ═══════════════════════════════════════════════════════════════════════════ */

static int ts_count_cb(uint32_t time, const void *data, uint16_t len,
                       void *arg) {
    (void)time; (void)data; (void)len;
    int *cnt = (int *)arg;
    (*cnt)++;
    return 0;
}

static int ts_append_query_ring(void *ctx) {
    (void)ctx;
    if (ts_setup() != 0) return -1;

    enum { N_RECORDS = 2000 };
    /* Data size cycling: 1, 2, 4, 8, 16, 32 bytes */
    static const uint16_t dsize_table[] = { 1, 2, 4, 8, 16, 32 };
    enum { DS_N = sizeof(dsize_table) / sizeof(dsize_table[0]) };

    trace_event(&g_trace, "[ts-append-query] Append %d records (varied data sizes)", N_RECORDS);
    uint32_t ts = 1000;
    for (int i = 0; i < N_RECORDS; i++) {
        uint16_t dlen = dsize_table[i % DS_N];
        uint8_t data[32];
        memset(data, (uint8_t)(i & 0xFF), dlen);
        data[0] = (uint8_t)(i & 0xFF);
        if (dlen > 1) data[dlen - 1] = (uint8_t)((i >> 8) & 0xFF);
        rdb_err_t rc = trace_ts_append(ts, data, dlen);
        if (rc == RDB_ERR_TIME_EXHAUSTED) {
            trace_event(&g_trace, "  [TS-EPOCH] reset at i=%d ts=%u", i, (unsigned)ts);
            TEST_ASSERT_RDB_OK(rdb_tsdb_reset_epoch(&g_ts_db));
            ts = 1000;
            rc = trace_ts_append(ts, data, dlen);
        }
        TEST_ASSERT_RDB_OK(rc);
        ts += 100;
    }

    trace_event(&g_trace, "  [ts-append-query] total_count=%u write_ops=%u head_sec=%u",
                (unsigned)g_ts_db.total_count,
                (unsigned)g_ts_db.stats.write_ops,
                (unsigned)g_ts_db.head_sec);

    TEST_ASSERT_LE(g_ts_db.total_count, g_ts_db.stats.write_ops);
    TEST_ASSERT_GT(g_ts_db.total_count, 0u);

    int count = 0;
    trace_event(&g_trace, "[ts-append-query] Full range query (1000..%u)",
                (unsigned)g_ts_db.last_time);
    TEST_ASSERT_RDB_OK(rdb_tsdb_query(&g_ts_db, 1000, g_ts_db.last_time,
                                      ts_count_cb, &count));
    trace_event(&g_trace, "  [ts-append-query] Full range result: %d records", count);
    TEST_ASSERT_GT(count, 0);

    int sub = 0;
    trace_event(&g_trace, "[ts-append-query] Narrow query (5000..10000)");
    TEST_ASSERT_RDB_OK(rdb_tsdb_query(&g_ts_db, 5000, 10000,
                                      ts_count_cb, &sub));
    trace_event(&g_trace, "  [ts-append-query] Narrow range result: %d records", sub);
    TEST_ASSERT_GT(sub, 0);

    trace_tsdb_stats(&g_trace, &g_ts_db);
    trace_tsdb_sector_summary(&g_trace, &g_ts_db);
    return 0;
}

/* ═══════════════════════════════════════════════════════════════════════════
 *  Large-sector init/format verification
 *
 *  Verify init after format with large sector counts.
 * ═══════════════════════════════════════════════════════════════════════════ */

static int init_format_large_sectors(void *ctx) {
    (void)ctx;

    trace_event(&g_trace, "[init-format-large] KVDB large-sector format+init (64 sectors)");
    if (sim_flash_init(&g_kv_sim, g_kv_flash, KV_FLASH_SIZE,
                       KV_SECTOR_SIZE, KV_PAGE_SIZE, KV_WG) != 0)
        return -1;
    sim_flash_set_trace(&g_kv_sim, &g_trace);
    g_kv_part = (rdb_partition_t) {
        .name = "KVDB", .base_addr = 0, .total_size = KVDB_PART_SIZE,
        .sector_size = KV_SECTOR_SIZE, .write_gran = KV_WG, .ops = &g_kv_ops
    };
    memset(&g_kv_db, 0, sizeof(g_kv_db));
    g_kv_db.part = &g_kv_part;
    g_kv_db.sectors = g_kv_meta;
    g_kv_db.sector_cnt = KV_SECTOR_CNT;
    TEST_ASSERT_RDB_OK(rdb_kvdb_format(&g_kv_db));
    TEST_ASSERT_RDB_OK(rdb_kvdb_init(&g_kv_db, &g_kv_part, g_kv_meta));
    TEST_ASSERT_EQ(g_kv_db.sector_cnt, 64u);
    trace_kvdb_snapshot(&g_trace, &g_kv_db);

    trace_event(&g_trace, "[init-format-large] TSDB large-sector format+init (32 sectors)");
    if (sim_flash_init(&g_ts_sim, g_ts_flash, TS_FLASH_SIZE,
                       TS_SECTOR_SIZE, TS_PAGE_SIZE, TS_WG) != 0)
        return -1;
    sim_flash_set_trace(&g_ts_sim, &g_trace);
    g_ts_part = (rdb_partition_t) {
        .name = "TSDB", .base_addr = 0, .total_size = TSDB_PART_SIZE,
        .sector_size = TS_SECTOR_SIZE, .write_gran = TS_WG, .ops = &g_ts_ops
    };
    memset(&g_ts_db, 0, sizeof(g_ts_db));
    memset(g_ts_ec, 0, sizeof(g_ts_ec));
    g_ts_db.part = &g_ts_part;
    g_ts_db.erase_cnts = g_ts_ec;
    g_ts_db.sector_cnt = TS_SECTOR_CNT;
    TEST_ASSERT_RDB_OK(rdb_tsdb_format(&g_ts_db));
    TEST_ASSERT_RDB_OK(rdb_tsdb_init(&g_ts_db, &g_ts_part, g_ts_ec));
    TEST_ASSERT_EQ(g_ts_db.sector_cnt, 32u);
    trace_tsdb_snapshot(&g_trace, &g_ts_db);
    trace_tsdb_sector_summary(&g_trace, &g_ts_db);

    return 0;
}

/* ═══════════════════════════════════════════════════════════════════════════
 *  TSDB safety: recover after write failure
 *
 *  Append records, inject commit-byte write faults, then re-init.
 *  Verifies head_off advancement (Phase 1.2 fix) prevents NOR
 *  violations on the next append after recovery.
 * ═══════════════════════════════════════════════════════════════════════════ */

static int ts_safety_recover_faults(void *ctx) {
    (void)ctx;
    if (ts_setup() != 0) return -1;

    trace_event(&g_trace, "[ts-safety-recover] Append 50 baseline records");
    for (int i = 0; i < 50; i++) {
        uint8_t data[8];
        memset(data, (uint8_t)i, sizeof(data));
        TEST_ASSERT_RDB_OK(trace_ts_append((uint32_t)((i + 1) * 100),
                                           data, sizeof(data)));
    }

    trace_event(&g_trace, "[ts-safety-recover] Inject commit-byte write faults (prob=30%%)");
    fault_ctx_t fctx;
    fault_init(&fctx, 0xBEEF0001u);
    sim_flash_set_fault_ctx(&g_ts_sim, &fctx);
    fault_quick_write_fail_probability(&fctx, 30);

    int failures = 0;
    for (int i = 0; i < 10; i++) {
        uint8_t data[8];
        memset(data, (uint8_t)(i + 50), sizeof(data));
        rdb_err_t rc = trace_ts_append((uint32_t)((i + 51) * 100),
                                       data, sizeof(data));
        if (rc == RDB_ERR_FLASH) {
            trace_event(&g_trace, "  [FAULT] append %d failed with RDB_ERR_FLASH", i);
            failures++;
        }
        if (rc != RDB_OK && rc != RDB_ERR_FLASH)
            TEST_ASSERT_RDB_OK(rc);
    }
    TEST_ASSERT_GT(failures, 0);
    trace_event(&g_trace, "  [FAULT] %d of 10 appends failed (expected)", failures);

    sim_flash_set_fault_ctx(&g_ts_sim, NULL);
    trace_event(&g_trace, "[ts-safety-recover] Recover: format+init after faults");

    memset(&g_ts_db, 0, sizeof(g_ts_db));
    memset(g_ts_ec, 0, sizeof(g_ts_ec));
    g_ts_db.part = &g_ts_part;
    g_ts_db.erase_cnts = g_ts_ec;
    g_ts_db.sector_cnt = TS_SECTOR_CNT;
    TEST_ASSERT_RDB_OK(rdb_tsdb_format(&g_ts_db));
    TEST_ASSERT_RDB_OK(rdb_tsdb_init(&g_ts_db, &g_ts_part, g_ts_ec));
    trace_tsdb_snapshot(&g_trace, &g_ts_db);

    trace_event(&g_trace, "[ts-safety-recover] Post-recovery appends (must not NOR-violate)");
    int recovered_appends = 0;
    for (int i = 0; i < 30; i++) {
        uint8_t data[8];
        memset(data, (uint8_t)(i + 100), sizeof(data));
        TEST_ASSERT_RDB_OK(trace_ts_append((uint32_t)((i + 1) * 200),
                                           data, sizeof(data)));
        recovered_appends++;
    }
    TEST_ASSERT_EQ(recovered_appends, 30);
    trace_event(&g_trace, "  [ts-safety-recover] All %d post-recovery appends OK", recovered_appends);

    trace_tsdb_stats(&g_trace, &g_ts_db);
    return 0;
}

/* ═══════════════════════════════════════════════════════════════════════════
 *  KVDB cache survives GC with 64 sectors, varied-length values
 *
 *  Write+overwrite 100 keys with varied sizes across 3 phases to
 *  generate garbage, verify reads after GC.  Cache must track
 *  migrated records at new addresses.
 * ═══════════════════════════════════════════════════════════════════════════ */

static int kv_cache_gc_migration(void *ctx) {
    (void)ctx;
    if (kv_setup() != 0) return -1;

    enum { N_KEYS = 100, BUF_SZ = 256 };

    trace_event(&g_trace, "[kv-cache-gc-mig] Phase 1: write %d keys (varied sizes)", N_KEYS);
    for (int i = 0; i < N_KEYS; i++) {
        char key[32];
        uint8_t val[BUF_SZ];
        snprintf(key, sizeof(key), "mg%03d", i);
        uint16_t vlen = make_value(i, val, sizeof(val));
        TEST_ASSERT_RDB_OK(trace_kv_set(key, val, vlen));
    }
    { char label[48]; snprintf(label, sizeof(label), "after phase 1 (%d keys)", N_KEYS);
      trace_cache_stats(label); }

    trace_event(&g_trace, "[kv-cache-gc-mig] Phase 2: overwrite all %d (creates garbage)", N_KEYS);
    for (int i = 0; i < N_KEYS; i++) {
        char key[32];
        uint8_t val[BUF_SZ];
        snprintf(key, sizeof(key), "mg%03d", i);
        uint16_t vlen = make_value(i + 10000, val, sizeof(val));
        TEST_ASSERT_RDB_OK(trace_kv_set(key, val, vlen));
    }
    trace_cache_stats("after phase 2 (overwrite)");

    trace_event(&g_trace, "[kv-cache-gc-mig] Phase 3: overwrite again (triggers GC)");
    for (int i = 0; i < N_KEYS; i++) {
        char key[32];
        uint8_t val[BUF_SZ];
        snprintf(key, sizeof(key), "mg%03d", i);
        uint16_t vlen = make_value(i + 20000, val, sizeof(val));
        rdb_err_t rc = trace_kv_set(key, val, vlen);
        if (rc != RDB_OK && rc != RDB_ERR_FULL)
            TEST_ASSERT_RDB_OK(rc);
    }

    trace_event(&g_trace, "[kv-cache-gc-mig] Phase 4: read back — verify consistency");
    int found = 0;
    for (int i = 0; i < N_KEYS; i++) {
        char key[32];
        uint8_t expected[BUF_SZ];
        snprintf(key, sizeof(key), "mg%03d", i);
        (void)make_value(i + 20000, expected, sizeof(expected));
        uint8_t buf[BUF_SZ];
        uint16_t olen = 0;
        rdb_err_t rc = trace_kv_get(key, buf, sizeof(buf), &olen);
        if (rc == RDB_OK) {
            TEST_ASSERT_GT(olen, 0u);
            TEST_ASSERT_EQ(buf[0], expected[0]);
            found++;
        }
    }
    TEST_ASSERT_GT(found, 0);
    trace_event(&g_trace, "  [kv-cache-gc-mig] Found %d of %d keys after GC", found, N_KEYS);
    trace_cache_stats("after GC migration + readback");

    trace_kvdb_stats(&g_trace, &g_kv_db);
    return 0;
}

/* ═══════════════════════════════════════════════════════════════════════════
 *  KVDB Cache collision with varied value sizes (RDB_KV_CACHE_SIZE > 0)
 *
 *  100 keys with identical hash+prefix+klen but different suffix bytes
 *  and varied value sizes.  Verifies the cache hit path's full key-byte
 *  comparison correctly disambiguates collisions with different-sized
 *  values and falls back to full-table scan.
 *
 *  Key design: all keys share the same first 8 bytes ("cachecol")
 *  and same total length (28), forcing cache fingerprint collisions.
 *  The 3-digit variant at bytes 8-10 distinguishes them.
 * ═══════════════════════════════════════════════════════════════════════════ */

#if RDB_KV_CACHE_SIZE > 0
static int kv_cache_collision_stress(void *ctx) {
    (void)ctx;
    if (kv_setup() != 0) return -1;

    enum { N_KEYS = 100, BUF_SZ = 256 };

    trace_event(&g_trace, "[kv-cache-collide] Cache collision: write %d keys (same prefix+len, varied vals)",
                N_KEYS);
    for (int i = 0; i < N_KEYS; i++) {
        char key[32];
        uint8_t val[BUF_SZ];
        memset(key, 'X', 28);
        key[28] = '\0';
        memcpy(key, "cachecol", 8);
        key[8]  = (char)('0' + (i / 100) % 10);
        key[9]  = (char)('0' + (i / 10) % 10);
        key[10] = (char)('0' + i % 10);
        uint16_t vlen = make_value(i, val, sizeof(val));
        TEST_ASSERT_RDB_OK(trace_kv_set(key, val, vlen));
    }
    { char label[56]; snprintf(label, sizeof(label), "after %d collision keys written", N_KEYS);
      trace_cache_stats(label); }

    trace_event(&g_trace, "[kv-cache-collide] First read: verify all %d under collision pressure",
                N_KEYS);
    for (int i = 0; i < N_KEYS; i++) {
        char key[32];
        uint8_t expected[BUF_SZ];
        memset(key, 'X', 28);
        key[28] = '\0';
        memcpy(key, "cachecol", 8);
        key[8]  = (char)('0' + (i / 100) % 10);
        key[9]  = (char)('0' + (i / 10) % 10);
        key[10] = (char)('0' + i % 10);
        uint16_t exp_len = make_value(i, expected, sizeof(expected));
        uint8_t buf[BUF_SZ];
        uint16_t olen = 0;
        TEST_ASSERT_RDB_OK(trace_kv_get(key, buf, sizeof(buf), &olen));
        TEST_ASSERT_EQ(olen, exp_len);
        TEST_ASSERT_MEM_EQ(buf, expected, exp_len);
    }
    trace_cache_stats("after 1st read (collision warm)");

    trace_event(&g_trace, "[kv-cache-collide] Second read: cache hit path under collision pressure");
    for (int i = 0; i < N_KEYS; i++) {
        char key[32];
        uint8_t expected[BUF_SZ];
        memset(key, 'X', 28);
        key[28] = '\0';
        memcpy(key, "cachecol", 8);
        key[8]  = (char)('0' + (i / 100) % 10);
        key[9]  = (char)('0' + (i / 10) % 10);
        key[10] = (char)('0' + i % 10);
        uint16_t exp_len = make_value(i, expected, sizeof(expected));
        uint8_t buf[BUF_SZ];
        uint16_t olen = 0;
        TEST_ASSERT_RDB_OK(trace_kv_get(key, buf, sizeof(buf), &olen));
        TEST_ASSERT_EQ(olen, exp_len);
        TEST_ASSERT_MEM_EQ(buf, expected, exp_len);
    }
    trace_cache_stats("after 2nd read (collision hits)");

    trace_event(&g_trace, "[kv-cache-collide] Delete first 50 keys (cache invalidation)");
    for (int i = 0; i < 50; i++) {
        char key[32];
        memset(key, 'X', 28);
        key[28] = '\0';
        memcpy(key, "cachecol", 8);
        key[8]  = (char)('0' + (i / 100) % 10);
        key[9]  = (char)('0' + (i / 10) % 10);
        key[10] = (char)('0' + i % 10);
        TEST_ASSERT_RDB_OK(trace_kv_del(key));
    }

    trace_event(&g_trace, "[kv-cache-collide] Verify: deleted gone, remainder readable");
    for (int i = 0; i < N_KEYS; i++) {
        char key[32];
        uint8_t expected[BUF_SZ];
        memset(key, 'X', 28);
        key[28] = '\0';
        memcpy(key, "cachecol", 8);
        key[8]  = (char)('0' + (i / 100) % 10);
        key[9]  = (char)('0' + (i / 10) % 10);
        key[10] = (char)('0' + i % 10);
        uint8_t buf[BUF_SZ];
        if (i < 50) {
            TEST_ASSERT_RDB_ERR(rdb_kvdb_get(&g_kv_db, key, NULL, 0, NULL),
                                RDB_ERR_NOT_FOUND);
        } else {
            uint16_t exp_len = make_value(i, expected, sizeof(expected));
            uint16_t olen = 0;
            TEST_ASSERT_RDB_OK(trace_kv_get(key, buf, sizeof(buf), &olen));
            TEST_ASSERT_EQ(olen, exp_len);
            TEST_ASSERT_MEM_EQ(buf, expected, exp_len);
        }
    }
    trace_cache_stats("after 50 deletes (collision)");

    trace_kvdb_stats(&g_trace, &g_kv_db);
    return 0;
}
#endif /* RDB_KV_CACHE_SIZE > 0 */

static int kv_cache_max_key_hit(void *ctx) {
    (void)ctx;
    if (kv_setup() != 0) return -1;

#if RDB_KV_CACHE_SIZE > 0
    char key[RDB_MAX_KEY_LEN + 1u];
    uint8_t val[32];
    uint8_t out[32];
    uint16_t out_len = 0;
    uint32_t klen = RDB_MAX_KEY_LEN;

    for (uint32_t i = 0; i < klen; i++)
        key[i] = (char)('a' + (i % 26u));
    key[klen] = '\0';
    for (uint16_t i = 0; i < sizeof(val); i++)
        val[i] = (uint8_t)(0xA0u + i);

    TEST_ASSERT_RDB_OK(trace_kv_set(key, val, (uint16_t)sizeof(val)));
    TEST_ASSERT_RDB_OK(trace_kv_get(key, out, sizeof(out), &out_len));
    TEST_ASSERT_EQ(out_len, (uint16_t)sizeof(val));
    TEST_ASSERT_MEM_EQ(out, val, sizeof(val));

    memset(out, 0, sizeof(out));
    out_len = 0;
    TEST_ASSERT_RDB_OK(trace_kv_get(key, out, sizeof(out), &out_len));
    TEST_ASSERT_EQ(out_len, (uint16_t)sizeof(val));
    TEST_ASSERT_MEM_EQ(out, val, sizeof(val));
#else
    trace_event(&g_trace, "[kv-cache-max-key] skipped because cache is disabled");
#endif
    return 0;
}

/* ═══════════════════════════════════════════════════════════════════════════
 *  Post-test hooks for sector summaries
 * ═══════════════════════════════════════════════════════════════════════════ */

static void post_test_sectors(const char *name, int result, void *ctx) {
    (void)result; (void)ctx;
    /* Name prefix encodes DB type: kv_ → KVDB, ts_ → TSDB, init_ → both */
    if (strstr(name, "ts_") == name || strstr(name, "ts_append") == name) {
        if (g_ts_db.initialized) {
            trace_tsdb_sector_summary(&g_trace, &g_ts_db);
            trace_tsdb_stats(&g_trace, &g_ts_db);
        }
    } else if (strstr(name, "init_") == name) {
        if (g_kv_db.initialized) {
            trace_kvdb_sector_summary(&g_trace, &g_kv_db);
            trace_kvdb_stats(&g_trace, &g_kv_db);
        }
        if (g_ts_db.initialized) {
            trace_tsdb_sector_summary(&g_trace, &g_ts_db);
            trace_tsdb_stats(&g_trace, &g_ts_db);
        }
    } else {
        if (g_kv_db.initialized) {
            trace_kvdb_sector_summary(&g_trace, &g_kv_db);
            trace_kvdb_stats(&g_trace, &g_kv_db);
        }
    }
}

/* ═══════════════════════════════════════════════════════════════════════════
 *  Test registration
 * ═══════════════════════════════════════════════════════════════════════════ */

static test_case_t g_cases[] = {
    { "kv_cache_write_read_delete", "KVDB cache: 200 keys (5 length categories) write/read/delete",
      kv_cache_write_read_delete, "KVDB-Cache", 1, NULL },
    { "kv_cache_hot_key_update", "KVDB cache: hot-key 500 updates (cycling sizes)",
      kv_cache_hot_key_update, "KVDB-Cache", 1, NULL },
    { "kv_cache_gc_stress", "KVDB GC: 120 keys x 5 rounds, varied-length stress",
      kv_cache_gc_stress, "KVDB-GC", 1, NULL },
    { "ts_append_query_ring", "TSDB: 2000 appends across 32-sector ring (1-32B data)",
      ts_append_query_ring, "TSDB-Recount", 1, NULL },
    { "init_format_large_sectors", "Large-sector init/format (KV 64 + TS 32)",
      init_format_large_sectors, "Init", 1, NULL },
    { "ts_safety_recover_faults", "TSDB safety: recover after write faults",
      ts_safety_recover_faults, "TSDB-Safety", 1, NULL },
    { "kv_cache_gc_migration", "KVDB cache: 100 keys GC migration, varied-length values",
      kv_cache_gc_migration, "KVDB-GC", 1, NULL },
#if RDB_KV_CACHE_SIZE > 0
    { "kv_cache_collision_stress", "KVDB cache: 100-key collision stress, varied values",
      kv_cache_collision_stress, "KVDB-Cache", 1, NULL },
#endif
    { "kv_cache_max_key_hit", "KVDB cache: cache-hit verification with 32-byte max key",
      kv_cache_max_key_hit, "KVDB-Cache", 1, NULL },
};

int main(int argc, char **argv) {
    (void)argc; (void)argv;

    test_config_t config = {
        .log_file = fopen(test_make_log_path("kvdb_cache"), "w"),
        .verbose = 1, .stop_on_fail = 0, .filter = NULL,
        .post_test_hook = post_test_sectors, .hook_ctx = NULL
    };
    test_framework_init(&config);

    trace_init(&g_trace, config.log_file, config.verbose);
    trace_event(&g_trace, "=== KVDB Cache + TSDB Safety Test Suite Start ===");

    test_suite_t *s = test_get_default_suite();

    int total = (int)(sizeof(g_cases) / sizeof(g_cases[0]));
    for (int i = 0; i < total; i++) {
        test_register_case(s, &g_cases[i]);
    }

    trace_event(&g_trace, "KVDB: %u sectors (%u KB)  TSDB: %u sectors (%u KB)",
                (unsigned)KV_SECTOR_CNT, (unsigned)(KVDB_PART_SIZE / 1024u),
                (unsigned)TS_SECTOR_CNT, (unsigned)(TSDB_PART_SIZE / 1024u));
#if RDB_KV_CACHE_SIZE > 0
    trace_event(&g_trace, "KV Cache: %u slots (%u bytes)",
                (unsigned)RDB_KV_CACHE_SIZE,
                (unsigned)sizeof(rdb_kv_cache_t));
#else
    trace_event(&g_trace, "KV Cache: DISABLED (RDB_KV_CACHE_SIZE=0)");
#endif

    int failed = test_run_all(NULL);

    trace_event(&g_trace, "=== KVDB Cache + TSDB Safety Test Suite End ===\n");

    test_print_report();
    if (config.log_file) fclose(config.log_file);
    return failed;
}
