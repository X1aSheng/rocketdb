/**
 * test_framework.c - RocketDB 测试框架实现
 *
 * Copyright (c) 2026 RocketDB Contributors
 * SPDX-License-Identifier: MIT
 */

#include "test_framework.h"
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* ═══════════════════════════════════════════════════════════════════════════
 *  §1  全局状态
 * ═══════════════════════════════════════════════════════════════════════════ */

static test_suite_t g_default_suite = {
    .name = "Default Test Suite", .first_case = NULL, .last_case = NULL, .case_count = 0};

static test_config_t g_config = {.log_file = NULL, .verbose = 1, .stop_on_fail = 0, .filter = NULL};

static test_stats_t   g_stats = {0};
static test_context_t g_ctx   = {0};

/* ═══════════════════════════════════════════════════════════════════════════
 *  §2  辅助函数
 * ═══════════════════════════════════════════════════════════════════════════ */

/**
 * @brief 获取当前时间（毫秒）
 */
static uint32_t get_time_ms(void) {
    return (uint32_t)(clock() * 1000 / CLOCKS_PER_SEC);
}

/**
 * @brief 输出日志（同时输出到终端和文件）
 */
static void log_output(const char* fmt, ...) {
    va_list ap;

    va_start(ap, fmt);

    /* Copy before consuming ap — va_copy from a consumed va_list is UB (C99 §7.15) */
    if (g_config.log_file) {
        va_list ap_copy;
        va_copy(ap_copy, ap);
        vfprintf(g_config.log_file, fmt, ap_copy);
        va_end(ap_copy);
        fflush(g_config.log_file);
    }

    /* Output to terminal */
    vprintf(fmt, ap);

    va_end(ap);
}

/**
 * @brief 检查测试名称是否匹配过滤器
 */
static int name_matches_filter(const char* name) {
    if (!g_config.filter) {
        return 1; /* 无过滤器，匹配所有 */
    }
    return strstr(name, g_config.filter) != NULL;
}

/* ═══════════════════════════════════════════════════════════════════════════
 *  §3  测试用例注册
 * ═══════════════════════════════════════════════════════════════════════════ */

void test_register_case(test_suite_t* suite, test_case_t* test_case) {
    if (!suite || !test_case) {
        return;
    }

    /* 添加到链表末尾 */
    if (suite->last_case) {
        suite->last_case->next = test_case;
    } else {
        suite->first_case = test_case;
    }

    suite->last_case = test_case;
    suite->case_count++;
    test_case->next = NULL;
}

test_suite_t* test_get_default_suite(void) {
    return &g_default_suite;
}

/* ═══════════════════════════════════════════════════════════════════════════
 *  §4  断言实现
 * ═══════════════════════════════════════════════════════════════════════════ */

void test_framework_assert(int condition, const char* msg, const char* file, int line) {
    g_stats.total_assertions++;
    g_ctx.current_case_asserts++;

    if (condition) {
        g_stats.passed_assertions++;
        if (g_config.verbose >= 2) {
            log_output("  [PASS] %s\n", msg);
        } else {
            printf(".");
        }
    } else {
        g_stats.failed_assertions++;
        g_ctx.current_case_failures++;
        g_ctx.test_failed = 1;

        log_output("\n  [FAIL] %s\n", msg);
        log_output("         at %s:%d\n", file, line);

        if (g_config.stop_on_fail) {
            log_output("\n[STOP] Test execution stopped due to failure.\n");
            test_print_report();
            exit(1);
        }
    }
}

/* ═══════════════════════════════════════════════════════════════════════════
 *  §5  测试运行器
 * ═══════════════════════════════════════════════════════════════════════════ */

void test_framework_init(const test_config_t* config) {
    if (config) {
        g_config = *config;
    }

    memset(&g_stats, 0, sizeof(g_stats));
    memset(&g_ctx, 0, sizeof(g_ctx));

    g_stats.start_time_ms = get_time_ms();
}

int test_run_suite(test_suite_t* suite, void* ctx) {
    if (!suite) {
        return -1;
    }

    log_output("\n========================================\n");
    log_output("Running test suite: %s\n", suite->name);
    log_output("========================================\n");

    test_case_t* tc = suite->first_case;
    while (tc) {
        g_stats.total_cases++;

        /* 检查是否启用 */
        if (!tc->enabled) {
            g_stats.skipped_cases++;
            log_output("[SKIP] %s (%s)\n", tc->name, tc->category);
            tc = tc->next;
            continue;
        }

        /* 检查过滤器 */
        if (!name_matches_filter(tc->name)) {
            g_stats.skipped_cases++;
            tc = tc->next;
            continue;
        }

        /* 运行测试用例 */
        g_ctx.current_case_name     = tc->name;
        g_ctx.current_case_asserts  = 0;
        g_ctx.current_case_failures = 0;
        g_ctx.test_failed           = 0;

        if (g_config.verbose) {
            log_output("\n[TEST] %s - %s\n", tc->name, tc->description);
        }

        int result = tc->func(ctx);

        if (result != 0 || g_ctx.test_failed) {
            g_stats.failed_cases++;
            if (!g_config.verbose) {
                log_output("\n[FAIL] %s\n", tc->name);
            }
        } else {
            g_stats.passed_cases++;
            if (g_config.verbose) {
                log_output("[OK] %s (%u assertions)\n", tc->name, g_ctx.current_case_asserts);
            }
        }

        /* Post-test hook for tracing */
        if (g_config.post_test_hook) {
            g_config.post_test_hook(tc->name, result, g_config.hook_ctx);
        }

        tc = tc->next;
    }

    return (int)g_stats.failed_cases;
}

int test_run_all(void* ctx) {
    return test_run_suite(&g_default_suite, ctx);
}

void test_get_stats(test_stats_t* stats) {
    if (stats) {
        *stats             = g_stats;
        stats->end_time_ms = get_time_ms();
    }
}

void test_reset_stats(void) {
    memset(&g_stats, 0, sizeof(g_stats));
    memset(&g_ctx, 0, sizeof(g_ctx));
    g_stats.start_time_ms = get_time_ms();
}

void test_print_report(void) {
    g_stats.end_time_ms = get_time_ms();
    uint32_t elapsed    = g_stats.end_time_ms - g_stats.start_time_ms;

    log_output("\n\n");
    log_output("========================================\n");
    log_output("Test Report\n");
    log_output("========================================\n");

    log_output("\nTest Cases:\n");
    log_output("  Total:   %u\n", g_stats.total_cases);
    log_output("  Passed:  %u\n", g_stats.passed_cases);
    log_output("  Failed:  %u\n", g_stats.failed_cases);
    log_output("  Skipped: %u\n", g_stats.skipped_cases);

    log_output("\nAssertions:\n");
    log_output("  Total:   %u\n", g_stats.total_assertions);
    log_output("  Passed:  %u\n", g_stats.passed_assertions);
    log_output("  Failed:  %u\n", g_stats.failed_assertions);

    log_output("\nTime:\n");
    log_output("  Elapsed: %u ms\n", elapsed);

    if (g_stats.failed_cases == 0 && g_stats.failed_assertions == 0) {
        log_output("\n✓ ALL TESTS PASSED\n");
    } else {
        log_output("\n✗ SOME TESTS FAILED\n");
    }

    log_output("========================================\n");
}

/* ═══════════════════════════════════════════════════════════════════════════
 *  §6  参数化测试
 * ═══════════════════════════════════════════════════════════════════════════ */

int test_run_parameterized(
    const char* name, param_test_fn_t func, void* ctx, const test_params_t* params, uint32_t count) {
    if (!name || !func || !params) {
        return -1;
    }

    log_output("\n[PARAM] Running parameterized test: %s\n", name);

    uint32_t failed = 0;

    for (uint32_t i = 0; i < count; i++) {
        g_ctx.current_case_name     = name;
        g_ctx.current_case_asserts  = 0;
        g_ctx.current_case_failures = 0;
        g_ctx.test_failed           = 0;

        log_output("  [%u/%u] gran=%u, sec_sz=%u, sec_cnt=%u, max_rec=%u\n", i + 1, count, params[i].write_granularity,
            params[i].sector_size, params[i].sector_count, params[i].max_record_size);

        int result = func(ctx, &params[i]);

        if (result != 0 || g_ctx.test_failed) {
            failed++;
            log_output("    [FAIL] Parameter set %u\n", i);
        } else {
            log_output("    [PASS] Parameter set %u (%u assertions)\n", i, g_ctx.current_case_asserts);
        }
    }

    if (failed == 0) {
        log_output("  [OK] All %u parameter sets passed\n", count);
    } else {
        log_output("  [FAIL] %u out of %u parameter sets failed\n", failed, count);
    }

    return (int)failed;
}

/* ═══════════════════════════════════════════════════════════════════════════
 *  §7  日志路径生成
 * ═══════════════════════════════════════════════════════════════════════════ */

const char* test_make_log_path(const char* name) {
    /* Not thread-safe — returns pointer to static buffer.
     * Callers must consume the result before the next call. */
    static char path[128];
    time_t      now     = time(NULL);
    struct tm*  tm_info = localtime(&now);
    char        ts[16];
    strftime(ts, sizeof(ts), "%y%m%d-%H%M%S", tm_info);
    snprintf(path, sizeof(path), "tests/out/%s-%s.log", ts, name);
    return path;
}
