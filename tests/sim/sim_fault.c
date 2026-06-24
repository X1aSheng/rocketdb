/**
 * sim_fault.c - RocketDB Flash 故障注入实现
 *
 * Copyright (c) 2026 RocketDB Contributors
 * SPDX-License-Identifier: MIT
 */

#include "sim_fault.h"
#include <stdio.h>
#include <string.h>

/* ═══════════════════════════════════════════════════════════════════════════
 *  §1  内部辅助函数
 * ═══════════════════════════════════════════════════════════════════════════ */

/**
 * @brief 简单的 PRNG（线性同余生成器）
 */
static uint32_t prng_next(uint32_t* state) {
    *state = (*state * 1103515245u + 12345u) & 0x7FFFFFFFu;
    return *state;
}

/**
 * @brief 生成 0-99 的随机数（用于概率判断）
 */
static uint32_t prng_percent(uint32_t* state) {
    return prng_next(state) % 100u;
}

/**
 * @brief 检查地址是否在范围内
 */
static int addr_in_range(uint32_t addr, uint32_t start, uint32_t end) {
    return (addr >= start && addr < end);
}

/* ═══════════════════════════════════════════════════════════════════════════
 *  §2  故障注入核心 API
 * ═══════════════════════════════════════════════════════════════════════════ */

void fault_init(fault_ctx_t* ctx, uint32_t seed) {
    if (!ctx)
        return;

    memset(ctx, 0, sizeof(fault_ctx_t));
    ctx->prng_state      = seed ? seed : 0x12345678u;
    ctx->corrupt_pattern = 0xFF;
}

int fault_add_rule(fault_ctx_t* ctx, const fault_rule_t* rule) {
    if (!ctx || !rule || ctx->rule_count >= 16) {
        return -1;
    }

    ctx->rules[ctx->rule_count] = *rule;
    return ctx->rule_count++;
}

void fault_remove_rule(fault_ctx_t* ctx, uint32_t rule_id) {
    if (!ctx || rule_id >= ctx->rule_count)
        return;

    /* 移动后续规则 */
    for (uint32_t i = rule_id; i < ctx->rule_count - 1; i++) {
        ctx->rules[i] = ctx->rules[i + 1];
    }
    ctx->rule_count--;
}

void fault_clear_rules(fault_ctx_t* ctx) {
    if (!ctx)
        return;
    ctx->rule_count          = 0;
    ctx->power_loss_armed    = 0;
    ctx->power_loss_at_write = 0;
    ctx->power_loss_at_byte  = 0;
}

void fault_set_rule_enabled(fault_ctx_t* ctx, uint32_t rule_id, int enabled) {
    if (!ctx || rule_id >= ctx->rule_count)
        return;
    ctx->rules[rule_id].enabled = enabled;
}

/* ═══════════════════════════════════════════════════════════════════════════
 *  §3  故障检查逻辑
 * ═══════════════════════════════════════════════════════════════════════════ */

/**
 * @brief 检查规则是否应该触发
 */
static int should_trigger_rule(fault_ctx_t* ctx, const fault_rule_t* rule, uint32_t op_count, uint32_t addr, size_t len) {
    (void)len; /* Reserved for pattern matching */

    if (!rule->enabled) {
        return 0;
    }

    switch (rule->trigger_mode) {
        case FAULT_TRIGGER_COUNT:
            /* 基于计数：第 N 次操作 */
            return (op_count == rule->trigger_count);

        case FAULT_TRIGGER_PROBABILITY:
            /* 基于概率：每次操作有 P% 概率 */
            return (prng_percent(&ctx->prng_state) < rule->probability_pct);

        case FAULT_TRIGGER_ADDRESS:
            /* 基于地址：特定地址范围 */
            return addr_in_range(addr, rule->addr_start, rule->addr_end);

        case FAULT_TRIGGER_PATTERN:
            /* 基于模式：暂不实现 */
            return 0;

        default:
            return 0;
    }
}

int fault_should_read_fail(fault_ctx_t* ctx, uint32_t addr, size_t len) {
    if (!ctx)
        return 0;

    ctx->read_count++;

    for (uint32_t i = 0; i < ctx->rule_count; i++) {
        fault_rule_t* rule = &ctx->rules[i];
        if (rule->type != FAULT_TYPE_READ_FAIL)
            continue;

        if (should_trigger_rule(ctx, rule, ctx->read_count, addr, len)) {
            ctx->fault_injected++;
            ctx->fault_by_type[FAULT_TYPE_READ_FAIL]++;
            return 1;
        }
    }

    return 0;
}

int fault_should_write_fail(fault_ctx_t* ctx, uint32_t addr, size_t len) {
    if (!ctx)
        return 0;

    ctx->write_count++;

    for (uint32_t i = 0; i < ctx->rule_count; i++) {
        fault_rule_t* rule = &ctx->rules[i];
        if (rule->type != FAULT_TYPE_WRITE_FAIL && rule->type != FAULT_TYPE_POWER_LOSS)
            continue;

        if (should_trigger_rule(ctx, rule, ctx->write_count, addr, len)) {
            ctx->fault_injected++;
            ctx->fault_by_type[rule->type]++;
            return 1;
        }
    }

    return 0;
}

int fault_should_erase_fail(fault_ctx_t* ctx, uint32_t addr) {
    if (!ctx)
        return 0;

    ctx->erase_count++;

    for (uint32_t i = 0; i < ctx->rule_count; i++) {
        fault_rule_t* rule = &ctx->rules[i];
        if (rule->type != FAULT_TYPE_ERASE_FAIL)
            continue;

        if (should_trigger_rule(ctx, rule, ctx->erase_count, addr, 0)) {
            ctx->fault_injected++;
            ctx->fault_by_type[FAULT_TYPE_ERASE_FAIL]++;
            return 1;
        }
    }

    return 0;
}

int fault_should_power_loss(fault_ctx_t* ctx, uint32_t addr, uint32_t offset) {
    (void)addr; /* Reserved for address-based power loss */

    if (!ctx || !ctx->power_loss_armed)
        return 0;

    if (ctx->write_count == ctx->power_loss_at_write && offset >= ctx->power_loss_at_byte) {
        ctx->fault_injected++;
        ctx->fault_by_type[FAULT_TYPE_POWER_LOSS]++;
        ctx->power_loss_armed = 0; /* 只触发一次 */
        return 1;
    }

    return 0;
}

void fault_inject_corruption(fault_ctx_t* ctx, uint8_t* mem, uint32_t addr, uint32_t len) {
    if (!ctx || !mem)
        return;

    for (uint32_t i = 0; i < ctx->rule_count; i++) {
        fault_rule_t* rule = &ctx->rules[i];
        if (!rule->enabled)
            continue;
        if (rule->type != FAULT_TYPE_DATA_CORRUPT)
            continue;

        if (rule->trigger_mode == FAULT_TRIGGER_ADDRESS) {
            /* 检查是否与损坏区域重叠 */
            uint32_t corrupt_start = rule->addr_start;
            uint32_t corrupt_end   = rule->addr_end;
            uint32_t data_start    = addr;
            uint32_t data_end      = addr + len;

            if (data_start < corrupt_end && data_end > corrupt_start) {
                /* 有重叠，注入损坏 */
                uint32_t overlap_start = (data_start > corrupt_start) ? data_start : corrupt_start;
                uint32_t overlap_end   = (data_end < corrupt_end) ? data_end : corrupt_end;

                for (uint32_t a = overlap_start; a < overlap_end; a++) {
                    mem[a] ^= ctx->corrupt_pattern; /* XOR 损坏 */
                }

                ctx->fault_injected++;
                ctx->fault_by_type[FAULT_TYPE_DATA_CORRUPT]++;
            }
        }
    }
}

/* ═══════════════════════════════════════════════════════════════════════════
 *  §4  便捷函数
 * ═══════════════════════════════════════════════════════════════════════════ */

void fault_quick_write_fail(fault_ctx_t* ctx, uint32_t nth) {
    fault_rule_t rule = {
        .type = FAULT_TYPE_WRITE_FAIL, .trigger_mode = FAULT_TRIGGER_COUNT, .trigger_count = nth, .enabled = 1};
    fault_add_rule(ctx, &rule);
}

void fault_quick_erase_fail(fault_ctx_t* ctx, uint32_t nth) {
    fault_rule_t rule = {
        .type = FAULT_TYPE_ERASE_FAIL, .trigger_mode = FAULT_TRIGGER_COUNT, .trigger_count = nth, .enabled = 1};
    fault_add_rule(ctx, &rule);
}

void fault_quick_write_fail_probability(fault_ctx_t* ctx, uint32_t pct) {
    if (pct > 100)
        pct = 100;

    fault_rule_t rule = {
        .type = FAULT_TYPE_WRITE_FAIL, .trigger_mode = FAULT_TRIGGER_PROBABILITY, .probability_pct = pct, .enabled = 1};
    fault_add_rule(ctx, &rule);
}

void fault_quick_power_loss(fault_ctx_t* ctx, uint32_t nth_write, uint32_t byte_offset) {
    ctx->power_loss_armed    = 1;
    ctx->power_loss_at_write = nth_write;
    ctx->power_loss_at_byte  = byte_offset;
}

void fault_quick_corrupt_data(fault_ctx_t* ctx, uint32_t addr, uint32_t len, uint8_t pattern) {
    ctx->corrupt_addr    = addr;
    ctx->corrupt_len     = len;
    ctx->corrupt_pattern = pattern;

    fault_rule_t rule = {.type = FAULT_TYPE_DATA_CORRUPT,
        .trigger_mode          = FAULT_TRIGGER_ADDRESS,
        .addr_start            = addr,
        .addr_end              = addr + len,
        .enabled               = 1};
    fault_add_rule(ctx, &rule);
}

void fault_quick_region_fail(fault_ctx_t* ctx, uint32_t addr_start, uint32_t addr_end, uint32_t pct) {
    if (pct > 100)
        pct = 100;

    fault_rule_t rule = {.type = FAULT_TYPE_WRITE_FAIL,
        .trigger_mode          = FAULT_TRIGGER_ADDRESS,
        .addr_start            = addr_start,
        .addr_end              = addr_end,
        .probability_pct       = pct,
        .enabled               = 1};
    fault_add_rule(ctx, &rule);
}

/* ═══════════════════════════════════════════════════════════════════════════
 *  §5  统计与调试
 * ═══════════════════════════════════════════════════════════════════════════ */

void fault_get_stats(fault_ctx_t* ctx, uint32_t ops[3], uint32_t faults[8]) {
    if (!ctx)
        return;

    if (ops) {
        ops[0] = ctx->read_count;
        ops[1] = ctx->write_count;
        ops[2] = ctx->erase_count;
    }

    if (faults) {
        for (int i = 0; i < 8; i++) {
            faults[i] = ctx->fault_by_type[i];
        }
    }
}

void fault_reset_stats(fault_ctx_t* ctx) {
    if (!ctx)
        return;

    ctx->read_count     = 0;
    ctx->write_count    = 0;
    ctx->erase_count    = 0;
    ctx->fault_injected = 0;
    memset(ctx->fault_by_type, 0, sizeof(ctx->fault_by_type));
}

void fault_print_report(fault_ctx_t* ctx) {
    if (!ctx)
        return;

    printf("\n========================================\n");
    printf("Fault Injection Report\n");
    printf("========================================\n");

    printf("\nOperations:\n");
    printf("  Reads:   %u\n", ctx->read_count);
    printf("  Writes:  %u\n", ctx->write_count);
    printf("  Erases:  %u\n", ctx->erase_count);

    printf("\nFaults Injected: %u\n", ctx->fault_injected);
    printf("  READ_FAIL:     %u\n", ctx->fault_by_type[FAULT_TYPE_READ_FAIL]);
    printf("  WRITE_FAIL:    %u\n", ctx->fault_by_type[FAULT_TYPE_WRITE_FAIL]);
    printf("  ERASE_FAIL:    %u\n", ctx->fault_by_type[FAULT_TYPE_ERASE_FAIL]);
    printf("  POWER_LOSS:    %u\n", ctx->fault_by_type[FAULT_TYPE_POWER_LOSS]);
    printf("  DATA_CORRUPT:  %u\n", ctx->fault_by_type[FAULT_TYPE_DATA_CORRUPT]);
    printf("  BIT_FLIP:      %u\n", ctx->fault_by_type[FAULT_TYPE_BIT_FLIP]);

    printf("\nActive Rules: %u\n", ctx->rule_count);
    for (uint32_t i = 0; i < ctx->rule_count; i++) {
        fault_rule_t* r = &ctx->rules[i];
        printf("  [%u] type=%d, mode=%d, %s\n", i, r->type, r->trigger_mode, r->enabled ? "ENABLED" : "DISABLED");
    }

    printf("========================================\n");
}

int fault_export_rules(fault_ctx_t* ctx, char* buf, size_t len) {
    if (!ctx || !buf || len == 0)
        return 0;

    int offset = 0;
    offset += snprintf(buf + offset, len - offset, "# Fault rules (count=%u)\n", ctx->rule_count);

    for (uint32_t i = 0; i < ctx->rule_count; i++) {
        fault_rule_t* r = &ctx->rules[i];
        offset += snprintf(buf + offset, len - offset, "%d,%d,%u,%u,%u,%u,%u,%d\n", r->type, r->trigger_mode,
            r->trigger_count, r->probability_pct, r->addr_start, r->addr_end, r->seed, r->enabled);
        if ((size_t)offset >= len - 1)
            break;
    }

    return offset;
}

int fault_import_rules(fault_ctx_t* ctx, const char* str) {
    if (!ctx || !str)
        return 0;

    int         imported = 0;
    const char* p        = str;

    while (*p) {
        /* Skip leading whitespace and blank lines */
        while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n')
            p++;
        if (!*p)
            break;

        /* Skip comment lines */
        if (*p == '#') {
            while (*p && *p != '\n')
                p++;
            continue;
        }

        /* Parse: type,mode,trigger_count,probability_pct,addr_start,addr_end,seed,enabled */
        fault_rule_t rule;
        int          type    = 0;
        int          mode    = 0;
        int          enabled = 0;
        int          n       = 0;
        int fields = sscanf(p, "%d,%d,%u,%u,%u,%u,%u,%d%n", &type, &mode, &rule.trigger_count, &rule.probability_pct,
            &rule.addr_start, &rule.addr_end, &rule.seed, &enabled, &n);
        if (fields < 8)
            break; /* malformed line, stop */

        if (type < 0 || type > FAULT_TYPE_BIT_FLIP || mode < 0 || mode > FAULT_TRIGGER_PATTERN) {
            break;
        }

        rule.type         = (fault_type_t)type;
        rule.trigger_mode = (fault_trigger_mode_t)mode;
        rule.enabled      = enabled;

        if (fault_add_rule(ctx, &rule) >= 0)
            imported++;

        p += n;
    }

    return imported;
}
