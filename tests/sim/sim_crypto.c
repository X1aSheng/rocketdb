#include <stdint.h>
#include <stddef.h>
#include "rocketdb.h"

/* CRC16-MODBUS implementation */
uint16_t rdb_crc16(const void *data, size_t len)
{
    if (data == NULL && len == 0)
        return 0xFFFFu;  /* initial seed */
    const uint8_t *p = (const uint8_t *)data;
    uint16_t crc = 0xFFFFu;
    for (size_t i = 0; i < len; i++) {
        crc ^= p[i];
        for (uint8_t b = 0; b < 8; b++) {
            if (crc & 1u) crc = (uint16_t)((crc >> 1) ^ 0xA001u);
            else crc >>= 1;
        }
    }
    return crc;
}

uint16_t rdb_crc16_cont(uint16_t crc, const void *data, size_t len)
{
    const uint8_t *p = (const uint8_t *)data;
    for (size_t i = 0; i < len; i++) {
        crc ^= p[i];
        for (uint8_t b = 0; b < 8; b++) {
            if (crc & 1u) crc = (uint16_t)((crc >> 1) ^ 0xA001u);
            else crc >>= 1;
        }
    }
    return crc;
}

uint16_t rdb_hash16(const void *data, size_t len)
{
    const uint8_t *p = (const uint8_t *)data;
    uint32_t h = 2166136261u;
    for (size_t i = 0; i < len; i++) {
        h ^= p[i];
        h *= 16777619u;
    }
    return (uint16_t)(h ^ (h >> 16));
}
