#ifndef RDB_SIM_FLASH_H
#define RDB_SIM_FLASH_H

#include <stdint.h>
#include <stddef.h>
#include "sim_fault.h"
#include "sim_trace.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint32_t read_fail_at;
    uint32_t write_fail_at;
    uint32_t erase_fail_at;
    uint32_t read_count;
    uint32_t write_count;
    uint32_t erase_count;
} sim_fault_t;

typedef struct {
    uint8_t       *mem;
    uint32_t       size;
    uint32_t       sector_size;
    uint32_t       page_size;
    uint8_t        write_gran; /* 0->1B,1->2B,2->4B,3->8B */
    sim_fault_t    old_fault;  /* 旧的简单故障系统（保留兼容） */
    fault_ctx_t   *fault_ctx;  /* 新的高级故障注入系统 */
    trace_ctx_t   *trace;      /* 可选：flash 操作追踪（NULL=不追踪） */
} sim_flash_t;

int sim_flash_init(sim_flash_t *f, uint8_t *buf, uint32_t size,
                   uint32_t sector_size, uint32_t page_size,
                   uint8_t write_gran);

int sim_flash_read(sim_flash_t *f, uint32_t addr, uint8_t *buf, size_t len);
int sim_flash_write(sim_flash_t *f, uint32_t addr, const uint8_t *buf, size_t len);
int sim_flash_erase(sim_flash_t *f, uint32_t addr);

void sim_flash_reset_faults(sim_flash_t *f);
void sim_flash_set_fault(sim_flash_t *f, uint32_t r, uint32_t w, uint32_t e);

/**
 * @brief 设置高级故障注入上下文
 * @param f      Flash 模拟器
 * @param ctx    故障上下文（NULL 禁用高级故障注入）
 */
void sim_flash_set_fault_ctx(sim_flash_t *f, fault_ctx_t *ctx);

/**
 * @brief 设置操作追踪上下文
 * @param f      Flash 模拟器
 * @param t      追踪上下文（NULL 禁用追踪）
 */
static inline void sim_flash_set_trace(sim_flash_t *f, trace_ctx_t *t)
{
    if (f) f->trace = t;
}

#ifdef __cplusplus
}
#endif

#endif
