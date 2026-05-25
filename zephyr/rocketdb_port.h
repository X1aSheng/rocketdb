/*
 * rocketdb_port.h — Zephyr OS adapter for RocketDB (public header)
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef ROCKETDB_PORT_H
#define ROCKETDB_PORT_H

#include <zephyr/kernel.h>
#include <zephyr/drivers/flash.h>
#include <rocketdb.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Per-instance flash context for RocketDB on Zephyr.
 *
 * Holds the Zephyr flash device, partition base offset, sector size,
 * and a per-instance mutex for lock/unlock ops.
 *
 * Allocate one of these per RocketDB partition, statically or on the
 * stack of a persistent thread.  It must outlive the database handle.
 */
struct rocketdb_flash_ctx {
	const struct device *dev;       /* Zephyr flash device */
	uint32_t             offset;    /* Partition base offset within the device */
	uint32_t             sec_size;  /* Erase sector size (cached for erase op) */
	uint8_t              write_gran;/* Write granularity exponent */
	struct k_mutex       mutex;     /* Per-instance mutex */
};

/**
 * @brief Wire a Zephyr flash partition for use with RocketDB.
 *
 * Populates the rdb_partition_t fields so the partition is backed by
 * a Zephyr flash device.  The ctx must outlive the database.
 *
 * @param part        [out] rdb_partition_t to populate
 * @param ctx         [in]  Pre-allocated context (will be initialised)
 * @param dev         [in]  Zephyr flash device (e.g. from DEVICE_DT_GET)
 * @param offset      [in]  Byte offset of the partition within the flash device
 * @param size        [in]  Partition size in bytes
 * @param sector_size [in]  Erase sector size (must be power of 2, ≥ 4096)
 * @param write_gran  [in]  Write granularity exponent (0→1B, 1→2B, 2→4B, 3→8B)
 * @param name        [in]  Human-readable label (static string, not copied)
 */
void rocketdb_partition_init(rdb_partition_t           *part,
			     struct rocketdb_flash_ctx *ctx,
			     const struct device       *dev,
			     uint32_t                   offset,
			     uint32_t                   size,
			     uint32_t                   sector_size,
			     uint8_t                    write_gran,
			     const char                *name);

#ifdef __cplusplus
}
#endif

#endif /* ROCKETDB_PORT_H */
