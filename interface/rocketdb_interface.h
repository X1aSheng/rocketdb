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
 * @file      rocketdb_interface.h
 * @brief     RocketDB hardware abstraction layer (HAL) interface
 * @version   1.1.0
 * @author    XiaSheng
 * @date      2015-05-04
 *
 * <h3>history</h3>
 * <table>
 * <tr><th>Date        <th>Version  <th>Author      <th>Description
 * <tr><td>2026/04/29  <td>1.0      <td>XiaSheng    <td>first upload
 * </table>
 */

#ifndef ROCKETDB_INTERFACE_H
#define ROCKETDB_INTERFACE_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @defgroup rocketdb_interface RocketDB interface driver
 * @brief    Hardware abstraction layer for RocketDB
 * @{
 */

/**
 * @brief     Flash read operation
 * @param[in] addr absolute flash address
 * @param[out] buf pointer to output buffer
 * @param[in] len number of bytes to read
 * @return    status code
 *            - 0 success
 *            - non-zero read failed
 * @note      Must handle unaligned addresses.
 */
int rocketdb_interface_flash_read(uint32_t addr, uint8_t* buf, size_t len);

/**
 * @brief     Flash write operation
 * @param[in] addr absolute flash address
 * @param[in] buf pointer to data buffer
 * @param[in] len number of bytes to write
 * @return    status code
 *            - 0 success
 *            - non-zero write failed
 * @note      Must respect NOR flash 1->0 bit-flip semantics.
 *            Caller guarantees addr and len are write-granularity aligned.
 */
int rocketdb_interface_flash_write(uint32_t addr, const uint8_t* buf, size_t len);

/**
 * @brief     Flash sector erase operation
 * @param[in] addr absolute address within the sector to erase
 * @return    status code
 *            - 0 success
 *            - non-zero erase failed
 * @note      Erases the entire sector containing addr.
 *            All bytes in the sector become 0xFF after erase.
 */
int rocketdb_interface_flash_erase(uint32_t addr);

/**
 * @brief     Acquire flash mutex
 * @note      May be empty if single-threaded.
 *            Should disable interrupts or take a mutex.
 */
void rocketdb_interface_flash_lock(void);

/**
 * @brief     Release flash mutex
 * @note      May be empty if single-threaded.
 *            Should enable interrupts or release a mutex.
 */
void rocketdb_interface_flash_unlock(void);

/**
 * @brief     Yield CPU during long operations
 * @note      May be empty.  Useful for feeding watchdog or
 *            yielding to RTOS scheduler during GC / rotation.
 */
void rocketdb_interface_flash_yield(void);

/**
 * @brief     Compute CRC-16 over a data block
 * @param[in] data pointer to data buffer
 * @param[in] len  number of bytes
 * @return    16-bit CRC value
 * @note      RocketDB uses CRC-16/MODBUS (polynomial 0xA001, reflected).
 *            Seed value is 0xFFFF.
 */
uint16_t rocketdb_interface_crc16(const void* data, size_t len);

/**
 * @brief     Continue CRC-16 computation with additional data
 * @param[in] crc  previous CRC value
 * @param[in] data pointer to next data block
 * @param[in] len  number of bytes
 * @return    updated 16-bit CRC value
 * @note      Allows streaming CRC computation across multiple blocks.
 */
uint16_t rocketdb_interface_crc16_cont(uint16_t crc, const void* data, size_t len);

/**
 * @brief     Compute 16-bit hash of a key
 * @param[in] data pointer to key data
 * @param[in] len  key length in bytes
 * @return    16-bit hash value
 * @note      Used for KVDB fast-reject lookup.
 *            DJB2 or similar distribution is recommended.
 */
uint16_t rocketdb_interface_hash16(const void* data, size_t len);

/**
 * @brief     Debug print output
 * @param[in] fmt format string
 * @param[in] ... variable arguments
 * @note      May be empty in production builds.
 */
void rocketdb_interface_debug_print(const char* const fmt, ...);

/**
 * @}
 */

#ifdef __cplusplus
}
#endif

#endif /* ROCKETDB_INTERFACE_H */
