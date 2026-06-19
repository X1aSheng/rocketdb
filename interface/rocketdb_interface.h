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

#ifndef ROCKETDB_INTERFACE_H
#define ROCKETDB_INTERFACE_H

#include <stddef.h>
#include <stdint.h>
#include "rocketdb.h"

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
 * @param[in] ctx  Opaque context pointer (e.g. flash device handle)
 * @param[in] addr absolute flash address
 * @param[out] buf pointer to output buffer
 * @param[in] len number of bytes to read
 * @return    status code
 *            - 0 success
 *            - non-zero read failed
 * @note      Must handle unaligned addresses.
 */
int rocketdb_interface_flash_read(void *ctx, uint32_t addr, uint8_t* buf, size_t len);

/**
 * @brief     Flash write operation
 * @param[in] ctx  Opaque context pointer
 * @param[in] addr absolute flash address
 * @param[in] buf pointer to data buffer
 * @param[in] len number of bytes to write
 * @return    status code
 *            - 0 success
 *            - non-zero write failed
 * @note      Must respect NOR flash 1->0 bit-flip semantics.
 *            Caller guarantees addr and len are write-granularity aligned
 *            for data payloads.  Single-byte state-transition writes
 *            (commit byte, mark_dead) occur at addr+1 regardless of
 *            write_gran and rely on NOR flash byte-program capability.
 */
int rocketdb_interface_flash_write(void *ctx, uint32_t addr, const uint8_t* buf, size_t len);

/**
 * @brief     Flash sector erase operation
 * @param[in] ctx  Opaque context pointer
 * @param[in] addr absolute address within the sector to erase
 * @return    status code
 *            - 0 success
 *            - non-zero erase failed
 * @note      Erases the entire sector containing addr.
 *            All bytes in the sector become 0xFF after erase.
 */
int rocketdb_interface_flash_erase(void *ctx, uint32_t addr);

/**
 * @brief     Acquire flash mutex
 * @note      May be empty if single-threaded.
 *            Should disable interrupts or take a mutex.
 */
void rocketdb_interface_flash_lock(void *ctx);

/**
 * @brief     Release flash mutex
 * @note      May be empty if single-threaded.
 *            Should enable interrupts or release a mutex.
 */
void rocketdb_interface_flash_unlock(void *ctx);

/**
 * @brief     Yield CPU during long operations
 * @note      May be empty.  Useful for feeding watchdog or
 *            yielding to RTOS scheduler during GC / rotation.
 */
void rocketdb_interface_flash_yield(void *ctx);

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
 *            Must match RocketDB's FNV-1a folded 16-bit hash.
 */
uint16_t rocketdb_interface_hash16(const void* data, size_t len);

/**
 * @brief     RocketDB-compatible flash ops table for this interface template.
 * @note      Assign this table to rdb_partition_t.ops and set
 *            rdb_partition_t.flash_ctx to your hardware context pointer.
 */
extern const rdb_flash_ops_t rocketdb_interface_ops;

/**
 * @brief     Debug print output
 * @param[in] fmt format string
 * @param[in] ... variable arguments
 * @note      May be empty in production builds.
 */
void rocketdb_interface_debug_print(const char* const fmt, ...);

/* Forward declaration (full definition in spi_flash.h) */
typedef struct spi_flash_device spi_flash_device_t;

/**
 * @brief     Register a pre-initialised flash device with the HAL port.
 * @param[in] dev  spi_flash_device_t* already initialised by main.c.
 * @note      Must be called once after spi_flash_create()+spi_flash_init().
 *            Sets the file-static device pointer used by all ops callbacks.
 */
void rocketdb_interface_init(spi_flash_device_t *dev);

/**
 * @brief     Return the registered flash device pointer (opaque).
 * @return    void* suitable for assigning to rdb_partition_t.flash_ctx.
 * @note      Call after rocketdb_interface_init().  Returns NULL if
 *            init hasn't been called.
 */
void *rocketdb_interface_get_ctx(void);

/**
 * @brief     Probe a SPI Flash device using an already-initialised port.
 * @param[in] port_ops  Pointer to pre-initialised spi_flash_port_ops_t.
 * @return    Flash device pointer, or NULL on failure.
 * @note      spi_flash_port_init() must have been called beforehand.
 *            This internally calls spi_flash_create() + spi_flash_init().
 *            Prefer rocketdb_interface_init() when main.c manages the device.
 */
spi_flash_device_t *rocketdb_interface_flash_probe(
    const void *port_ops);

/**
 * @}
 */

#ifdef __cplusplus
}
#endif

#endif /* ROCKETDB_INTERFACE_H */
