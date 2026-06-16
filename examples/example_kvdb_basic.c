#include "rocketdb.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define EXAMPLE_SECTOR_SIZE 4096u
#define EXAMPLE_SECTOR_COUNT 8u
#define EXAMPLE_FLASH_SIZE (EXAMPLE_SECTOR_SIZE * EXAMPLE_SECTOR_COUNT)
#define EXAMPLE_PAGE_SIZE 256u

typedef struct {
    char wifi_ssid[24];
    char wifi_password[32];
    uint32_t boot_count;
    uint8_t retry_limit;
} app_config_t;

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
    part->name = "kvdb-config";
    part->base_addr = 0u;
    part->total_size = EXAMPLE_FLASH_SIZE;
    part->sector_size = EXAMPLE_SECTOR_SIZE;
    part->write_gran = 0u;
    part->ops = &flash_ops;
    part->flash_ctx = NULL;
}

static rdb_err_t open_clean_kvdb(rdb_kvdb_t *db,
                                 const rdb_partition_t *part,
                                 rdb_kv_sector_meta_t *meta)
{
    memset(db, 0, sizeof(*db));
    memset(meta, 0, sizeof(rdb_kv_sector_meta_t) * EXAMPLE_SECTOR_COUNT);

    db->part = part;
    db->sectors = meta;
    db->sector_cnt = EXAMPLE_SECTOR_COUNT;

    if (rdb_kvdb_format(db) != RDB_OK) {
        return RDB_ERR_FLASH;
    }

    return rdb_kvdb_init(db, part, meta);
}

static int set_text(rdb_kvdb_t *db, const char *key, const char *text)
{
    return require_ok(rdb_kvdb_set(db, key, text, (uint16_t)(strlen(text) + 1u)), key);
}

static int print_kv_pairs(rdb_kvdb_t *db)
{
    rdb_kv_iter_t it;
    rdb_err_t status;
    char key[RDB_MAX_KEY_LEN + 1u];
    char value[48];
    uint16_t key_len;
    uint16_t value_len;

    if (require_ok(rdb_kv_iter_init(&it, db), "iterator init") != 0) {
        return 1;
    }

    printf("active KV pairs:\n");
    while ((status = rdb_kv_iter_next(&it, key, sizeof(key), value, sizeof(value),
                                      &key_len, &value_len)) == RDB_OK) {
        if (strcmp(key, "cfg") == 0) {
            printf("  %.*s = <app_config_t %u bytes>\n",
                   (int)key_len, key, (unsigned)value_len);
        } else {
            if (value_len >= sizeof(value)) {
                value[sizeof(value) - 1u] = '\0';
            } else {
                value[value_len] = '\0';
            }
            printf("  %.*s = %s\n", (int)key_len, key, value);
        }
    }

    if (status != RDB_ERR_ITER_END) {
        printf("iterator failed: %d\n", (int)status);
        return 1;
    }

    return 0;
}

int main(void)
{
    rdb_partition_t part;
    rdb_kvdb_t db;
    uint8_t meta_buf[EXAMPLE_SECTOR_COUNT * (sizeof(rdb_kv_sector_meta_t) + RDB_BLOOM_BYTES)];
    rdb_kv_sector_meta_t *meta = (rdb_kv_sector_meta_t *)meta_buf;
    app_config_t config;
    app_config_t loaded_config;
    rdb_kv_stats_t stats;
    uint32_t total;
    uint32_t used;
    uint32_t avail;
    uint32_t min_ec;
    uint32_t max_ec;
    uint32_t avg_ec;
    uint16_t out_len;

    memset(g_flash, 0xFF, sizeof(g_flash));
    memset(g_erase_count, 0, sizeof(g_erase_count));
    memset(&config, 0, sizeof(config));
    memset(&loaded_config, 0, sizeof(loaded_config));
    fill_partition(&part);

    snprintf(config.wifi_ssid, sizeof(config.wifi_ssid), "%s", "rocket-lab");
    snprintf(config.wifi_password, sizeof(config.wifi_password), "%s",
             "change-me-before-flight");
    config.boot_count = 7u;
    config.retry_limit = 3u;

    if (require_ok(open_clean_kvdb(&db, &part, meta), "open KVDB") != 0) {
        return 1;
    }

    if (require_ok(rdb_kvdb_set(&db, "cfg", &config, sizeof(config)), "set cfg") != 0 ||
        set_text(&db, "device.id", "RDB-DEV-0001") != 0 ||
        set_text(&db, "net.mode", "station") != 0 ||
        set_text(&db, "cal.rev", "A0") != 0) {
        return 1;
    }

    config.boot_count++;
    if (require_ok(rdb_kvdb_set(&db, "cfg", &config, sizeof(config)), "update cfg") != 0 ||
        require_ok(rdb_kvdb_delete(&db, "cal.rev"), "delete cal.rev") != 0) {
        return 1;
    }

    out_len = 0u;
    if (require_ok(rdb_kvdb_get(&db, "cfg", &loaded_config, sizeof(loaded_config),
                                &out_len), "get cfg") != 0) {
        return 1;
    }
    if (out_len != sizeof(loaded_config) ||
        loaded_config.boot_count != 8u ||
        strcmp(loaded_config.wifi_ssid, "rocket-lab") != 0 ||
        rdb_kvdb_exists(&db, "cal.rev") != RDB_ERR_NOT_FOUND) {
        printf("KVDB readback mismatch\n");
        return 1;
    }

    if (print_kv_pairs(&db) != 0) {
        return 1;
    }

    rdb_kvdb_space_info(&db, &total, &used, &avail);
    rdb_kvdb_wear_info(&db, &min_ec, &max_ec, &avg_ec);
    rdb_kvdb_get_stats(&db, &stats);

    printf("KVDB example passed\n");
    printf("  config: ssid=%s boot_count=%u retry=%u\n",
           loaded_config.wifi_ssid,
           (unsigned)loaded_config.boot_count,
           (unsigned)loaded_config.retry_limit);
    printf("  space: total=%u used=%u avail=%u\n",
           (unsigned)total, (unsigned)used, (unsigned)avail);
    printf("  wear: min=%u max=%u avg=%u\n",
           (unsigned)min_ec, (unsigned)max_ec, (unsigned)avg_ec);
    printf("  stats: writes=%u reads=%u deletes=%u gc=%u\n",
           (unsigned)stats.write_ops,
           (unsigned)stats.read_ops,
           (unsigned)stats.delete_ops,
           (unsigned)stats.gc_runs);

    return 0;
}
