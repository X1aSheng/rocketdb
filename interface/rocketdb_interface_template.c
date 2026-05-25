/**
 * Copyright (c) 2015 XiaSheng(info@zhis.net)
 *
 * The MIT License (MIT)
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in all
 * copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 *
 * @file      rocketdb_interface_template.c
 * @brief     RocketDB interface template source file
 * @version   1.2.0
 * @author    XiaSheng
 * @date      2015-05-04
 *
 * <h3>history</h3>
 * <table>
 * <tr><th>Date        <th>Version  <th>Author      <th>Description
 * <tr><td>2026/04/29  <td>1.0      <td>XiaSheng    <td>first upload
 * </table>
 */

#include "rocketdb_interface.h"

/* ── W25QXX SPI Flash Command Reference ────────────────────────────────────
 * When implementing for W25QXX series SPI NOR Flash, use these command
 * opcodes. Every erase/write must be preceded by WREN (0x06), and the
 * implementation must poll RDSR1 (0x05) until BUSY (bit 0) clears.
 * Page Program must be split at 256-byte page boundaries.
 *
 *   #define W25Q_CMD_WREN        0x06  // Write Enable
 *   #define W25Q_CMD_WRDI        0x04  // Write Disable
 *   #define W25Q_CMD_RDSR1       0x05  // Read Status Register 1
 *   #define W25Q_CMD_READ        0x03  // Read Data
 *   #define W25Q_CMD_PAGE_PG     0x02  // Page Program (1–256 bytes)
 *   #define W25Q_CMD_SECT_ER     0x20  // Sector Erase (4 KB)
 *   #define W25Q_CMD_BLK32_ER    0x52  // Block Erase (32 KB)
 *   #define W25Q_CMD_BLK64_ER    0xD8  // Block Erase (64 KB)
 *   #define W25Q_CMD_RDID        0x9F  // JEDEC ID
 *
 * Status Register 1 bits: BUSY(0), WEL(1), BP0(2), BP1(3), BP2(4)
 * ───────────────────────────────────────────────────────────────────────── */

/**
 * @brief     Flash read operation
 * @param[in] addr absolute flash address
 * @param[out] buf pointer to output buffer
 * @param[in] len number of bytes to read
 * @return    status code
 *            - 0 success
 *            - non-zero read failed
 * @note      Implement flash read for your hardware here.
 */
int rocketdb_interface_flash_read(void *ctx, uint32_t addr, uint8_t* buf, size_t len)
{
    /* TODO: Implement flash read for your hardware */
    (void)ctx;
    (void)addr;
    (void)buf;
    (void)len;
    return 0;
}

/**
 * @brief     Flash write operation
 * @param[in] ctx  Opaque context pointer
 * @param[in] addr absolute flash address
 * @param[in] buf pointer to data buffer
 * @param[in] len number of bytes to write
 * @return    status code
 *            - 0 success
 *            - non-zero write failed
 * @note      Implement flash write for your hardware here.
 *            For W25QXX, split Page Program operations so no command crosses
 *            a 256-byte page boundary.  Preserve NOR 1->0 semantics and return
 *            non-zero if the target range was not erased enough for the write.
 */
int rocketdb_interface_flash_write(void *ctx, uint32_t addr, const uint8_t* buf, size_t len)
{
    /* TODO: Implement flash write for your hardware */
    (void)ctx;
    (void)addr;
    (void)buf;
    (void)len;
    return 0;
}

/**
 * @brief     Flash sector erase operation
 * @param[in] ctx  Opaque context pointer
 * @param[in] addr absolute address within the sector to erase
 * @return    status code
 *            - 0 success
 *            - non-zero erase failed
 * @note      Implement flash erase for your hardware here.
 */
int rocketdb_interface_flash_erase(void *ctx, uint32_t addr)
{
    /* TODO: Implement flash erase for your hardware */
    (void)ctx;
    (void)addr;
    return 0;
}

/**
 * @brief     Acquire flash mutex
 * @note      Implement lock for your hardware here.
 */
void rocketdb_interface_flash_lock(void *ctx)
{
    /* TODO: Implement flash lock for your hardware (may be empty) */
    (void)ctx;
}

/**
 * @brief     Release flash mutex
 * @note      Implement unlock for your hardware here.
 */
void rocketdb_interface_flash_unlock(void *ctx)
{
    /* TODO: Implement flash unlock for your hardware (may be empty) */
    (void)ctx;
}

/**
 * @brief     Yield CPU during long operations
 * @note      Implement yield for your hardware here (may be empty).
 */
void rocketdb_interface_flash_yield(void *ctx)
{
    /* TODO: Implement yield for your hardware (may be empty) */
    (void)ctx;
}

/**
 * @brief     Compute CRC-16 over a data block
 * @param[in] data pointer to data buffer
 * @param[in] len  number of bytes
 * @return    16-bit CRC value
 * @note      Implement CRC-16/MODBUS for your hardware here.
 *            rdb_crc16(NULL, 0) must return the initial seed 0xFFFF.
 */
uint16_t rocketdb_interface_crc16(const void* data, size_t len)
{
    /* TODO: Implement CRC-16/MODBUS for your hardware */
    if (data == NULL && len == 0)
        return 0xFFFF;

    uint16_t crc = 0xFFFF;
    const uint8_t* p = (const uint8_t*)data;
    for (size_t i = 0; i < len; i++)
    {
        crc ^= p[i];
        for (int j = 0; j < 8; j++)
            crc = (crc >> 1) ^ ((crc & 1) ? 0xA001 : 0);
    }
    return crc;
}

/**
 * @brief     Continue CRC-16 computation with additional data
 * @param[in] crc  previous CRC value
 * @param[in] data pointer to next data block
 * @param[in] len  number of bytes
 * @return    updated 16-bit CRC value
 * @note      Implement CRC-16/MODBUS continuation for your hardware here.
 */
uint16_t rocketdb_interface_crc16_cont(uint16_t crc, const void* data, size_t len)
{
    /* TODO: Implement CRC-16/MODBUS continuation for your hardware */
    const uint8_t* p = (const uint8_t*)data;
    for (size_t i = 0; i < len; i++)
    {
        crc ^= p[i];
        for (int j = 0; j < 8; j++)
            crc = (crc >> 1) ^ ((crc & 1) ? 0xA001 : 0);
    }
    return crc;
}

/**
 * @brief     Compute 16-bit hash of a key
 * @param[in] data pointer to key data
 * @param[in] len  key length in bytes
 * @return    16-bit hash value
 * @note      Implement a hash function for your hardware here.
 *            RocketDB uses FNV-1a folded to 16 bits.  Keep this identical
 *            across bare-metal, simulator, and Zephyr builds so existing
 *            flash contents remain portable.
 */
uint16_t rocketdb_interface_hash16(const void* data, size_t len)
{
    /* TODO: Replace with a hardware-accelerated equivalent if available */
    uint32_t h = 2166136261u;
    const uint8_t* p = (const uint8_t*)data;
    for (size_t i = 0; i < len; i++)
    {
        h ^= p[i];
        h *= 16777619u;
    }
    return (uint16_t)(h ^ (h >> 16));
}

/* Symbols consumed by RocketDB core.  Applications may either compile this
 * template as-is after filling in the flash operations, or provide equivalent
 * rdb_* functions elsewhere. */
uint16_t rdb_crc16(const void* data, size_t len)
{
    return rocketdb_interface_crc16(data, len);
}

uint16_t rdb_crc16_cont(uint16_t crc, const void* data, size_t len)
{
    return rocketdb_interface_crc16_cont(crc, data, len);
}

uint16_t rdb_hash16(const void* data, size_t len)
{
    return rocketdb_interface_hash16(data, len);
}

const rdb_flash_ops_t rocketdb_interface_ops = {
    .read   = rocketdb_interface_flash_read,
    .write  = rocketdb_interface_flash_write,
    .erase  = rocketdb_interface_flash_erase,
    .lock   = rocketdb_interface_flash_lock,
    .unlock = rocketdb_interface_flash_unlock,
    .yield  = rocketdb_interface_flash_yield,
};

/**
 * @brief     Debug print output
 * @param[in] fmt format string
 * @param[in] ... variable arguments
 * @note      Implement debug print for your hardware here.
 */
void rocketdb_interface_debug_print(const char* const fmt, ...)
{
    /* TODO: Implement debug print for your hardware (may be empty) */
    (void)fmt;
}
