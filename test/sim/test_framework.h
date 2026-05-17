/**
 * test_framework.h - RocketDB 测试框架
 * 
 * 功能：
 * - 测试用例注册与自动发现
 * - 参数化测试支持
 * - 丰富的断言宏库
 * - 测试统计收集
 * - 结构化的测试报告
 * 
 * Copyright (c) 2026 RocketDB Contributors
 * SPDX-License-Identifier: MIT
 */

#ifndef TEST_FRAMEWORK_H
#define TEST_FRAMEWORK_H

#include <stdint.h>
#include <stddef.h>
#include <stdio.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ═══════════════════════════════════════════════════════════════════════════
 *  §1  测试用例定义
 * ═══════════════════════════════════════════════════════════════════════════ */

/**
 * @brief 测试用例函数签名
 * @param ctx  用户上下文指针（用于传递 db、flash 等环境）
 * @return     0 表示成功，非零表示失败
 */
typedef int (*test_case_fn_t)(void *ctx);

/**
 * @brief 测试用例描述符
 */
typedef struct test_case {
    const char       *name;        /**< 测试用例名称（如 "TC-KV-01"） */
    const char       *description; /**< 测试用例描述 */
    test_case_fn_t    func;        /**< 测试函数指针 */
    const char       *category;    /**< 测试分类（如 "KVDB", "TSDB"） */
    int               enabled;     /**< 是否启用（1=启用，0=跳过） */
    struct test_case *next;        /**< 链表指针 */
} test_case_t;

/**
 * @brief 测试套件（一组相关测试用例的集合）
 */
typedef struct test_suite {
    const char    *name;           /**< 套件名称 */
    test_case_t   *first_case;     /**< 第一个测试用例 */
    test_case_t   *last_case;      /**< 最后一个测试用例 */
    uint32_t       case_count;     /**< 测试用例数量 */
} test_suite_t;

/**
 * @brief 测试运行器配置
 */
typedef struct test_config test_config_t;

/**
 * @brief 测试后回调函数类型
 * @param name   测试用例名称
 * @param result 测试结果（0=通过，非0=失败）
 * @param ctx    用户上下文指针
 */
typedef void (*test_post_hook_fn)(const char *name, int result, void *ctx);

struct test_config {
    FILE             *log_file;       /**< 日志输出文件（NULL=不输出文件日志） */
    int               verbose;        /**< 详细模式（1=详细，0=简洁） */
    int               stop_on_fail;   /**< 遇到失败即停止（1=停止，0=继续） */
    const char       *filter;         /**< 测试过滤器（NULL=运行所有，否则匹配名称） */
    test_post_hook_fn post_test_hook; /**< 每个测试用例后调用（NULL=不调用） */
    void             *hook_ctx;       /**< 传递给 post_test_hook 的上下文 */
};

/* ═══════════════════════════════════════════════════════════════════════════
 *  §2  测试统计
 * ═══════════════════════════════════════════════════════════════════════════ */

/**
 * @brief 测试统计数据
 */
typedef struct {
    uint32_t total_cases;          /**< 总测试用例数 */
    uint32_t passed_cases;         /**< 通过的用例数 */
    uint32_t failed_cases;         /**< 失败的用例数 */
    uint32_t skipped_cases;        /**< 跳过的用例数 */
    
    uint32_t total_assertions;     /**< 总断言数 */
    uint32_t passed_assertions;    /**< 通过的断言数 */
    uint32_t failed_assertions;    /**< 失败的断言数 */
    
    uint32_t start_time_ms;        /**< 测试开始时间（毫秒） */
    uint32_t end_time_ms;          /**< 测试结束时间（毫秒） */
} test_stats_t;

/**
 * @brief 当前测试用例的运行上下文
 */
typedef struct {
    const char    *current_case_name;     /**< 当前运行的测试用例名称 */
    uint32_t       current_case_asserts;  /**< 当前用例的断言数 */
    uint32_t       current_case_failures; /**< 当前用例的失败数 */
    int            test_failed;           /**< 当前测试是否已失败 */
} test_context_t;

/* ═══════════════════════════════════════════════════════════════════════════
 *  §3  断言宏库
 * ═══════════════════════════════════════════════════════════════════════════ */

/**
 * @brief 基础断言宏（内部使用）
 */
#define TEST_ASSERT_IMPL(cond, msg) \
    do { \
        test_framework_assert((cond), (msg), __FILE__, __LINE__); \
    } while (0)

/**
 * @brief 断言条件为真
 */
#define TEST_ASSERT(cond) \
    TEST_ASSERT_IMPL((cond), #cond)

/**
 * @brief 断言条件为真，带自定义消息
 */
#define TEST_ASSERT_MSG(cond, msg) \
    TEST_ASSERT_IMPL((cond), msg)

/**
 * @brief 断言两个整数相等
 */
#define TEST_ASSERT_EQ(a, b) \
    TEST_ASSERT_IMPL((a) == (b), #a " == " #b)

/**
 * @brief 断言两个整数不相等
 */
#define TEST_ASSERT_NE(a, b) \
    TEST_ASSERT_IMPL((a) != (b), #a " != " #b)

/**
 * @brief 断言 a < b
 */
#define TEST_ASSERT_LT(a, b) \
    TEST_ASSERT_IMPL((a) < (b), #a " < " #b)

/**
 * @brief 断言 a <= b
 */
#define TEST_ASSERT_LE(a, b) \
    TEST_ASSERT_IMPL((a) <= (b), #a " <= " #b)

/**
 * @brief 断言 a > b
 */
#define TEST_ASSERT_GT(a, b) \
    TEST_ASSERT_IMPL((a) > (b), #a " > " #b)

/**
 * @brief 断言 a >= b
 */
#define TEST_ASSERT_GE(a, b) \
    TEST_ASSERT_IMPL((a) >= (b), #a " >= " #b)

/**
 * @brief 断言指针为 NULL
 */
#define TEST_ASSERT_NULL(ptr) \
    TEST_ASSERT_IMPL((ptr) == NULL, #ptr " is NULL")

/**
 * @brief 断言指针不为 NULL
 */
#define TEST_ASSERT_NOT_NULL(ptr) \
    TEST_ASSERT_IMPL((ptr) != NULL, #ptr " is not NULL")

/**
 * @brief 断言内存块相等
 */
#define TEST_ASSERT_MEM_EQ(a, b, len) \
    TEST_ASSERT_IMPL(memcmp((a), (b), (len)) == 0, "memory blocks equal")

/**
 * @brief 断言字符串相等
 */
#define TEST_ASSERT_STR_EQ(a, b) \
    TEST_ASSERT_IMPL(strcmp((a), (b)) == 0, "strings equal")

/**
 * @brief 断言 RocketDB 错误码为 RDB_OK
 */
#define TEST_ASSERT_RDB_OK(expr) \
    TEST_ASSERT_IMPL((expr) == RDB_OK, #expr " == RDB_OK")

/**
 * @brief 断言 RocketDB 错误码为指定值
 */
#define TEST_ASSERT_RDB_ERR(expr, err) \
    TEST_ASSERT_IMPL((expr) == (err), #expr " == " #err)

/* ═══════════════════════════════════════════════════════════════════════════
 *  §4  测试用例注册
 * ═══════════════════════════════════════════════════════════════════════════ */

/**
 * @brief 注册一个测试用例
 * @param suite      目标测试套件
 * @param test_case  测试用例描述符
 */
void test_register_case(test_suite_t *suite, test_case_t *test_case);

/**
 * @brief 创建测试用例（辅助宏）
 * 
 * 用法：
 *   TEST_CASE(kvdb_basic_set_get, "KVDB", "Basic set/get operations") {
 *       // 测试代码
 *       TEST_ASSERT(rdb_kvdb_set(db, "key", "val", 3) == RDB_OK);
 *       return 0;
 *   }
 */
#define TEST_CASE(fn_name, category, description) \
    static int test_func_##fn_name(void *ctx); \
    static test_case_t test_case_##fn_name = { \
        #fn_name, \
        (description), \
        test_func_##fn_name, \
        (category), \
        1, \
        NULL \
    }; \
    static int test_func_##fn_name(void *ctx)

/**
 * @brief 自动注册测试用例到全局套件
 * 必须在定义 TEST_CASE 后调用
 */
#define REGISTER_TEST(fn_name) \
    __attribute__((constructor)) \
    static void register_##fn_name(void) { \
        test_register_case(test_get_default_suite(), &test_case_##fn_name); \
    }

/* ═══════════════════════════════════════════════════════════════════════════
 *  §5  参数化测试支持
 * ═══════════════════════════════════════════════════════════════════════════ */

/**
 * @brief 参数化测试配置
 */
typedef struct {
    uint8_t  write_granularity;    /**< 写入粒度：0=1B, 1=2B, 2=4B, 3=8B */
    uint32_t sector_size;          /**< 扇区大小 */
    uint32_t sector_count;         /**< 扇区数量 */
    uint16_t max_record_size;      /**< 最大记录大小 */
} test_params_t;

/**
 * @brief 参数化测试函数签名
 */
typedef int (*param_test_fn_t)(void *ctx, const test_params_t *params);

/**
 * @brief 运行参数化测试
 * @param name    测试名称
 * @param func    测试函数
 * @param ctx     用户上下文
 * @param params  参数数组
 * @param count   参数数量
 * @return        失败的测试数量
 */
int test_run_parameterized(const char *name, param_test_fn_t func, 
                           void *ctx, const test_params_t *params, 
                           uint32_t count);

/* ═══════════════════════════════════════════════════════════════════════════
 *  §6  测试运行器
 * ═══════════════════════════════════════════════════════════════════════════ */

/**
 * @brief 初始化测试框架
 * @param config  测试配置（NULL=使用默认配置）
 */
void test_framework_init(const test_config_t *config);

/**
 * @brief 运行指定测试套件
 * @param suite  测试套件
 * @param ctx    用户上下文（传递给所有测试用例）
 * @return       失败的测试用例数量
 */
int test_run_suite(test_suite_t *suite, void *ctx);

/**
 * @brief 运行所有已注册的测试
 * @param ctx  用户上下文
 * @return     失败的测试用例数量
 */
int test_run_all(void *ctx);

/**
 * @brief 获取测试统计信息
 * @param stats  输出统计数据
 */
void test_get_stats(test_stats_t *stats);

/**
 * @brief 打印测试报告
 */
void test_print_report(void);

/**
 * @brief 获取默认测试套件（用于自动注册）
 */
test_suite_t *test_get_default_suite(void);

/**
 * @brief 断言实现（内部函数，由宏调用）
 */
void test_framework_assert(int condition, const char *msg, 
                          const char *file, int line);

/**
 * @brief 重置测试统计（用于多次运行）
 */
void test_reset_stats(void);

/**
 * @brief 生成带时间戳的日志文件路径
 * @param name  测试名称（不含路径和后缀，如 "kvdb_basic"）
 * @return      静态缓冲区指针，格式 "test/out/YYMMDD-HHMMSS-name.log"
 *
 * 调用者不应释放返回的指针。每次调用会覆盖上一次的结果。
 * 如果 test/out/ 目录不存在，会自动创建。
 */
const char *test_make_log_path(const char *name);

#ifdef __cplusplus
}
#endif

#endif /* TEST_FRAMEWORK_H */
