/**
 * test_kvdb_basic.c — KVDB basic functionality tests
 *
 * Covers: set/get, update, delete, exists, TOO_LARGE,
 *         write granularity matrix (1/2/4/8B),
 *         sequence number wrap-around recovery,
 *         mixed key/value length distributions,
 *         corrupt record header skip during iteration.
 *
 * References: T-300, T-302, T-304, T-306, T-307
 */

#include "sim_flash.h"
#include "sim_dist.h"
#include "test_framework.h"
#include "../../src/rocketdb.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define FLASH_SIZE      (128u * 1024u)
#define SECTOR_SIZE     4096u
#define PAGE_SIZE       256u
#define DEFAULT_WG      0u
#define KVDB_PART_SIZE  (64u * 1024u)
#define KV_SECTOR_CNT   (KVDB_PART_SIZE / SECTOR_SIZE)

/* ── Shared flash environment ──────────────────────────────────────────── */

static uint8_t              g_flash_buf[FLASH_SIZE];
static sim_flash_t          g_flash;
static rdb_partition_t      g_part;
static rdb_kvdb_t           g_db;
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

static rdb_err_t kv_reset(uint8_t write_gran)
{
    if (sim_flash_init(&g_flash, g_flash_buf, FLASH_SIZE,
                       SECTOR_SIZE, PAGE_SIZE, write_gran) != 0)
        return RDB_ERR_FLASH;
    g_part = (rdb_partition_t) {
        .name = "KVDB", .base_addr = 0, .total_size = KVDB_PART_SIZE,
        .sector_size = SECTOR_SIZE, .write_gran = write_gran, .ops = &g_ops
    };
    g_db.part = &g_part;
    g_db.sectors = g_meta;
    g_db.sector_cnt = (uint8_t)KV_SECTOR_CNT;

    trace_event(&g_trace, "KVDB format+init (wg=%u)", write_gran);
    rdb_err_t ret = rdb_kvdb_format(&g_db);
    if (ret != RDB_OK) return ret;
    ret = rdb_kvdb_init(&g_db, &g_part, g_meta);
    if (ret == RDB_OK)
        trace_kvdb_snapshot(&g_trace, &g_db);
    return ret;
}

static uint32_t kv_data_start(uint8_t wg) {
    return RDB_ALIGN_UP((uint32_t)sizeof(rdb_kv_sector_hdr_t), 1u << wg);
}

/* ── Helpers for key construction ──────────────────────────────────────── */

static void build_key_2d(char *key, int idx) {
    key[0] = 'K'; key[1] = (char)('0' + (idx / 10));
    key[2] = (char)('0' + (idx % 10)); key[3] = '\0';
}

static int key_index_2d(const char *key) {
    if (!key || key[0] != 'K' || key[1] < '0' || key[1] > '9' ||
        key[2] < '0' || key[2] > '9') return -1;
    return (key[1] - '0') * 10 + (key[2] - '0');
}

static uint32_t lcg_next(uint32_t *s) {
    *s = (*s * 1664525u) + 1013904223u;
    return *s;
}

/* ═══════════════════════════════════════════════════════════════════════════
 *  T-300: set/get/update/delete/exists/TOO_LARGE
 * ═══════════════════════════════════════════════════════════════════════════ */

TEST_CASE(kv_set_get_basic, "KVDB", "Basic set/get operations")
{
    (void)ctx;
    TEST_ASSERT_RDB_OK(kv_reset(DEFAULT_WG));
    const char *key = "alpha";
    const uint8_t val[] = { 1, 2, 3, 4, 5 };

    trace_kv_op(&g_trace, "SET", key, 5, (uint16_t)sizeof(val), g_db.write_seq, RDB_OK);
    TEST_ASSERT_RDB_OK(rdb_kvdb_set(&g_db, key, val, (uint16_t)sizeof(val)));
    uint8_t out[8] = { 0 }; uint16_t out_len = 0;
    TEST_ASSERT_RDB_OK(rdb_kvdb_get(&g_db, key, out, sizeof(out), &out_len));
    trace_kv_op(&g_trace, "GET", key, 5, out_len, 0, RDB_OK);
    TEST_ASSERT_EQ(out_len, (uint16_t)sizeof(val));
    TEST_ASSERT_MEM_EQ(out, val, sizeof(val));
    TEST_ASSERT_RDB_OK(rdb_kvdb_exists(&g_db, key));

    out_len = 0;
    TEST_ASSERT_RDB_ERR(rdb_kvdb_get(&g_db, key, NULL, 0, &out_len), RDB_ERR_TOO_LARGE);
    TEST_ASSERT_EQ(out_len, (uint16_t)sizeof(val));
    return 0;
}

TEST_CASE(kv_set_update, "KVDB", "Update existing key")
{
    (void)ctx;
    TEST_ASSERT_RDB_OK(kv_reset(DEFAULT_WG));
    const char *key = "k1";
    const uint8_t v1[] = { '1', '2', '3' };
    const uint8_t v2[] = { 'A', 'B', 'C', 'D', 'E' };

    TEST_ASSERT_RDB_OK(rdb_kvdb_set(&g_db, key, v1, (uint16_t)sizeof(v1)));
    TEST_ASSERT_RDB_OK(rdb_kvdb_set(&g_db, key, v2, (uint16_t)sizeof(v2)));

    uint8_t out[8] = { 0 }; uint16_t out_len = 0;
    TEST_ASSERT_RDB_OK(rdb_kvdb_get(&g_db, key, out, sizeof(out), &out_len));
    TEST_ASSERT_EQ(out_len, (uint16_t)sizeof(v2));
    TEST_ASSERT_MEM_EQ(out, v2, sizeof(v2));
    return 0;
}

TEST_CASE(kv_delete_exists, "KVDB", "Delete and exists checks")
{
    (void)ctx;
    TEST_ASSERT_RDB_OK(kv_reset(DEFAULT_WG));
    const char *key = "to_delete";
    const uint8_t val[] = { 0xAA, 0xBB };

    TEST_ASSERT_RDB_OK(rdb_kvdb_set(&g_db, key, val, (uint16_t)sizeof(val)));
    TEST_ASSERT_RDB_OK(rdb_kvdb_exists(&g_db, key));
    TEST_ASSERT_RDB_OK(rdb_kvdb_delete(&g_db, key));
    TEST_ASSERT_RDB_ERR(rdb_kvdb_exists(&g_db, key), RDB_ERR_NOT_FOUND);

    uint8_t out[4] = { 0 }; uint16_t out_len = 0;
    TEST_ASSERT_RDB_ERR(rdb_kvdb_get(&g_db, key, out, sizeof(out), &out_len), RDB_ERR_NOT_FOUND);
    return 0;
}

TEST_CASE(kv_get_too_large, "KVDB", "Get with undersized buffer")
{
    (void)ctx;
    TEST_ASSERT_RDB_OK(kv_reset(DEFAULT_WG));
    const char *key = "big";
    const uint8_t val[] = { 1, 2, 3, 4, 5, 6, 7, 8 };

    TEST_ASSERT_RDB_OK(rdb_kvdb_set(&g_db, key, val, (uint16_t)sizeof(val)));
    uint8_t out[4] = { 0 }; uint16_t out_len = 0;
    TEST_ASSERT_RDB_ERR(rdb_kvdb_get(&g_db, key, out, sizeof(out), &out_len), RDB_ERR_TOO_LARGE);
    TEST_ASSERT_EQ(out_len, (uint16_t)sizeof(val));
    return 0;
}

/* ═══════════════════════════════════════════════════════════════════════════
 *  T-306: write granularity 1/2/4/8B
 * ═══════════════════════════════════════════════════════════════════════════ */

TEST_CASE(kv_write_gran_matrix, "KVDB", "Write granularity matrix 1/2/4/8B")
{
    (void)ctx;
    const uint8_t v1[] = { 1, 2, 3, 4, 5, 6 };
    const uint8_t v2[] = { 9, 8, 7, 6 };
    uint8_t out[16];
    uint16_t out_len;

    for (uint8_t gran = 0; gran <= 3; gran++) {
        TEST_ASSERT_RDB_OK(kv_reset(gran));
        TEST_ASSERT_RDB_OK(rdb_kvdb_set(&g_db, "WG", v1, (uint16_t)sizeof(v1)));
        TEST_ASSERT_RDB_OK(rdb_kvdb_set(&g_db, "WG", v2, (uint16_t)sizeof(v2)));
        memset(out, 0, sizeof(out));
        out_len = 0;
        TEST_ASSERT_RDB_OK(rdb_kvdb_get(&g_db, "WG", out, sizeof(out), &out_len));
        TEST_ASSERT_EQ(out_len, (uint16_t)sizeof(v2));
        TEST_ASSERT_MEM_EQ(out, v2, sizeof(v2));
    }
    return 0;
}

/* ═══════════════════════════════════════════════════════════════════════════
 *  T-302: sequence wrap-around recovery
 * ═══════════════════════════════════════════════════════════════════════════ */

TEST_CASE(kv_seq_wrap_recovery, "KVDB", "Sequence wrap-around recovery")
{
    (void)ctx;
    TEST_ASSERT_RDB_OK(kv_reset(DEFAULT_WG));
    g_db.write_seq = 0xFFFFFFFEu;

    TEST_ASSERT_RDB_OK(rdb_kvdb_set(&g_db, "wrap", "old", 3));
    TEST_ASSERT_RDB_OK(rdb_kvdb_set(&g_db, "wrap", "new", 3));
    TEST_ASSERT_EQ(g_db.write_seq, 0u);

    /* Power-cycle: re-init without erasing flash */
    TEST_ASSERT_RDB_OK(rdb_kvdb_init(&g_db, &g_part, g_meta));

    char out[8] = { 0 }; uint16_t out_len = 0;
    TEST_ASSERT_RDB_OK(rdb_kvdb_get(&g_db, "wrap", out, sizeof(out), &out_len));
    TEST_ASSERT_EQ(out_len, 3);
    TEST_ASSERT_MEM_EQ(out, "new", 3);
    return 0;
}

/* ═══════════════════════════════════════════════════════════════════════════
 *  T-307: mixed key/value length distributions
 * ═══════════════════════════════════════════════════════════════════════════ */

#define MIX_KEY_CNT     32
#define MIX_MAX_VAL     256
#define MIX_ROUNDS      8

TEST_CASE(kv_mixed_lengths, "KVDB", "Mixed key/value length Monte Carlo")
{
    (void)ctx;
    TEST_ASSERT_RDB_OK(kv_reset(DEFAULT_WG));

    char     keys[MIX_KEY_CNT][RDB_MAX_KEY_LEN + 1];
    uint8_t  expected[MIX_KEY_CNT][MIX_MAX_VAL];
    uint16_t expected_len[MIX_KEY_CNT];

    sim_dist_t kdist, vdist;
    sim_dist_init_gaussian(&kdist, 0x1111u, 6, 24, 10.0, 4.0);
    sim_dist_init_powerlaw(&vdist, 0x2222u, 1, MIX_MAX_VAL, 1.6);

    for (uint32_t i = 0; i < MIX_KEY_CNT; i++) {
        uint32_t klen = sim_dist_next(&kdist);
        char prefix[8];
        snprintf(prefix, sizeof(prefix), "K%02u", (unsigned)i);
        uint32_t pl = (uint32_t)strlen(prefix);
        if (klen < pl + 1u) klen = pl + 1u;
        if (klen > RDB_MAX_KEY_LEN) klen = RDB_MAX_KEY_LEN;
        memcpy(keys[i], prefix, pl);
        for (uint32_t j = pl; j < klen; j++) keys[i][j] = (char)('a' + (i % 26u));
        keys[i][klen] = '\0';
        expected_len[i] = 0;
    }

    uint32_t seed = 0xA5A5A5A5u;
    for (uint32_t round = 0; round < MIX_ROUNDS; round++) {
        for (uint32_t i = 0; i < MIX_KEY_CNT; i++) {
            uint16_t vlen = (uint16_t)sim_dist_next(&vdist);
            if (vlen == 0) vlen = 1;
            if (vlen > MIX_MAX_VAL) vlen = MIX_MAX_VAL;
            for (uint16_t j = 0; j < vlen; j++) expected[i][j] = (uint8_t)lcg_next(&seed);
            expected_len[i] = vlen;
            TEST_ASSERT_RDB_OK(rdb_kvdb_set(&g_db, keys[i], expected[i], vlen));
        }
    }

    for (uint32_t i = 0; i < MIX_KEY_CNT; i++) {
        uint8_t out[MIX_MAX_VAL]; uint16_t out_len = 0;
        TEST_ASSERT_RDB_OK(rdb_kvdb_get(&g_db, keys[i], out, sizeof(out), &out_len));
        TEST_ASSERT_EQ(out_len, expected_len[i]);
        TEST_ASSERT_MEM_EQ(out, expected[i], expected_len[i]);
    }
    return 0;
}

/* ═══════════════════════════════════════════════════════════════════════════
 *  T-304: corrupt record header skip during iteration
 * ═══════════════════════════════════════════════════════════════════════════ */

#define CORRUPT_KEY_CNT 16
#define CORRUPT_GC_TGT  1u
#define CORRUPT_MAX_LOOPS 5000u

TEST_CASE(kv_corrupt_header_skip, "KVDB", "Corrupt header skip during iteration")
{
    (void)ctx;
    TEST_ASSERT_RDB_OK(kv_reset(DEFAULT_WG));

    char     keys[CORRUPT_KEY_CNT][4];
    uint8_t  latest_val[CORRUPT_KEY_CNT][4];
    uint16_t latest_len[CORRUPT_KEY_CNT];
    uint32_t ds = kv_data_start(DEFAULT_WG);

    /* Insert initial records and identify the first record offset */
    for (int i = 0; i < CORRUPT_KEY_CNT; i++) {
        build_key_2d(keys[i], i);
        uint8_t v = (uint8_t)(0x10u + i);
        TEST_ASSERT_RDB_OK(rdb_kvdb_set(&g_db, keys[i], &v, 1));
    }

    /* Find first record in sector 0 and corrupt its header byte */
    uint32_t base = g_part.base_addr, off = ds, ss = g_part.sector_size;
    uint32_t first_addr = RDB_ADDR_INVALID;
    while (off + sizeof(rdb_kv_record_hdr_t) <= ss) {
        rdb_kv_record_hdr_t rh;
        sim_flash_read(&g_flash, base + off, (uint8_t*)&rh, sizeof(rh));
        if (rh.magic == 0xFFu && rh.state == 0xFFu) break;
        if (rh.magic == RDB_KV_RECORD_MAGIC && rh.key_len >= 1 &&
            rh.key_len <= RDB_MAX_KEY_LEN && rh.val_len <= RDB_MAX_VAL_LEN) {
            first_addr = base + off; break;
        }
        off += RDB_ALIGN_UP(sizeof(rdb_kv_record_hdr_t), 1u << DEFAULT_WG);
    }
    TEST_ASSERT_NE(first_addr, 0xFFFFFFFFu);
    { uint8_t z = 0x00; sim_flash_write(&g_flash, first_addr, &z, 1); }

    /* Write new data, force GC, then iterate and verify all keys present */
    for (int i = 0; i < CORRUPT_KEY_CNT; i++) {
        latest_val[i][0] = (uint8_t)(0x80u + i);
        latest_val[i][1] = (uint8_t)(0x90u + i);
        latest_val[i][2] = (uint8_t)(0xA0u + i);
        latest_val[i][3] = (uint8_t)(0xB0u + i);
        latest_len[i] = 4;
        TEST_ASSERT_RDB_OK(rdb_kvdb_set(&g_db, keys[i], latest_val[i], 4));
    }

    uint32_t loops = 0;
    while (g_db.stats.gc_runs < CORRUPT_GC_TGT && loops < CORRUPT_MAX_LOOPS) {
        for (int i = 0; i < CORRUPT_KEY_CNT; i++)
            TEST_ASSERT_RDB_OK(rdb_kvdb_set(&g_db, keys[i], latest_val[i], 4));
        loops++;
    }
    TEST_ASSERT_GE(g_db.stats.gc_runs, CORRUPT_GC_TGT);

    rdb_kv_iter_t it;
    TEST_ASSERT_RDB_OK(rdb_kv_iter_init(&it, &g_db));
    uint8_t seen[CORRUPT_KEY_CNT]; memset(seen, 0, sizeof(seen));
    char    kb[16]; uint8_t vb[16]; uint16_t kl, vl;

    while (rdb_kv_iter_next(&it, kb, sizeof(kb), vb, sizeof(vb), &kl, &vl) == RDB_OK) {
        int idx = key_index_2d(kb);
        TEST_ASSERT_GE(idx, 0);
        TEST_ASSERT_LT(idx, CORRUPT_KEY_CNT);
        TEST_ASSERT_EQ(seen[idx], 0);
        seen[idx] = 1;
        TEST_ASSERT_EQ(vl, latest_len[idx]);
        TEST_ASSERT_MEM_EQ(vb, latest_val[idx], latest_len[idx]);
    }
    for (int i = 0; i < CORRUPT_KEY_CNT; i++) TEST_ASSERT_EQ(seen[i], 1);
    return 0;
}

/* ═══════════════════════════════════════════════════════════════════════════
 *  TC-KV-01: Empty init / format verification
 *
 *  Verify active sector selection, write_off at data_start,
 *  and erase count increments across format cycles.
 * ═══════════════════════════════════════════════════════════════════════════ */

TEST_CASE(kv_init_format_verify, "KVDB", "Empty init and format verification")
{
    (void)ctx;

    /* Step 1: fresh 0xFF flash → init → verify one ACTIVE sector */
    TEST_ASSERT_RDB_OK(kv_reset(DEFAULT_WG));
    TEST_ASSERT_NE(g_db.active_sec, 0xFF);
    TEST_ASSERT_EQ(g_db.sectors[g_db.active_sec].status, RDB_SEC_ACTIVE);
    TEST_ASSERT_EQ(g_db.sectors[g_db.active_sec].write_off,
                   (uint16_t)kv_data_start(DEFAULT_WG));
    TEST_ASSERT_GE(g_db.write_seq, 1u);

    /* Step 2: verify erase counts — all sectors should be non-zero after format */
    for (uint8_t s = 0; s < KV_SECTOR_CNT; s++)
        TEST_ASSERT_GE(g_db.sectors[s].erase_cnt, 1u);

    /* Step 3: format again — erase counts should increment */
    uint32_t ec0[KV_SECTOR_CNT];
    for (uint8_t s = 0; s < KV_SECTOR_CNT; s++)
        ec0[s] = g_db.sectors[s].erase_cnt;

    TEST_ASSERT_RDB_OK(kv_reset(DEFAULT_WG));
    for (uint8_t s = 0; s < KV_SECTOR_CNT; s++)
        TEST_ASSERT_GT(g_db.sectors[s].erase_cnt, ec0[s]);

    /* Step 4: validate sector header written once per sector (write_seq per sector) */
    for (uint8_t s = 0; s < KV_SECTOR_CNT; s++) {
        if (g_db.sectors[s].status == RDB_SEC_ACTIVE ||
            g_db.sectors[s].status == RDB_SEC_SEALED)
            TEST_ASSERT_NE(g_db.sectors[s].create_seq, 0u);
    }

    /* Step 5: basic set/get works after init */
    uint8_t val = 0xAB;
    TEST_ASSERT_RDB_OK(rdb_kvdb_set(&g_db, "key", &val, 1));
    uint16_t out_len = 0; uint8_t out[1];
    TEST_ASSERT_RDB_OK(rdb_kvdb_get(&g_db, "key", out, 1, &out_len));
    TEST_ASSERT_EQ(out_len, 1u);
    TEST_ASSERT_EQ(out[0], 0xAB);
    return 0;
}

/* ═══════════════════════════════════════════════════════════════════════════
 *  TC-X-02 (KV part): Maximum key/value boundary test
 *
 *  Computes actual max value length from sector geometry (sector_size,
 *  write_gran, key length), then tests precise boundary: max_val succeeds,
 *  max_val+1 returns TOO_LARGE.  Also verifies max key length and zero-length.
 * ═══════════════════════════════════════════════════════════════════════════ */

static uint32_t kv_max_val_for_key(const rdb_kvdb_t *db, uint16_t key_len)
{
    uint32_t gran = 1u << db->part->write_gran;
    uint32_t ds   = RDB_ALIGN_UP((uint32_t)sizeof(rdb_kv_sector_hdr_t), gran);
    uint32_t cap  = db->part->sector_size - ds;
    uint32_t ka   = RDB_ALIGN_UP(key_len, gran);
    uint32_t va_max = cap - (uint32_t)sizeof(rdb_kv_record_hdr_t) - ka;
    return va_max - (va_max % gran);
}

TEST_CASE(kv_max_boundaries, "KVDB", "Maximum key/value boundary test")
{
    (void)ctx;
    TEST_ASSERT_RDB_OK(kv_reset(DEFAULT_WG));

    trace_kvdb_geometry(&g_trace, &g_db);

    /* ── Max key length (63) ── */
    {
        char mk[RDB_MAX_KEY_LEN + 2];
        memset(mk, 'K', RDB_MAX_KEY_LEN);
        mk[RDB_MAX_KEY_LEN] = '\0';
        TEST_ASSERT_RDB_OK(rdb_kvdb_set(&g_db, mk, "v", 1));

        uint8_t out[2]; uint16_t out_len = 0;
        TEST_ASSERT_RDB_OK(rdb_kvdb_get(&g_db, mk, out, sizeof(out), &out_len));
        TEST_ASSERT_EQ(out_len, 1u);
        TEST_ASSERT_EQ(out[0], 'v');
    }
    /* Key too long (64) → TOO_LARGE */
    {
        char ok[RDB_MAX_KEY_LEN + 3];
        memset(ok, 'K', RDB_MAX_KEY_LEN + 1u);
        ok[RDB_MAX_KEY_LEN + 1u] = '\0';
        TEST_ASSERT_RDB_ERR(rdb_kvdb_set(&g_db, ok, "v", 1), RDB_ERR_TOO_LARGE);
    }

    /* ── Max value length (computed from geometry) ── */
    {
        uint32_t max_val = kv_max_val_for_key(&g_db, 2); /* key="MK" = 2 bytes */
        TEST_ASSERT_GT(max_val, 0u);

        trace_event(&g_trace, "  [KV-MAX] computed max_val=%uB for key_len=2", max_val);

        /* Exact max_val — must succeed */
        uint8_t* mv = (uint8_t*)malloc(max_val);
        TEST_ASSERT(mv != NULL);
        memset(mv, 0xCC, max_val);
        TEST_ASSERT_RDB_OK(rdb_kvdb_set(&g_db, "MK", mv, (uint16_t)max_val));
        uint8_t* out = (uint8_t*)malloc(max_val);
        TEST_ASSERT(out != NULL);
        uint16_t out_len = 0;
        TEST_ASSERT_RDB_OK(rdb_kvdb_get(&g_db, "MK", out, max_val, &out_len));
        TEST_ASSERT_EQ(out_len, (uint16_t)max_val);
        TEST_ASSERT_MEM_EQ(out, mv, max_val);
        free(out);
        free(mv);

        /* max_val + 1 — must be TOO_LARGE */
        mv = (uint8_t*)malloc(max_val + 1u);
        TEST_ASSERT(mv != NULL);
        memset(mv, 0xDD, max_val + 1u);
        TEST_ASSERT_RDB_ERR(rdb_kvdb_set(&g_db, "OV", mv, (uint16_t)(max_val + 1u)),
                            RDB_ERR_TOO_LARGE);
        free(mv);
    }

    /* Value far too large (4096) → TOO_LARGE for any configuration */
    {
        uint8_t* ov = (uint8_t*)malloc(RDB_MAX_VAL_LEN + 1u);
        TEST_ASSERT(ov != NULL);
        TEST_ASSERT_RDB_ERR(rdb_kvdb_set(&g_db, "FV", ov,
            RDB_MAX_VAL_LEN + 1u), RDB_ERR_TOO_LARGE);
        free(ov);
    }

    /* ── Zero-length value ── */
    {
        TEST_ASSERT_RDB_OK(rdb_kvdb_set(&g_db, "ZV", NULL, 0));
        uint16_t out_len = 0xFFFF;
        TEST_ASSERT_RDB_OK(rdb_kvdb_get(&g_db, "ZV", NULL, 0, &out_len));
        TEST_ASSERT_EQ(out_len, 0u);
    }
    return 0;
}

/* ═══════════════════════════════════════════════════════════════════════════
 *  TC-X-03: Capacity accounting verification
 *
 *  Cross-check max_live and space_info against actual usage
 *  across a mixed set/delete workload.
 * ═══════════════════════════════════════════════════════════════════════════ */

TEST_CASE(kv_capacity_accounting, "KVDB", "Capacity accounting cross-check")
{
    (void)ctx;
    TEST_ASSERT_RDB_OK(kv_reset(DEFAULT_WG));

    uint32_t total = 0, used = 0, avail = 0;
    rdb_kvdb_space_info(&g_db, &total, &used, &avail);
    TEST_ASSERT_GT(total, 0u);
    TEST_ASSERT_EQ(avail, total); /* Nothing written yet */
    TEST_ASSERT_EQ(g_db.live_bytes, used);

    /* Write known-size records and verify accounting */
    char key[4] = { 'A', '0', '0', 0 };
    uint8_t val[64];
    memset(val, 0xDD, sizeof(val));
    for (int i = 0; i < 50; i++) {
        key[1] = (char)('0' + (i / 10));
        key[2] = (char)('0' + (i % 10));
        TEST_ASSERT_RDB_OK(rdb_kvdb_set(&g_db, key, val, (uint16_t)sizeof(val)));
    }

    rdb_kvdb_space_info(&g_db, &total, &used, &avail);
    TEST_ASSERT_EQ(used, g_db.live_bytes);
    TEST_ASSERT_EQ(avail, total - used);
    TEST_ASSERT_LE(used, total);

    /* Delete half the keys — live_bytes should decrease */
    uint32_t live_before = g_db.live_bytes;
    for (int i = 0; i < 25; i++) {
        key[1] = (char)('0' + (i / 10));
        key[2] = (char)('0' + (i % 10));
        TEST_ASSERT_RDB_OK(rdb_kvdb_delete(&g_db, key));
    }
    TEST_ASSERT_LT(g_db.live_bytes, live_before);

    rdb_kvdb_space_info(&g_db, NULL, &used, NULL);
    TEST_ASSERT_EQ(used, g_db.live_bytes);
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
        .log_file = fopen(test_make_log_path("kvdb_basic"), "w"),
        .verbose = 1, .stop_on_fail = 0, .filter = NULL,
        .post_test_hook = post_test_kvdb_sectors, .hook_ctx = NULL
    };
    test_framework_init(&config);

    trace_init(&g_trace, config.log_file, config.verbose);
    sim_flash_set_trace(&g_flash, &g_trace);
    trace_event(&g_trace, "=== KVDB Basic Test Suite Start ===");

    test_suite_t *s = test_get_default_suite();
    test_register_case(s, &test_case_kv_set_get_basic);
    test_register_case(s, &test_case_kv_set_update);
    test_register_case(s, &test_case_kv_delete_exists);
    test_register_case(s, &test_case_kv_get_too_large);
    test_register_case(s, &test_case_kv_write_gran_matrix);
    test_register_case(s, &test_case_kv_seq_wrap_recovery);
    test_register_case(s, &test_case_kv_mixed_lengths);
    test_register_case(s, &test_case_kv_corrupt_header_skip);
    test_register_case(s, &test_case_kv_init_format_verify);
    test_register_case(s, &test_case_kv_max_boundaries);
    test_register_case(s, &test_case_kv_capacity_accounting);

    test_run_all(NULL);

    trace_event(&g_trace, "=== KVDB Basic Test Suite End ===\n");
    trace_kvdb_stats(&g_trace, &g_db);

    test_print_report();
    if (config.log_file) fclose(config.log_file);

    test_stats_t stats; test_get_stats(&stats);
    return (stats.failed_cases == 0) ? 0 : 1;
}
