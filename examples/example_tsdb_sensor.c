#include "rocketdb.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define EXAMPLE_SECTOR_SIZE 4096u
#define EXAMPLE_SECTOR_COUNT 8u
#define EXAMPLE_FLASH_SIZE (EXAMPLE_SECTOR_SIZE * EXAMPLE_SECTOR_COUNT)

typedef struct {
    int16_t temperature_c_x100;
    uint16_t humidity_x100;
} sensor_sample_t;

typedef struct {
    uint32_t count;
    int32_t temperature_sum;
} query_summary_t;

static uint8_t g_flash[EXAMPLE_FLASH_SIZE];

uint16_t rdb_crc16_cont(uint16_t crc, const void *data, size_t len)
{
    const uint8_t *p = (const uint8_t *)data;
    size_t i;
    uint8_t bit;

    for (i = 0; i < len; ++i) {
        crc ^= p[i];
        for (bit = 0; bit < 8; ++bit) {
            if (crc & 1u) {
                crc = (uint16_t)((crc >> 1) ^ 0xA001u);
            } else {
                crc >>= 1;
            }
        }
    }

    return crc;
}

uint16_t rdb_crc16(const void *data, size_t len)
{
    return rdb_crc16_cont(0xFFFFu, data, len);
}

uint16_t rdb_hash16(const void *data, size_t len)
{
    const uint8_t *p = (const uint8_t *)data;
    uint32_t hash = 2166136261u;
    size_t i;

    for (i = 0; i < len; ++i) {
        hash ^= p[i];
        hash *= 16777619u;
    }

    return (uint16_t)(hash ^ (hash >> 16));
}

static int flash_read(uint32_t addr, uint8_t *buf, size_t len)
{
    if ((buf == NULL && len > 0u) || addr > EXAMPLE_FLASH_SIZE ||
        len > (EXAMPLE_FLASH_SIZE - addr)) {
        return -1;
    }

    memcpy(buf, &g_flash[addr], len);
    return 0;
}

static int flash_write(uint32_t addr, const uint8_t *buf, size_t len)
{
    size_t i;

    if ((buf == NULL && len > 0u) || addr > EXAMPLE_FLASH_SIZE ||
        len > (EXAMPLE_FLASH_SIZE - addr)) {
        return -1;
    }

    for (i = 0; i < len; ++i) {
        if ((uint8_t)(~g_flash[addr + i]) & buf[i]) {
            return -1;
        }
    }

    for (i = 0; i < len; ++i) {
        g_flash[addr + i] &= buf[i];
    }

    return 0;
}

static int flash_erase(uint32_t addr)
{
    if (addr > EXAMPLE_FLASH_SIZE ||
        EXAMPLE_SECTOR_SIZE > (EXAMPLE_FLASH_SIZE - addr) ||
        (addr % EXAMPLE_SECTOR_SIZE) != 0u) {
        return -1;
    }

    memset(&g_flash[addr], 0xFF, EXAMPLE_SECTOR_SIZE);
    return 0;
}

static void flash_lock(void)
{
}

static void flash_unlock(void)
{
}

static void flash_yield(void)
{
}

static const rdb_flash_ops_t flash_ops = {
    flash_read,
    flash_write,
    flash_erase,
    flash_lock,
    flash_unlock,
    flash_yield,
};

static int collect_sample(uint32_t ts, const void *data, uint16_t len, void *arg)
{
    query_summary_t *summary = (query_summary_t *)arg;
    const sensor_sample_t *sample = (const sensor_sample_t *)data;

    (void)ts;

    if (len != sizeof(sensor_sample_t)) {
        return -1;
    }

    summary->count++;
    summary->temperature_sum += sample->temperature_c_x100;
    return 0;
}

static int require_ok(rdb_err_t status, const char *step)
{
    if (status != RDB_OK) {
        printf("%s failed: %d\n", step, (int)status);
        return 1;
    }
    return 0;
}

int main(void)
{
    rdb_partition_t part;
    rdb_tsdb_t db;
    uint32_t erase_counts[EXAMPLE_SECTOR_COUNT];
    uint32_t i;
    uint32_t oldest = 0u;
    uint32_t newest = 0u;
    query_summary_t summary;

    memset(g_flash, 0xFF, sizeof(g_flash));
    memset(&part, 0, sizeof(part));
    memset(&db, 0, sizeof(db));
    memset(erase_counts, 0, sizeof(erase_counts));
    memset(&summary, 0, sizeof(summary));

    part.name = "example-ts";
    part.base_addr = 0u;
    part.total_size = EXAMPLE_FLASH_SIZE;
    part.sector_size = EXAMPLE_SECTOR_SIZE;
    part.write_gran = 1u;
    part.ops = &flash_ops;

    db.part = &part;
    db.erase_cnts = erase_counts;
    db.sector_cnt = EXAMPLE_SECTOR_COUNT;
    if (require_ok(rdb_tsdb_format(&db), "tsdb format") != 0) {
        return 1;
    }
    if (require_ok(rdb_tsdb_init(&db, &part, erase_counts), "tsdb init") != 0) {
        return 1;
    }

    for (i = 0; i < 16u; ++i) {
        sensor_sample_t sample;
        sample.temperature_c_x100 = (int16_t)(2250 + (int16_t)i * 5);
        sample.humidity_x100 = (uint16_t)(5000u + i * 10u);

        if (require_ok(rdb_tsdb_append(&db, 1000u + i, &sample, sizeof(sample)),
                       "append sample") != 0) {
            return 1;
        }
    }

    if (require_ok(rdb_tsdb_query(&db, 1004u, 1011u, collect_sample, &summary),
                   "query samples") != 0) {
        return 1;
    }
    rdb_tsdb_time_range(&db, &oldest, &newest);
    if (summary.count != 8u || oldest != 1000u || newest != 1015u) {
        printf("unexpected query result: count=%u oldest=%u newest=%u\n",
               (unsigned)summary.count, (unsigned)oldest, (unsigned)newest);
        return 1;
    }

    printf("TSDB example passed: queried=%u average_c_x100=%d\n",
           (unsigned)summary.count,
           (int)(summary.temperature_sum / (int32_t)summary.count));
    return 0;
}
