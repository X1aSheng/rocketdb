#include "sim_flash.h"
#include "sim_vectors.h"
#include <stdio.h>
#include <string.h>
#include <stdarg.h>
#include <time.h>

#include "../../src/rocketdb.h"

#define FLASH_SIZE      (128u * 1024u)
#define SECTOR_SIZE     4096u
#define PAGE_SIZE       256u
#define WRITE_GRAN      0u /* 1 byte */

#define PART_SIZE       (32u * 1024u)

static uint8_t g_flash_buf[FLASH_SIZE];
static sim_flash_t g_flash;

static FILE *g_log = NULL;

static void log_line(const char *fmt, ...)
{
    if (!g_log) return;
    va_list ap;
    va_start(ap, fmt);
    vfprintf(g_log, fmt, ap);
    fprintf(g_log, "\n");
    va_end(ap);
    fflush(g_log);
}

static int fl_read(uint32_t addr, uint8_t *buf, size_t len)
{
    return sim_flash_read(&g_flash, addr, buf, len);
}

static int fl_write(uint32_t addr, const uint8_t *buf, size_t len)
{
    return sim_flash_write(&g_flash, addr, buf, len);
}

static int fl_erase(uint32_t addr)
{
    return sim_flash_erase(&g_flash, addr);
}

static void fl_lock(void) { }
static void fl_unlock(void) { }
static void fl_yield(void) { }

static rdb_flash_ops_t g_ops = {
    .read = fl_read,
    .write = fl_write,
    .erase = fl_erase,
    .lock = fl_lock,
    .unlock = fl_unlock,
    .yield = fl_yield
};

static int kv_basic_set_get(rdb_kvdb_t *db)
{
    const char *k = "K00";
    const uint8_t v[8] = {1,2,3,4,5,6,7,8};
    log_line("[KV] basic set/get start");
    if (rdb_kvdb_set(db, k, v, sizeof(v)) != RDB_OK) return -1;

    uint8_t out[8];
    uint16_t out_len = 0;
    if (rdb_kvdb_get(db, k, out, sizeof(out), &out_len) != RDB_OK) return -1;
    if (out_len != sizeof(v)) return -1;
    if (memcmp(out, v, sizeof(v)) != 0) return -1;
    log_line("[KV] basic set/get ok");
    return 0;
}

static int kv_gc_stress(rdb_kvdb_t *db, uint32_t target_gc)
{
    char key[4] = { 'K', '0', '0', 0 };
    uint8_t val[32];
    memset(val, 0xA5, sizeof(val));

    uint32_t loops = 0;
    log_line("[KV] gc stress start target=%u", target_gc);
    while (db->stats.gc_runs < target_gc && loops < 200000u) {
        for (int i = 0; i < 20; i++) {
            key[1] = (char)('0' + (i / 10));
            key[2] = (char)('0' + (i % 10));
            if (rdb_kvdb_set(db, key, val, sizeof(val)) != RDB_OK)
                return -1;
        }
        loops++;
        if ((loops % 1000u) == 0) {
            log_line("[KV] gc progress loops=%u gc_runs=%u",
                     loops, db->stats.gc_runs);
        }
    }
    log_line("[KV] gc stress done loops=%u gc_runs=%u",
             loops, db->stats.gc_runs);
    return (db->stats.gc_runs >= target_gc) ? 0 : -1;
}

static uint32_t g_ts_count = 0;

static int ts_count_cb(uint32_t t, const void *d, uint16_t len, void *arg)
{
    (void)t; (void)d; (void)len; (void)arg;
    g_ts_count++;
    return RDB_ITER_CONTINUE;
}

static int ts_basic_append_query(rdb_tsdb_t *db)
{
    uint8_t data[16];
    memset(data, 0x3C, sizeof(data));

    log_line("[TS] append/query start");
    for (uint32_t i = 1; i <= 200; i++) {
        if (rdb_tsdb_append(db, i, data, sizeof(data)) != RDB_OK)
            return -1;
    }

    g_ts_count = 0;
    if (rdb_tsdb_query(db, 1, 0, ts_count_cb, NULL) != RDB_OK) return -1;
    log_line("[TS] append/query done count=%u", g_ts_count);
    return (g_ts_count >= 200) ? 0 : -1;
}

int main(void)
{
    time_t now = time(NULL);
    g_log = fopen("test\\out\\sim_log.txt", "wb");
    if (!g_log) {
        printf("log open failed\n");
        return 1;
    }
    log_line("RocketDB sim start");
    log_line("timestamp=%u", (unsigned)now);
    log_line("flash size=%u sector=%u page=%u write_gran=%u",
             FLASH_SIZE, SECTOR_SIZE, PAGE_SIZE, WRITE_GRAN);

    if (sim_flash_init(&g_flash, g_flash_buf, FLASH_SIZE,
                       SECTOR_SIZE, PAGE_SIZE, WRITE_GRAN) != 0) {
        log_line("flash init failed");
        return 1;
    }
    log_line("flash init ok");

    rdb_partition_t kv1 = {
        .name = "KVDB1",
        .base_addr = 0x00000,
        .total_size = PART_SIZE,
        .sector_size = SECTOR_SIZE,
        .write_gran = WRITE_GRAN,
        .ops = &g_ops
    };

    rdb_partition_t ts1 = {
        .name = "TSDB1",
        .base_addr = 0x10000,
        .total_size = PART_SIZE,
        .sector_size = SECTOR_SIZE,
        .write_gran = WRITE_GRAN,
        .ops = &g_ops
    };

    uint8_t kv_meta[rdb_kvdb_meta_size(8)];
    uint32_t ts_ec[rdb_tsdb_ec_size(8) / sizeof(uint32_t)];

    rdb_kvdb_t kvdb;
    rdb_tsdb_t tsdb;

    if (rdb_kvdb_init(&kvdb, &kv1, kv_meta) != RDB_OK) {
        log_line("kvdb init failed");
        return 1;
    }
    log_line("kvdb init ok");

    if (rdb_tsdb_init(&tsdb, &ts1, ts_ec) != RDB_OK) {
        log_line("tsdb init failed");
        return 1;
    }
    log_line("tsdb init ok");

    if (kv_basic_set_get(&kvdb) != 0) {
        log_line("kv basic test failed");
        return 1;
    }

    if (kv_gc_stress(&kvdb, 100) != 0) {
        log_line("kv gc stress failed gc_runs=%u", kvdb.stats.gc_runs);
        return 1;
    }

    if (ts_basic_append_query(&tsdb) != 0) {
        log_line("ts basic test failed");
        return 1;
    }

    if (rdb_vec_generate_kv("test\\out\\kv_vectors.bin", 0xC0FFEEu, 2000,
                            RDB_MAX_KEY_LEN, RDB_MAX_VAL_LEN) != 0) {
        log_line("kv vector generation failed");
        return 1;
    }
    log_line("kv vectors generated");

    if (rdb_vec_generate_ts("test\\out\\ts_vectors.bin", 0xC0FFEEu, 2000,
                            (uint16_t)tsdb.max_data_len) != 0) {
        log_line("ts vector generation failed");
        return 1;
    }
    log_line("ts vectors generated");

    log_line("sim completed");
    if (g_log) fclose(g_log);
    g_log = NULL;
    printf("sim completed\n");
    return 0;
}
