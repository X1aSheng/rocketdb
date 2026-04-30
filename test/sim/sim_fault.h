/**
 * sim_fault.h - RocketDB Flash 故障注入接口
 * 
 * 功能：
 * - CRC 损坏注入（修改已写入数据）
 * - 操作失败模拟（read/write/erase 失败）
 * - 随机掉电中断（在关键路径中断操作）
 * - 概率性故障注入
 * - 故障记录与重放
 * 
 * Copyright (c) 2026 RocketDB Contributors
 * SPDX-License-Identifier: MIT
 */

#ifndef SIM_FAULT_H
#define SIM_FAULT_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ═══════════════════════════════════════════════════════════════════════════
 *  §1  故障类型定义
 * ═══════════════════════════════════════════════════════════════════════════ */

/**
 * @brief 故障注入类型
 */
typedef enum {
    FAULT_TYPE_NONE = 0,           /**< 无故障 */
    FAULT_TYPE_READ_FAIL,          /**< 读取失败 */
    FAULT_TYPE_WRITE_FAIL,         /**< 写入失败 */
    FAULT_TYPE_ERASE_FAIL,         /**< 擦除失败 */
    FAULT_TYPE_POWER_LOSS,         /**< 掉电中断 */
    FAULT_TYPE_DATA_CORRUPT,       /**< 数据损坏 */
    FAULT_TYPE_BIT_FLIP,           /**< 单比特翻转 */
} fault_type_t;

/**
 * @brief 故障触发模式
 */
typedef enum {
    FAULT_TRIGGER_COUNT = 0,       /**< 基于计数：第 N 次操作 */
    FAULT_TRIGGER_PROBABILITY,     /**< 基于概率：每次操作有 P% 概率 */
    FAULT_TRIGGER_ADDRESS,         /**< 基于地址：特定地址范围 */
    FAULT_TRIGGER_PATTERN,         /**< 基于模式：特定数据模式 */
} fault_trigger_mode_t;

/**
 * @brief 故障注入规则
 */
typedef struct {
    fault_type_t         type;             /**< 故障类型 */
    fault_trigger_mode_t trigger_mode;     /**< 触发模式 */
    uint32_t             trigger_count;    /**< COUNT 模式：触发次数 */
    uint32_t             probability_pct;  /**< PROBABILITY 模式：概率（0-100） */
    uint32_t             addr_start;       /**< ADDRESS 模式：起始地址 */
    uint32_t             addr_end;         /**< ADDRESS 模式：结束地址 */
    uint32_t             seed;             /**< 随机数种子 */
    int                  enabled;          /**< 是否启用 */
} fault_rule_t;

/**
 * @brief 故障注入上下文
 */
typedef struct {
    fault_rule_t  rules[16];               /**< 最多 16 条故障规则 */
    uint32_t      rule_count;              /**< 当前规则数量 */
    
    /* 操作计数器 */
    uint32_t      read_count;
    uint32_t      write_count;
    uint32_t      erase_count;
    
    /* 故障统计 */
    uint32_t      fault_injected;          /**< 总注入故障数 */
    uint32_t      fault_by_type[8];        /**< 按类型统计 */
    
    /* 掉电模拟 */
    int           power_loss_armed;        /**< 掉电已准备 */
    uint32_t      power_loss_at_write;     /**< 在第 N 次写入时掉电 */
    uint32_t      power_loss_at_byte;      /**< 在字节偏移处掉电 */
    
    /* 数据损坏注入 */
    uint32_t      corrupt_addr;            /**< 损坏地址 */
    uint32_t      corrupt_len;             /**< 损坏长度 */
    uint8_t       corrupt_pattern;         /**< 损坏模式 */
    
    /* PRNG 状态 */
    uint32_t      prng_state;              /**< PRNG 状态（用于概率注入） */
} fault_ctx_t;

/* ═══════════════════════════════════════════════════════════════════════════
 *  §2  故障注入 API
 * ═══════════════════════════════════════════════════════════════════════════ */

/**
 * @brief 初始化故障注入上下文
 * @param ctx   故障上下文
 * @param seed  随机数种子
 */
void fault_init(fault_ctx_t *ctx, uint32_t seed);

/**
 * @brief 添加故障注入规则
 * @param ctx   故障上下文
 * @param rule  故障规则
 * @return      规则 ID（失败返回 -1）
 */
int fault_add_rule(fault_ctx_t *ctx, const fault_rule_t *rule);

/**
 * @brief 移除故障注入规则
 * @param ctx      故障上下文
 * @param rule_id  规则 ID
 */
void fault_remove_rule(fault_ctx_t *ctx, uint32_t rule_id);

/**
 * @brief 清除所有故障规则
 * @param ctx  故障上下文
 */
void fault_clear_rules(fault_ctx_t *ctx);

/**
 * @brief 启用/禁用故障规则
 * @param ctx      故障上下文
 * @param rule_id  规则 ID
 * @param enabled  1=启用，0=禁用
 */
void fault_set_rule_enabled(fault_ctx_t *ctx, uint32_t rule_id, int enabled);

/* ═══════════════════════════════════════════════════════════════════════════
 *  §3  故障检查（由 Flash 模拟器调用）
 * ═══════════════════════════════════════════════════════════════════════════ */

/**
 * @brief 检查读操作是否应该失败
 * @param ctx   故障上下文
 * @param addr  读取地址
 * @param len   读取长度
 * @return      1=应该失败，0=正常执行
 */
int fault_should_read_fail(fault_ctx_t *ctx, uint32_t addr, size_t len);

/**
 * @brief 检查写操作是否应该失败
 * @param ctx   故障上下文
 * @param addr  写入地址
 * @param len   写入长度
 * @return      1=应该失败，0=正常执行
 */
int fault_should_write_fail(fault_ctx_t *ctx, uint32_t addr, size_t len);

/**
 * @brief 检查擦除操作是否应该失败
 * @param ctx   故障上下文
 * @param addr  擦除地址
 * @return      1=应该失败，0=正常执行
 */
int fault_should_erase_fail(fault_ctx_t *ctx, uint32_t addr);

/**
 * @brief 检查是否应该触发掉电中断
 * @param ctx       故障上下文
 * @param addr      当前写入地址
 * @param offset    字节偏移
 * @return          1=触发掉电，0=继续执行
 */
int fault_should_power_loss(fault_ctx_t *ctx, uint32_t addr, uint32_t offset);

/**
 * @brief 注入数据损坏（修改已写入的数据）
 * @param ctx   故障上下文
 * @param mem   Flash 内存缓冲区
 * @param addr  损坏地址
 * @param len   损坏长度
 */
void fault_inject_corruption(fault_ctx_t *ctx, uint8_t *mem, 
                             uint32_t addr, uint32_t len);

/* ═══════════════════════════════════════════════════════════════════════════
 *  §4  便捷函数（快速配置常见故障场景）
 * ═══════════════════════════════════════════════════════════════════════════ */

/**
 * @brief 配置：在第 N 次写入时失败
 */
void fault_quick_write_fail(fault_ctx_t *ctx, uint32_t nth);

/**
 * @brief 配置：在第 N 次擦除时失败
 */
void fault_quick_erase_fail(fault_ctx_t *ctx, uint32_t nth);

/**
 * @brief 配置：以 P% 概率写入失败
 */
void fault_quick_write_fail_probability(fault_ctx_t *ctx, uint32_t pct);

/**
 * @brief 配置：在第 N 次写入的第 M 字节时掉电
 */
void fault_quick_power_loss(fault_ctx_t *ctx, uint32_t nth_write, uint32_t byte_offset);

/**
 * @brief 配置：损坏指定地址的数据
 */
void fault_quick_corrupt_data(fault_ctx_t *ctx, uint32_t addr, uint32_t len, uint8_t pattern);

/**
 * @brief 配置：在地址范围内以 P% 概率写入失败
 */
void fault_quick_region_fail(fault_ctx_t *ctx, uint32_t addr_start, uint32_t addr_end, uint32_t pct);

/* ═══════════════════════════════════════════════════════════════════════════
 *  §5  统计与调试
 * ═══════════════════════════════════════════════════════════════════════════ */

/**
 * @brief 获取故障统计信息
 * @param ctx   故障上下文
 * @param ops   输出：操作次数（read/write/erase）
 * @param faults 输出：故障注入次数（按类型）
 */
void fault_get_stats(fault_ctx_t *ctx, uint32_t ops[3], uint32_t faults[8]);

/**
 * @brief 重置故障统计
 * @param ctx  故障上下文
 */
void fault_reset_stats(fault_ctx_t *ctx);

/**
 * @brief 打印故障注入报告
 * @param ctx  故障上下文
 */
void fault_print_report(fault_ctx_t *ctx);

/**
 * @brief 导出故障规则（用于记录和重放）
 * @param ctx   故障上下文
 * @param buf   输出缓冲区
 * @param len   缓冲区大小
 * @return      写入的字节数
 */
int fault_export_rules(fault_ctx_t *ctx, char *buf, size_t len);

/**
 * @brief 导入故障规则（从字符串）
 * @param ctx   故障上下文
 * @param str   规则字符串
 * @return      导入的规则数量
 */
int fault_import_rules(fault_ctx_t *ctx, const char *str);

#ifdef __cplusplus
}
#endif

#endif /* SIM_FAULT_H */
