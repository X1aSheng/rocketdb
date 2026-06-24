/**
 * test_example.c - 测试框架使用示例
 *
 * 演示如何使用新的测试框架编写测试用例
 */

#include "../../src/rocketdb.h"
#include "sim_flash.h"
#include "test_framework.h"
#include <stdio.h>
#include <string.h>

/* 测试环境上下文 */
typedef struct {
    sim_flash_t* flash;
    rdb_kvdb_t*  kvdb;
    rdb_tsdb_t*  tsdb;
} test_env_t;

#define EX_FLASH_SIZE  (64u * 1024u)
#define EX_SECTOR_SIZE 4096u
#define EX_SECTOR_CNT  (EX_FLASH_SIZE / EX_SECTOR_SIZE)

static uint8_t     g_flash_buf[EX_FLASH_SIZE];
static sim_flash_t g_flash;
/* Meta buffer sized for sector metadata + bloom filter bitmaps */
#define META_BUF_SZ (EX_SECTOR_CNT * (sizeof(rdb_kv_sector_meta_t) + RDB_BLOOM_BYTES))
static uint8_t               g_kv_meta_buf[META_BUF_SZ];
static rdb_kv_sector_meta_t* g_kv_meta = (rdb_kv_sector_meta_t*)g_kv_meta_buf;
static rdb_partition_t       g_part;
static rdb_kvdb_t            g_kvdb;
static trace_ctx_t           g_trace;

static int ex_read(void* ctx, uint32_t addr, uint8_t* buf, size_t len) {
    (void)ctx;
    return sim_flash_read(&g_flash, addr, buf, len);
}
static int ex_write(void* ctx, uint32_t addr, const uint8_t* buf, size_t len) {
    (void)ctx;
    return sim_flash_write(&g_flash, addr, buf, len);
}
static int ex_erase(void* ctx, uint32_t addr) {
    (void)ctx;
    return sim_flash_erase(&g_flash, addr);
}
static void ex_lock(void* ctx) {
    (void)ctx;
}
static void ex_unlock(void* ctx) {
    (void)ctx;
}
static void ex_yield(void* ctx) {
    (void)ctx;
}

static rdb_flash_ops_t g_ops = {
    .read = ex_read, .write = ex_write, .erase = ex_erase, .lock = ex_lock, .unlock = ex_unlock, .yield = ex_yield};

/* ── Trace wrapper ─────────────────────────────────────────────────── */
static rdb_err_t trace_kv_set(rdb_kvdb_t* db, const char* key, const void* val, uint16_t vlen) {
    trace_event(&g_trace, "  [KV-WRITE] key=%s vsz=%u", key, (unsigned)vlen);
    return rdb_kvdb_set(db, key, (const uint8_t*)val, vlen);
}

/* ═══════════════════════════════════════════════════════════════════════════
 *  示例 1: 基础测试用例
 * ═══════════════════════════════════════════════════════════════════════════ */

TEST_CASE(example_basic_assert, "Example", "Basic assertion examples") {
    test_env_t* env = (test_env_t*)ctx;
    (void)env; /* 未使用参数 */

    /* 基础断言 */
    TEST_ASSERT(1 + 1 == 2);
    TEST_ASSERT_MSG(2 * 2 == 4, "2 times 2 equals 4");

    /* 整数比较 */
    TEST_ASSERT_EQ(100, 100);
    TEST_ASSERT_NE(100, 200);
    TEST_ASSERT_LT(1, 2);
    TEST_ASSERT_LE(2, 2);
    TEST_ASSERT_GT(10, 5);
    TEST_ASSERT_GE(10, 10);

    /* 指针断言 */
    int  x   = 42;
    int* ptr = &x;
    TEST_ASSERT_NOT_NULL(ptr);
    TEST_ASSERT_NULL(NULL);

    /* 内存比较 */
    uint8_t buf1[4] = {1, 2, 3, 4};
    uint8_t buf2[4] = {1, 2, 3, 4};
    TEST_ASSERT_MEM_EQ(buf1, buf2, 4);

    /* 字符串比较 */
    TEST_ASSERT_STR_EQ("hello", "hello");

    return 0;
}

/* 注册测试用例到默认套件（可选，如果使用手动注册方式） */
/* REGISTER_TEST(example_basic_assert); */

/* ═══════════════════════════════════════════════════════════════════════════
 *  示例 2: KVDB 测试用例
 * ═══════════════════════════════════════════════════════════════════════════ */

TEST_CASE(example_kvdb_set_get, "KVDB", "KVDB set/get operations") {
    test_env_t* env = (test_env_t*)ctx;
    rdb_kvdb_t* db  = env->kvdb;

    const char* key = "test_key";
    uint8_t     val_write[64];
    uint8_t     val_read[64];
    uint16_t    out_len;

    memset(val_write, 0xAA, sizeof(val_write));

    /* 测试 set */
    rdb_err_t ret = trace_kv_set(db, key, val_write, sizeof(val_write));
    TEST_ASSERT_RDB_OK(ret);

    /* 测试 get */
    memset(val_read, 0, sizeof(val_read));
    ret = rdb_kvdb_get(db, key, val_read, sizeof(val_read), &out_len);
    TEST_ASSERT_RDB_OK(ret);
    TEST_ASSERT_EQ(out_len, sizeof(val_write));
    TEST_ASSERT_MEM_EQ(val_read, val_write, sizeof(val_write));

    /* 测试 exists */
    TEST_ASSERT_RDB_OK(rdb_kvdb_exists(db, key));

    /* 测试 delete */
    ret = rdb_kvdb_delete(db, key);
    TEST_ASSERT_RDB_OK(ret);

    /* 验证已删除 */
    ret = rdb_kvdb_get(db, key, val_read, sizeof(val_read), &out_len);
    TEST_ASSERT_RDB_ERR(ret, RDB_ERR_NOT_FOUND);

    return 0;
}

/* ═══════════════════════════════════════════════════════════════════════════
 *  示例 3: 参数化测试
 * ═══════════════════════════════════════════════════════════════════════════ */

static int example_param_write_gran_test(void* ctx, const test_params_t* params) {
    test_env_t* env = (test_env_t*)ctx;
    (void)env;

    /* 使用不同的参数运行测试 */
    uint8_t  gran     = params->write_granularity;
    uint32_t sec_size = params->sector_size;

    TEST_ASSERT_LT(gran, 4);        /* 写粒度 0-3 */
    TEST_ASSERT_GE(sec_size, 4096); /* 扇区至少 4KB */

    /* 这里可以使用参数进行实际测试 */
    printf("    Testing with gran=%u, sec_size=%u\n", gran, sec_size);

    return 0;
}

/* ═══════════════════════════════════════════════════════════════════════════
 *  主测试入口
 * ═══════════════════════════════════════════════════════════════════════════ */

static void post_test_example_sectors(const char* name, int result, void* ctx) {
    (void)name;
    (void)result;
    (void)ctx;
    trace_kvdb_sector_summary(&g_trace, &g_kvdb);
}

int main(void) {
    /* 初始化模拟 Flash */
    sim_flash_init(&g_flash, g_flash_buf, EX_FLASH_SIZE, EX_SECTOR_SIZE, 256, 0);

    g_part = (rdb_partition_t) {.name = "example",
        .base_addr                    = 0,
        .total_size                   = EX_FLASH_SIZE,
        .sector_size                  = EX_SECTOR_SIZE,
        .write_gran                   = 0,
        .ops                          = &g_ops};

    /* 初始化 KVDB */
    g_kvdb.part       = &g_part;
    g_kvdb.sectors    = g_kv_meta;
    g_kvdb.sector_cnt = (uint8_t)EX_SECTOR_CNT;
    rdb_kvdb_format(&g_kvdb);
    rdb_kvdb_init(&g_kvdb, &g_part, g_kv_meta);

    /* 初始化测试环境 */
    test_env_t env = {.flash = &g_flash, .kvdb = &g_kvdb, .tsdb = NULL};

    /* 配置测试框架 */
    test_config_t config = {.log_file = fopen(test_make_log_path("example"), "w"),
        .verbose                      = 1,
        .stop_on_fail                 = 0,
        .filter                       = NULL, /* NULL = 运行所有测试 */
        .post_test_hook               = post_test_example_sectors,
        .hook_ctx                     = NULL};

    test_framework_init(&config);

    trace_init(&g_trace, config.log_file, config.verbose);
    sim_flash_set_trace(&g_flash, &g_trace);
    trace_event(&g_trace, "=== Example Test Suite Start ===");

    /* 手动注册测试用例（如果不使用 REGISTER_TEST 宏） */
    test_suite_t* suite = test_get_default_suite();
    test_register_case(suite, &test_case_example_basic_assert);
    test_register_case(suite, &test_case_example_kvdb_set_get);

    /* 运行所有测试 */
    printf("Running example tests...\n");
    test_run_all(&env);

    trace_event(&g_trace, "=== Example Test Suite End ===\n");

    /* 运行参数化测试 */
    printf("\n\nRunning parameterized tests...\n");
    test_params_t params[] = {
        {.write_granularity = 0, .sector_size = 4096, .sector_count = 16, .max_record_size = 256},
        {.write_granularity = 1, .sector_size = 4096, .sector_count = 16, .max_record_size = 512},
        {.write_granularity = 2, .sector_size = 8192, .sector_count = 32, .max_record_size = 1024},
        {.write_granularity = 3, .sector_size = 8192, .sector_count = 32, .max_record_size = 2048},
    };

    test_run_parameterized("example_param_write_gran", example_param_write_gran_test, &env, params, 4);

    /* 打印测试报告 */
    test_print_report();

    /* 清理 */
    if (config.log_file) {
        fclose(config.log_file);
    }

    /* 获取统计信息并返回退出码 */
    test_stats_t stats;
    test_get_stats(&stats);

    return (stats.failed_cases == 0) ? 0 : 1;
}
