/**
 * @file scenarios.c
 * @brief RocketDB performance benchmark scenarios.
 */

#include "perf_benchmark.h"
#include "../../src/rocketdb.h"
#include "../sim/sim_flash.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define PERF_FLASH_SIZE  (512u * 1024u)
#define PERF_SECTOR_SIZE 4096u
#define PERF_PAGE_SIZE   256u
#define PERF_WRITE_GRAN  0u
#define PERF_SECTOR_CNT  (PERF_FLASH_SIZE / PERF_SECTOR_SIZE)

#define PERF_KV_PART_SIZE (256u * 1024u)
#define PERF_TS_PART_SIZE (256u * 1024u)
#define PERF_PART_SECTORS (PERF_KV_PART_SIZE / PERF_SECTOR_SIZE)

static uint8_t g_flash_buf[PERF_FLASH_SIZE];
static sim_flash_t g_flash;
static rdb_kv_sector_meta_t g_kv_meta[PERF_SECTOR_CNT];
static rdb_kv_sector_meta_t g_mixed_kv_meta[PERF_PART_SECTORS];
static uint32_t g_ts_ec[PERF_SECTOR_CNT];
static uint32_t g_mixed_ts_ec[PERF_PART_SECTORS];
static uint32_t g_query_count;

static int perf_read(void *ctx, uint32_t addr, uint8_t *buf, size_t len)
{
    (void)ctx;
    return sim_flash_read(&g_flash, addr, buf, len);
}

static int perf_write(void *ctx, uint32_t addr, const uint8_t *buf, size_t len)
{
    (void)ctx;
    return sim_flash_write(&g_flash, addr, buf, len);
}

static int perf_erase(void *ctx, uint32_t addr)
{
    (void)ctx;
    return sim_flash_erase(&g_flash, addr);
}

static void perf_lock(void *ctx)
{
    (void)ctx;
}

static void perf_unlock(void *ctx)
{
    (void)ctx;
}

static void perf_yield(void *ctx)
{
    (void)ctx;
}

static const rdb_flash_ops_t g_ops = {
    perf_read,
    perf_write,
    perf_erase,
    perf_lock,
    perf_unlock,
    perf_yield,
};

static int query_count_cb(uint32_t ts, const void *data, uint16_t len, void *arg)
{
    (void)ts;
    (void)data;
    (void)len;
    (void)arg;
    g_query_count++;
    return RDB_ITER_CONTINUE;
}

static int flash_reset(void)
{
    return sim_flash_init(&g_flash, g_flash_buf, PERF_FLASH_SIZE,
                          PERF_SECTOR_SIZE, PERF_PAGE_SIZE, PERF_WRITE_GRAN);
}

static rdb_partition_t make_part(const char *name, uint32_t base, uint32_t size)
{
    rdb_partition_t part;

    part.name = name;
    part.base_addr = base;
    part.total_size = size;
    part.sector_size = PERF_SECTOR_SIZE;
    part.write_gran = PERF_WRITE_GRAN;
    part.ops = &g_ops;
    return part;
}

static int kv_open(rdb_kvdb_t *db, rdb_partition_t *part, void *meta_buf,
                   const char *name, uint32_t base, uint32_t size)
{
    rdb_err_t ret;

    memset(db, 0, sizeof(*db));
    *part = make_part(name, base, size);
    db->part = part;
    db->sectors = (rdb_kv_sector_meta_t *)meta_buf;
    db->sector_cnt = (uint8_t)(size / PERF_SECTOR_SIZE);
    ret = rdb_kvdb_format(db);
    if (ret != RDB_OK) {
        return ret;
    }
    return rdb_kvdb_init(db, part, meta_buf);
}

static int ts_open(rdb_tsdb_t *db, rdb_partition_t *part, uint32_t *ec_buf,
                   const char *name, uint32_t base, uint32_t size)
{
    rdb_err_t ret;

    memset(db, 0, sizeof(*db));
    *part = make_part(name, base, size);
    db->part = part;
    db->erase_cnts = ec_buf;
    db->sector_cnt = (uint8_t)(size / PERF_SECTOR_SIZE);
    ret = rdb_tsdb_format(db);
    if (ret != RDB_OK) {
        return ret;
    }
    return rdb_tsdb_init(db, part, ec_buf);
}

static int add_elapsed_sample(perf_stats_t *stats, perf_timer_t *timer)
{
    uint64_t elapsed_ns = perf_elapsed_ns(timer);
    uint64_t elapsed_us = (elapsed_ns + 999u) / 1000u;

    if (stats == NULL) {
        return -1;
    }
    if (elapsed_us == 0u) {
        elapsed_us = 1u;
    }
    perf_stats_add_sample(stats, elapsed_us);
    return 0;
}

static int print_and_free(perf_stats_t *stats)
{
    if (stats == NULL) {
        return -1;
    }
    perf_stats_calculate(stats);
    perf_stats_print(stats);
    perf_stats_free(stats);
    return 0;
}

static int scenario_p100_basic_set_get(void)
{
    rdb_kvdb_t db;
    rdb_partition_t part;
    perf_timer_t timer;
    perf_stats_t *stats_set;
    perf_stats_t *stats_get;
    int i;

    if (flash_reset() != 0 ||
        kv_open(&db, &part, g_kv_meta, "perf-kv", 0u, PERF_FLASH_SIZE) != RDB_OK) {
        return -1;
    }

    stats_set = perf_stats_create("P-100", "kv_set_small", 1000u);
    stats_get = perf_stats_create("P-100", "kv_get_small", 1000u);
    if (stats_set == NULL || stats_get == NULL) {
        perf_stats_free(stats_set);
        perf_stats_free(stats_get);
        return -1;
    }

    for (i = 0; i < 1000; ++i) {
        char key[24];
        char val[40];

        snprintf(key, sizeof(key), "key_%05d", i);
        snprintf(val, sizeof(val), "value_%05d_test", i);
        perf_start(&timer);
        if (rdb_kvdb_set(&db, key, val, (uint16_t)(strlen(val) + 1u)) != RDB_OK) {
            return -1;
        }
        perf_stop(&timer);
        add_elapsed_sample(stats_set, &timer);
    }

    for (i = 0; i < 1000; ++i) {
        char key[24];
        char buf[40];
        uint16_t out_len = 0u;

        snprintf(key, sizeof(key), "key_%05d", i);
        perf_start(&timer);
        if (rdb_kvdb_get(&db, key, buf, sizeof(buf), &out_len) != RDB_OK) {
            return -1;
        }
        perf_stop(&timer);
        add_elapsed_sample(stats_get, &timer);
    }

    return print_and_free(stats_set) || print_and_free(stats_get);
}

static int scenario_p101_gc_stress(void)
{
    rdb_kvdb_t db;
    rdb_partition_t part;
    perf_timer_t timer;
    perf_stats_t *stats;
    int cycle;

    if (flash_reset() != 0 ||
        kv_open(&db, &part, g_kv_meta, "perf-kv-gc", 0u, PERF_FLASH_SIZE) != RDB_OK) {
        return -1;
    }

    stats = perf_stats_create("P-101", "kv_update_gc_cycle", 200u);
    if (stats == NULL) {
        return -1;
    }

    for (cycle = 0; cycle < 200; ++cycle) {
        int i;

        perf_start(&timer);
        for (i = 0; i < 16; ++i) {
            char key[24];
            char val[96];

            snprintf(key, sizeof(key), "gc_key_%02d", i);
            snprintf(val, sizeof(val), "cycle=%03d key=%02d padding-for-gc-stress", cycle, i);
            if (rdb_kvdb_set(&db, key, val, (uint16_t)(strlen(val) + 1u)) != RDB_OK) {
                return -1;
            }
        }
        if ((cycle % 8) == 7 && rdb_kvdb_gc(&db) != RDB_OK) {
            return -1;
        }
        perf_stop(&timer);
        add_elapsed_sample(stats, &timer);
    }

    return print_and_free(stats);
}

static int scenario_p102_large_value(void)
{
    rdb_kvdb_t db;
    rdb_partition_t part;
    uint8_t large_val[2048];
    perf_timer_t timer;
    perf_stats_t *stats;
    int i;

    if (flash_reset() != 0 ||
        kv_open(&db, &part, g_kv_meta, "perf-kv-large", 0u, PERF_FLASH_SIZE) != RDB_OK) {
        return -1;
    }

    memset(large_val, 'X', sizeof(large_val));
    stats = perf_stats_create("P-102", "kv_set_2kb", 100u);
    if (stats == NULL) {
        return -1;
    }

    for (i = 0; i < 100; ++i) {
        char key[24];

        snprintf(key, sizeof(key), "large_%04d", i);
        perf_start(&timer);
        if (rdb_kvdb_set(&db, key, large_val, sizeof(large_val)) != RDB_OK) {
            return -1;
        }
        perf_stop(&timer);
        add_elapsed_sample(stats, &timer);
    }

    return print_and_free(stats);
}

static int scenario_p103_random_access(void)
{
    rdb_kvdb_t db;
    rdb_partition_t part;
    perf_timer_t timer;
    perf_stats_t *stats;
    int i;

    if (flash_reset() != 0 ||
        kv_open(&db, &part, g_kv_meta, "perf-kv-random", 0u, PERF_FLASH_SIZE) != RDB_OK) {
        return -1;
    }

    for (i = 0; i < 100; ++i) {
        char key[24];
        char val[32];

        snprintf(key, sizeof(key), "rand_%03d", i);
        snprintf(val, sizeof(val), "value_%03d", i);
        if (rdb_kvdb_set(&db, key, val, (uint16_t)(strlen(val) + 1u)) != RDB_OK) {
            return -1;
        }
    }

    srand(1u);
    stats = perf_stats_create("P-103", "kv_random_get", 1000u);
    if (stats == NULL) {
        return -1;
    }

    for (i = 0; i < 1000; ++i) {
        char key[24];
        char buf[32];
        uint16_t out_len = 0u;

        snprintf(key, sizeof(key), "rand_%03d", rand() % 100);
        perf_start(&timer);
        if (rdb_kvdb_get(&db, key, buf, sizeof(buf), &out_len) != RDB_OK) {
            return -1;
        }
        perf_stop(&timer);
        add_elapsed_sample(stats, &timer);
    }

    return print_and_free(stats);
}

static int scenario_p104_tsdb_append(void)
{
    rdb_tsdb_t db;
    rdb_partition_t part;
    perf_timer_t timer;
    perf_stats_t *stats;
    int i;

    if (flash_reset() != 0 ||
        ts_open(&db, &part, g_ts_ec, "perf-ts", 0u, PERF_FLASH_SIZE) != RDB_OK) {
        return -1;
    }

    stats = perf_stats_create("P-104", "tsdb_append", 1000u);
    if (stats == NULL) {
        return -1;
    }

    for (i = 0; i < 1000; ++i) {
        float value = 25.5f + (float)(i % 10);

        perf_start(&timer);
        if (rdb_tsdb_append(&db, 1000000u + (uint32_t)i, &value, sizeof(value)) != RDB_OK) {
            return -1;
        }
        perf_stop(&timer);
        add_elapsed_sample(stats, &timer);
    }

    return print_and_free(stats);
}

static int scenario_p105_tsdb_query(void)
{
    rdb_tsdb_t db;
    rdb_partition_t part;
    perf_timer_t timer;
    perf_stats_t *stats;
    int i;

    if (flash_reset() != 0 ||
        ts_open(&db, &part, g_ts_ec, "perf-ts-query", 0u, PERF_FLASH_SIZE) != RDB_OK) {
        return -1;
    }

    for (i = 0; i < 1000; ++i) {
        float value = 25.5f + (float)(i % 10);

        if (rdb_tsdb_append(&db, 1000000u + (uint32_t)i, &value, sizeof(value)) != RDB_OK) {
            return -1;
        }
    }

    stats = perf_stats_create("P-105", "tsdb_query_range", 100u);
    if (stats == NULL) {
        return -1;
    }

    for (i = 0; i < 100; ++i) {
        uint32_t start = 1000000u + (uint32_t)(i * 5);
        uint32_t end = start + 100u;

        g_query_count = 0u;
        perf_start(&timer);
        if (rdb_tsdb_query(&db, start, end, query_count_cb, NULL) != RDB_OK) {
            return -1;
        }
        perf_stop(&timer);
        add_elapsed_sample(stats, &timer);
    }

    return print_and_free(stats);
}

static int scenario_p106_mixed_workload(void)
{
    rdb_kvdb_t kvdb;
    rdb_tsdb_t tsdb;
    rdb_partition_t kv_part;
    rdb_partition_t ts_part;
    perf_timer_t timer;
    perf_stats_t *stats;
    int i;

    if (flash_reset() != 0 ||
        kv_open(&kvdb, &kv_part, g_mixed_kv_meta,
                "perf-mixed-kv", 0u, PERF_KV_PART_SIZE) != RDB_OK ||
        ts_open(&tsdb, &ts_part, g_mixed_ts_ec,
                "perf-mixed-ts", PERF_KV_PART_SIZE, PERF_TS_PART_SIZE) != RDB_OK) {
        return -1;
    }

    srand(2u);
    stats = perf_stats_create("P-106", "mixed_operation", 500u);
    if (stats == NULL) {
        return -1;
    }

    for (i = 0; i < 500; ++i) {
        int op_type = rand() % 100;

        perf_start(&timer);
        if (op_type < 50) {
            char key[24];
            char val[40];

            snprintf(key, sizeof(key), "mixed_%04d", i);
            snprintf(val, sizeof(val), "value_%04d", i);
            if (rdb_kvdb_set(&kvdb, key, val, (uint16_t)(strlen(val) + 1u)) != RDB_OK) {
                return -1;
            }
        } else if (op_type < 80) {
            float temp = 20.0f + (float)(i % 10);

            if (rdb_tsdb_append(&tsdb, 2000000u + (uint32_t)i,
                                &temp, sizeof(temp)) != RDB_OK) {
                return -1;
            }
        } else {
            uint32_t start = 2000000u + (uint32_t)(i / 5);

            g_query_count = 0u;
            if (rdb_tsdb_query(&tsdb, start, start + 50u,
                               query_count_cb, NULL) != RDB_OK) {
                return -1;
            }
        }
        perf_stop(&timer);
        add_elapsed_sample(stats, &timer);
    }

    return print_and_free(stats);
}

int main(void)
{
    printf("scenario_id,operation,count,avg_us,min_us,max_us,p95_us,throughput_ops_per_sec\n");

    if (scenario_p100_basic_set_get() != 0 ||
        scenario_p101_gc_stress() != 0 ||
        scenario_p102_large_value() != 0 ||
        scenario_p103_random_access() != 0 ||
        scenario_p104_tsdb_append() != 0 ||
        scenario_p105_tsdb_query() != 0 ||
        scenario_p106_mixed_workload() != 0) {
        fprintf(stderr, "benchmark failed\n");
        return 1;
    }

    return 0;
}
