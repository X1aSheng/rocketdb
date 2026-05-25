#include "rocketdb.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define EXAMPLE_SECTOR_SIZE 4096u
#define EXAMPLE_SECTOR_COUNT 8u
#define EXAMPLE_FLASH_SIZE (EXAMPLE_SECTOR_SIZE * EXAMPLE_SECTOR_COUNT)
#define EXAMPLE_PAGE_SIZE 256u
#define SAMPLE_COUNT 24u

typedef struct {
    int16_t temperature_c_x100;
    uint16_t humidity_x100;
    uint16_t battery_mv;
} sensor_sample_t;

typedef struct {
    uint32_t count;
    int32_t temp_sum;
    int16_t temp_min;
    int16_t temp_max;
    uint16_t humidity_max;
    uint8_t bad_records;
} query_summary_t;

static uint8_t g_flash[EXAMPLE_FLASH_SIZE];
static uint32_t g_erase_count[EXAMPLE_SECTOR_COUNT];

uint16_t rdb_crc16_cont(uint16_t crc, const void *data, size_t len)
{
    const uint8_t *p = (const uint8_t *)data;
    size_t i;
    uint8_t bit;

    if (p == NULL && len != 0u) {
        return crc;
    }

    for (i = 0u; i < len; ++i) {
        crc ^= p[i];
        for (bit = 0u; bit < 8u; ++bit) {
            if ((crc & 1u) != 0u) {
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

    if (p == NULL && len != 0u) {
        return 0u;
    }

    for (i = 0u; i < len; ++i) {
        hash ^= p[i];
        hash *= 16777619u;
    }

    return (uint16_t)(hash ^ (hash >> 16));
}

static int flash_read(void *ctx, uint32_t addr, uint8_t *buf, size_t len)
{
    (void)ctx;
    if ((buf == NULL && len != 0u) || addr > EXAMPLE_FLASH_SIZE ||
        len > (EXAMPLE_FLASH_SIZE - addr)) {
        return -1;
    }

    memcpy(buf, &g_flash[addr], len);
    return 0;
}

static int flash_write(void *ctx, uint32_t addr, const uint8_t *buf, size_t len)
{
    (void)ctx;
    if ((buf == NULL && len != 0u) || addr > EXAMPLE_FLASH_SIZE ||
        len > (EXAMPLE_FLASH_SIZE - addr)) {
        return -1;
    }

    while (len != 0u) {
        size_t page_room = EXAMPLE_PAGE_SIZE - (addr % EXAMPLE_PAGE_SIZE);
        size_t chunk = (len < page_room) ? len : page_room;
        size_t i;

        for (i = 0u; i < chunk; ++i) {
            if (((uint8_t)(~g_flash[addr + i]) & buf[i]) != 0u) {
                return -1;
            }
        }

        for (i = 0u; i < chunk; ++i) {
            g_flash[addr + i] &= buf[i];
        }

        addr += (uint32_t)chunk;
        buf += chunk;
        len -= chunk;
    }

    return 0;
}

static int flash_erase(void *ctx, uint32_t addr)
{
    (void)ctx;
    uint32_t sector;

    if (addr > EXAMPLE_FLASH_SIZE ||
        EXAMPLE_SECTOR_SIZE > (EXAMPLE_FLASH_SIZE - addr) ||
        (addr % EXAMPLE_SECTOR_SIZE) != 0u) {
        return -1;
    }

    sector = addr / EXAMPLE_SECTOR_SIZE;
    memset(&g_flash[addr], 0xFF, EXAMPLE_SECTOR_SIZE);
    g_erase_count[sector]++;
    return 0;
}

static void flash_lock(void *ctx)
{
    (void)ctx;
}

static void flash_unlock(void *ctx)
{
    (void)ctx;
}

static void flash_yield(void *ctx)
{
    (void)ctx;
}

static const rdb_flash_ops_t flash_ops = {
    .read = flash_read,
    .write = flash_write,
    .erase = flash_erase,
    .lock = flash_lock,
    .unlock = flash_unlock,
    .yield = flash_yield,
};

static int require_ok(rdb_err_t status, const char *step)
{
    if (status != RDB_OK) {
        printf("%s failed: %d\n", step, (int)status);
        return 1;
    }

    return 0;
}

static void fill_partition(rdb_partition_t *part)
{
    memset(part, 0, sizeof(*part));
    part->name = "tsdb-sensor";
    part->base_addr = 0u;
    part->total_size = EXAMPLE_FLASH_SIZE;
    part->sector_size = EXAMPLE_SECTOR_SIZE;
    part->write_gran = 0u;
    part->ops = &flash_ops;
    part->flash_ctx = NULL;
}

static rdb_err_t open_clean_tsdb(rdb_tsdb_t *db,
                                 const rdb_partition_t *part,
                                 uint32_t *erase_counts)
{
    memset(db, 0, sizeof(*db));
    memset(erase_counts, 0, sizeof(uint32_t) * EXAMPLE_SECTOR_COUNT);

    db->part = part;
    db->erase_cnts = erase_counts;
    db->sector_cnt = EXAMPLE_SECTOR_COUNT;

    if (rdb_tsdb_format(db) != RDB_OK) {
        return RDB_ERR_FLASH;
    }

    return rdb_tsdb_init(db, part, erase_counts);
}

static sensor_sample_t make_sample(uint32_t index)
{
    sensor_sample_t sample;

    sample.temperature_c_x100 = (int16_t)(2150 + (int16_t)((index * 7u) % 95u));
    sample.humidity_x100 = (uint16_t)(4650u + ((index * 13u) % 420u));
    sample.battery_mv = (uint16_t)(3300u - (index % 30u));

    return sample;
}

static int collect_sample(uint32_t ts, const void *data, uint16_t len, void *arg)
{
    query_summary_t *summary = (query_summary_t *)arg;
    const sensor_sample_t *sample = (const sensor_sample_t *)data;

    (void)ts;

    if (sample == NULL || len != sizeof(*sample)) {
        summary->bad_records++;
        return RDB_ITER_CONTINUE;
    }

    if (summary->count == 0u) {
        summary->temp_min = sample->temperature_c_x100;
        summary->temp_max = sample->temperature_c_x100;
        summary->humidity_max = sample->humidity_x100;
    } else {
        if (sample->temperature_c_x100 < summary->temp_min) {
            summary->temp_min = sample->temperature_c_x100;
        }
        if (sample->temperature_c_x100 > summary->temp_max) {
            summary->temp_max = sample->temperature_c_x100;
        }
        if (sample->humidity_x100 > summary->humidity_max) {
            summary->humidity_max = sample->humidity_x100;
        }
    }

    summary->count++;
    summary->temp_sum += sample->temperature_c_x100;
    return RDB_ITER_CONTINUE;
}

int main(void)
{
    rdb_partition_t part;
    rdb_tsdb_t db;
    uint32_t erase_counts[EXAMPLE_SECTOR_COUNT];
    rdb_ts_stats_t stats;
    query_summary_t summary;
    sensor_sample_t oldest_sample;
    sensor_sample_t latest_sample;
    uint32_t oldest_time;
    uint32_t latest_time;
    uint32_t range_oldest;
    uint32_t range_newest;
    uint32_t min_ec;
    uint32_t max_ec;
    uint32_t avg_ec;
    uint16_t out_len;
    uint32_t i;
    const uint32_t base_time = 1700000000u;

    memset(g_flash, 0xFF, sizeof(g_flash));
    memset(g_erase_count, 0, sizeof(g_erase_count));
    memset(&summary, 0, sizeof(summary));
    memset(&oldest_sample, 0, sizeof(oldest_sample));
    memset(&latest_sample, 0, sizeof(latest_sample));
    fill_partition(&part);

    if (require_ok(open_clean_tsdb(&db, &part, erase_counts), "open TSDB") != 0) {
        return 1;
    }

    for (i = 0u; i < SAMPLE_COUNT; ++i) {
        sensor_sample_t sample = make_sample(i);
        uint32_t timestamp = base_time + (i * 60u);

        if (require_ok(rdb_tsdb_append(&db, timestamp, &sample, sizeof(sample)),
                       "append sample") != 0) {
            return 1;
        }
    }

    if (require_ok(rdb_tsdb_query(&db, base_time + 300u, base_time + 900u,
                                  collect_sample, &summary),
                   "query samples") != 0) {
        return 1;
    }

    if (summary.count != 11u || summary.bad_records != 0u) {
        printf("unexpected query summary: count=%u bad=%u\n",
               (unsigned)summary.count, (unsigned)summary.bad_records);
        return 1;
    }

    out_len = 0u;
    if (require_ok(rdb_tsdb_get_oldest(&db, &oldest_time, &oldest_sample,
                                       sizeof(oldest_sample), &out_len),
                   "get oldest") != 0) {
        return 1;
    }
    if (out_len != sizeof(oldest_sample) || oldest_time != base_time) {
        printf("oldest record mismatch\n");
        return 1;
    }

    out_len = 0u;
    if (require_ok(rdb_tsdb_get_latest(&db, &latest_time, &latest_sample,
                                       sizeof(latest_sample), &out_len),
                   "get latest") != 0) {
        return 1;
    }
    if (out_len != sizeof(latest_sample) ||
        latest_time != base_time + ((SAMPLE_COUNT - 1u) * 60u) ||
        latest_sample.battery_mv != make_sample(SAMPLE_COUNT - 1u).battery_mv) {
        printf("latest record mismatch\n");
        return 1;
    }

    rdb_tsdb_time_range(&db, &range_oldest, &range_newest);
    if (range_oldest != oldest_time || range_newest != latest_time ||
        rdb_tsdb_count(&db) != SAMPLE_COUNT) {
        printf("time range/count mismatch\n");
        return 1;
    }

    rdb_tsdb_wear_info(&db, &min_ec, &max_ec, &avg_ec);
    rdb_tsdb_get_stats(&db, &stats);

    printf("TSDB example passed\n");
    printf("  samples: total=%u query=%u temp_avg_x100=%d\n",
           (unsigned)rdb_tsdb_count(&db),
           (unsigned)summary.count,
           (int)(summary.temp_sum / (int32_t)summary.count));
    printf("  range: oldest=%u latest=%u latest_battery_mv=%u\n",
           (unsigned)range_oldest,
           (unsigned)range_newest,
           (unsigned)latest_sample.battery_mv);
    printf("  wear: min=%u max=%u avg=%u\n",
           (unsigned)min_ec, (unsigned)max_ec, (unsigned)avg_ec);
    printf("  stats: writes=%u reads=%u rotations=%u lost=%u\n",
           (unsigned)stats.write_ops,
           (unsigned)stats.read_ops,
           (unsigned)stats.sector_rotations,
           (unsigned)stats.records_lost);

    return 0;
}
