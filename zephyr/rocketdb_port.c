/*
 * rocketdb_port.c — Zephyr OS adapter for RocketDB
 *
 * Provides:
 *   - rdb_flash_ops_t backed by Zephyr flash API
 *   - rdb_crc16 / rdb_crc16_cont (CRC-16/MODBUS)
 *   - rdb_hash16 (DJB2, 16-bit)
 *   - Per-instance context factory (rocketdb_partition_init)
 *
 * CRC note:
 *   The engine requires CRC-16/MODBUS (poly 0x8005 reflected = 0xA001,
 *   init 0xFFFF, no output XOR).  Do NOT substitute Zephyr's crc16()
 *   (often CCITT poly 0x1021) without verifying equivalence.
 *
 * SPDX-License-Identifier: MIT
 */

#include "rocketdb_port.h"
#include <string.h>

/* ═══════════════════════════════════════════════════════════
 *  Flash ops — map Zephyr flash API → rdb_flash_ops_t
 *
 *  The ctx pointer carries a struct rocketdb_flash_ctx *.
 *  RocketDB addresses are partition-relative (base_addr=0);
 *  the ctx->offset is added to produce device-absolute offsets.
 * ═══════════════════════════════════════════════════════════ */

static int rdb_zephyr_read(void *ctx, uint32_t addr,
			   uint8_t *buf, size_t len)
{
	struct rocketdb_flash_ctx *c = (struct rocketdb_flash_ctx *)ctx;
	return flash_read(c->dev, c->offset + (off_t)addr, buf, len);
}

static int rdb_zephyr_write(void *ctx, uint32_t addr,
			    const uint8_t *buf, size_t len)
{
	struct rocketdb_flash_ctx *c = (struct rocketdb_flash_ctx *)ctx;
	return flash_write(c->dev, c->offset + (off_t)addr, buf, len);
}

static int rdb_zephyr_erase(void *ctx, uint32_t addr)
{
	struct rocketdb_flash_ctx *c = (struct rocketdb_flash_ctx *)ctx;
	return flash_erase(c->dev, c->offset + (off_t)addr, c->sec_size);
}

static void rdb_zephyr_lock(void *ctx)
{
	struct rocketdb_flash_ctx *c = (struct rocketdb_flash_ctx *)ctx;
	k_mutex_lock(&c->mutex, K_FOREVER);
}

static void rdb_zephyr_unlock(void *ctx)
{
	struct rocketdb_flash_ctx *c = (struct rocketdb_flash_ctx *)ctx;
	k_mutex_unlock(&c->mutex);
}

static void rdb_zephyr_yield(void *ctx)
{
	(void)ctx;
	k_yield();
}

static const rdb_flash_ops_t rdb_zephyr_ops = {
	.read   = rdb_zephyr_read,
	.write  = rdb_zephyr_write,
	.erase  = rdb_zephyr_erase,
	.lock   = rdb_zephyr_lock,
	.unlock = rdb_zephyr_unlock,
	.yield  = rdb_zephyr_yield,
};

/* ═══════════════════════════════════════════════════════════
 *  Partition initializer
 * ═══════════════════════════════════════════════════════════ */

void rocketdb_partition_init(rdb_partition_t           *part,
			     struct rocketdb_flash_ctx *ctx,
			     const struct device       *dev,
			     uint32_t                   offset,
			     uint32_t                   size,
			     uint32_t                   sector_size,
			     uint8_t                    write_gran,
			     const char                *name)
{
	k_mutex_init(&ctx->mutex);
	ctx->dev      = dev;
	ctx->offset   = offset;
	ctx->sec_size = sector_size;

	part->name        = name;
	part->base_addr   = 0;   /* partition-relative addresses */
	part->total_size  = size;
	part->sector_size = sector_size;
	part->write_gran  = write_gran;
	part->ops         = &rdb_zephyr_ops;
	part->flash_ctx   = ctx;
}

/* ═══════════════════════════════════════════════════════════
 *  CRC-16/MODBUS (poly 0x8005 reflected = 0xA001, init 0xFFFF)
 * ═══════════════════════════════════════════════════════════ */

uint16_t rdb_crc16(const void *data, size_t len)
{
	if (data == NULL && len == 0)
		return 0xFFFF;   /* initial seed */

	uint16_t crc = 0xFFFF;
	const uint8_t *p = (const uint8_t *)data;
	for (size_t i = 0; i < len; i++) {
		crc ^= p[i];
		for (int j = 0; j < 8; j++)
			crc = (crc >> 1) ^ ((crc & 1) ? 0xA001 : 0);
	}
	return crc;
}

uint16_t rdb_crc16_cont(uint16_t crc, const void *data, size_t len)
{
	const uint8_t *p = (const uint8_t *)data;
	for (size_t i = 0; i < len; i++) {
		crc ^= p[i];
		for (int j = 0; j < 8; j++)
			crc = (crc >> 1) ^ ((crc & 1) ? 0xA001 : 0);
	}
	return crc;
}

/* ═══════════════════════════════════════════════════════════
 *  DJB2 16-bit hash for KVDB fast-reject lookup
 * ═══════════════════════════════════════════════════════════ */

uint16_t rdb_hash16(const void *data, size_t len)
{
	uint16_t h = 5381;
	const uint8_t *p = (const uint8_t *)data;
	for (size_t i = 0; i < len; i++)
		h = ((h << 5) + h) ^ p[i];   /* h = h * 33 XOR byte */
	return h;
}
