#include "rocketdb.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define EXAMPLE_SECTOR_SIZE 4096u
#define EXAMPLE_SECTOR_COUNT 8u
#define EXAMPLE_FLASH_SIZE (EXAMPLE_SECTOR_SIZE * EXAMPLE_SECTOR_COUNT)

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
    rdb_kvdb_t db;
    rdb_kv_sector_meta_t meta[EXAMPLE_SECTOR_COUNT];
    uint32_t boot_count = 7u;
    uint32_t loaded_count = 0u;
    char ssid[32];
    uint16_t ssid_len = 0u;

    memset(g_flash, 0xFF, sizeof(g_flash));
    memset(&part, 0, sizeof(part));
    memset(&db, 0, sizeof(db));
    memset(meta, 0, sizeof(meta));
    memset(ssid, 0, sizeof(ssid));

    part.name = "example-kv";
    part.base_addr = 0u;
    part.total_size = EXAMPLE_FLASH_SIZE;
    part.sector_size = EXAMPLE_SECTOR_SIZE;
    part.write_gran = 1u;
    part.ops = &flash_ops;

    db.part = &part;
    db.sectors = meta;
    db.sector_cnt = EXAMPLE_SECTOR_COUNT;
    if (require_ok(rdb_kvdb_format(&db), "kvdb format") != 0) {
        return 1;
    }
    if (require_ok(rdb_kvdb_init(&db, &part, meta), "kvdb init") != 0) {
        return 1;
    }
    if (require_ok(rdb_kvdb_set(&db, "wifi.ssid", "rocket-lab", 11u), "set ssid") != 0) {
        return 1;
    }
    if (require_ok(rdb_kvdb_set(&db, "boot.count", &boot_count, sizeof(boot_count)),
                   "set boot count") != 0) {
        return 1;
    }
    if (require_ok(rdb_kvdb_get(&db, "wifi.ssid", ssid, sizeof(ssid), &ssid_len),
                   "get ssid") != 0) {
        return 1;
    }
    if (require_ok(rdb_kvdb_get(&db, "boot.count", &loaded_count,
                                sizeof(loaded_count), NULL),
                   "get boot count") != 0) {
        return 1;
    }
    if (strcmp(ssid, "rocket-lab") != 0 || loaded_count != boot_count) {
        printf("readback mismatch\n");
        return 1;
    }
    if (require_ok(rdb_kvdb_delete(&db, "wifi.ssid"), "delete ssid") != 0) {
        return 1;
    }
    if (rdb_kvdb_exists(&db, "wifi.ssid") == RDB_OK) {
        printf("deleted key still exists\n");
        return 1;
    }

    printf("KVDB example passed: boot.count=%u\n", (unsigned)loaded_count);
    return 0;
}
