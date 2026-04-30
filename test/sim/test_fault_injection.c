/**
 * test_fault_injection.c - 故障注入功能演示
 * 
 * 演示如何使用故障注入系统测试 RocketDB 的鲁棒性
 */

#include "sim_flash.h"
#include "sim_fault.h"
#include "test_framework.h"
#include "../../src/rocketdb.h"
#include <stdio.h>
#include <string.h>

#define FLASH_SIZE      (128u * 1024u)
#define SECTOR_SIZE     4096u
#define KVDB_PART_SIZE  (64u * 1024u)

static uint8_t g_flash_buf[FLASH_SIZE];
static sim_flash_t g_flash;
static fault_ctx_t g_fault_ctx;

/* Flash 回调函数 */
static int fl_read(uint32_t addr, uint8_t *buf, size_t len) {
    return sim_flash_read(&g_flash, addr, buf, len);
}
static int fl_write(uint32_t addr, const uint8_t *buf, size_t len) {
    return sim_flash_write(&g_flash, addr, buf, len);
}
static int fl_erase(uint32_t addr) {
    return sim_flash_erase(&g_flash, addr);
}
static void fl_lock(void) { }
static void fl_unlock(void) { }
static void fl_yield(void) { }

static rdb_flash_ops_t g_ops = {
    .read = fl_read,
    .write = fl_write,
    .erase = fl_erase,
    .lock = fl_lock,
    .unlock = fl_unlock,
    .yield = fl_yield
};

/* ═══════════════════════════════════════════════════════════════════════════
 *  测试用例 1：写入失败注入
 * ═══════════════════════════════════════════════════════════════════════════ */

TEST_CASE(fault_write_fail_nth, "Fault", "Write fails on Nth operation")
{
    (void)ctx;
    
    printf("\n[Fault Test 1] Write fails on 5th operation\n");
    
    /* 初始化 Flash */
    sim_flash_init(&g_flash, g_flash_buf, FLASH_SIZE, SECTOR_SIZE, 256, 0);
    
    /* 配置故障：第 5 次写入失败 */
    fault_init(&g_fault_ctx, 0x12345);
    fault_quick_write_fail(&g_fault_ctx, 5);
    sim_flash_set_fault_ctx(&g_flash, &g_fault_ctx);
    
    /* 初始化 KVDB */
    rdb_partition_t part = {
        .name = "test",
        .base_addr = 0,
        .total_size = KVDB_PART_SIZE,
        .sector_size = SECTOR_SIZE,
        .write_gran = 0,
        .ops = &g_ops
    };
    
    uint8_t meta_buf[512];
    rdb_kvdb_t db;
    db.part = &part;
    db.sectors = (rdb_kv_sector_meta_t*)meta_buf;
    rdb_kvdb_format(&db);
    rdb_err_t ret = rdb_kvdb_init(&db, &part, meta_buf);
    TEST_ASSERT_RDB_OK(ret);

    /* 尝试多次写入，观察第 5 次失败 */
    int fail_count = 0;
    for (int i = 0; i < 10; i++) {
        char key[16];
        snprintf(key, sizeof(key), "key%d", i);
        ret = rdb_kvdb_set(&db, key, "value", 5);

        if (ret != RDB_OK) {
            printf("  Set #%d failed (write_count=%u)\n", i + 1, g_fault_ctx.write_count);
            fail_count++;
        } else {
            printf("  Set #%d success\n", i + 1);
        }
    }
    TEST_ASSERT(fail_count > 0); /* at least one write should fail */

    /* 打印故障报告 */
    fault_print_report(&g_fault_ctx);

    return 0;
}

/* ═══════════════════════════════════════════════════════════════════════════
 *  测试用例 2：概率性故障注入
 * ═══════════════════════════════════════════════════════════════════════════ */

TEST_CASE(fault_write_fail_probability, "Fault", "Write fails with probability")
{
    (void)ctx;

    printf("\n[Fault Test 2] Write fails with 20%% probability\n");

    /* 初始化 Flash */
    sim_flash_init(&g_flash, g_flash_buf, FLASH_SIZE, SECTOR_SIZE, 256, 0);

    /* 配置故障：20% 概率写入失败 */
    fault_init(&g_fault_ctx, 0x66666);
    fault_quick_write_fail_probability(&g_fault_ctx, 20);
    sim_flash_set_fault_ctx(&g_flash, &g_fault_ctx);

    /* 初始化 KVDB */
    rdb_partition_t part = {
        .name = "test",
        .base_addr = 0,
        .total_size = KVDB_PART_SIZE,
        .sector_size = SECTOR_SIZE,
        .write_gran = 0,
        .ops = &g_ops
    };

    uint8_t meta_buf[512];
    rdb_kvdb_t db;
    db.part = &part;
    db.sectors = (rdb_kv_sector_meta_t*)meta_buf;
    rdb_kvdb_format(&db);
    rdb_kvdb_init(&db, &part, meta_buf);

    /* 尝试 50 次写入，统计失败率 */
    int success = 0, failed = 0;
    for (int i = 0; i < 50; i++) {
        char key[16];
        snprintf(key, sizeof(key), "k%d", i);
        rdb_err_t ret = rdb_kvdb_set(&db, key, "v", 1);
        
        if (ret == RDB_OK) {
            success++;
        } else {
            failed++;
        }
    }
    
    printf("  Success: %d, Failed: %d (%.1f%% failure rate)\n",
           success, failed, (failed * 100.0f) / (success + failed));
    
    fault_print_report(&g_fault_ctx);
    
    return 0;
}

/* ═══════════════════════════════════════════════════════════════════════════
 *  测试用例 3：擦除失败注入
 * ═══════════════════════════════════════════════════════════════════════════ */

TEST_CASE(fault_erase_fail, "Fault", "Erase fails on Nth operation")
{
    (void)ctx;
    
    printf("\n[Fault Test 3] Erase fails on 2nd operation\n");
    
    /* 初始化 Flash */
    sim_flash_init(&g_flash, g_flash_buf, FLASH_SIZE, SECTOR_SIZE, 256, 0);
    
    /* 配置故障：第 2 次擦除失败 */
    fault_init(&g_fault_ctx, 0x99999);
    fault_quick_erase_fail(&g_fault_ctx, 2);
    sim_flash_set_fault_ctx(&g_flash, &g_fault_ctx);
    
    /* 初始化 KVDB */
    rdb_partition_t part = {
        .name = "test",
        .base_addr = 0,
        .total_size = KVDB_PART_SIZE,
        .sector_size = SECTOR_SIZE,
        .write_gran = 0,
        .ops = &g_ops
    };
    
    uint8_t meta_buf[512];
    rdb_kvdb_t db;
    db.part = &part;
    db.sectors = (rdb_kv_sector_meta_t*)meta_buf;
    rdb_kvdb_format(&db);
    rdb_kvdb_init(&db, &part, meta_buf);

    /* 写入大量数据触发 GC（需要擦除） */
    printf("  Writing data to trigger GC...\n");
    for (int i = 0; i < 200; i++) {
        char key[16];
        uint8_t val[100];
        snprintf(key, sizeof(key), "key%d", i);
        memset(val, i, sizeof(val));
        
        rdb_err_t ret = rdb_kvdb_set(&db, key, val, sizeof(val));
        if (ret != RDB_OK) {
            printf("  Set failed at iteration %d (erase_count=%u)\n", 
                   i, g_fault_ctx.erase_count);
            break;
        }
    }
    
    fault_print_report(&g_fault_ctx);
    
    return 0;
}

/* ═══════════════════════════════════════════════════════════════════════════
 *  测试用例 4：掉电中断模拟
 * ═══════════════════════════════════════════════════════════════════════════ */

TEST_CASE(fault_power_loss, "Fault", "Power loss during write")
{
    (void)ctx;
    
    printf("\n[Fault Test 4] Power loss at 3rd write, byte 8\n");
    
    /* 初始化 Flash */
    sim_flash_init(&g_flash, g_flash_buf, FLASH_SIZE, SECTOR_SIZE, 256, 0);
    
    /* 配置故障：第 3 次写入的第 8 字节时掉电 */
    fault_init(&g_fault_ctx, 0xAAAAA);
    fault_quick_power_loss(&g_fault_ctx, 3, 8);
    sim_flash_set_fault_ctx(&g_flash, &g_fault_ctx);
    
    /* 初始化 KVDB */
    rdb_partition_t part = {
        .name = "test",
        .base_addr = 0,
        .total_size = KVDB_PART_SIZE,
        .sector_size = SECTOR_SIZE,
        .write_gran = 0,
        .ops = &g_ops
    };
    
    uint8_t meta_buf[512];
    rdb_kvdb_t db;
    db.part = &part;
    db.sectors = (rdb_kv_sector_meta_t*)meta_buf;
    rdb_kvdb_format(&db);
    rdb_kvdb_init(&db, &part, meta_buf);

    /* 写入数据，观察掉电 */
    for (int i = 0; i < 10; i++) {
        char key[16];
        snprintf(key, sizeof(key), "key%d", i);
        rdb_err_t ret = rdb_kvdb_set(&db, key, "long_value_12345", 16);
        
        if (ret != RDB_OK) {
            printf("  Power loss detected at write #%d\n", i + 1);
            break;
        }
        printf("  Write #%d success\n", i + 1);
    }
    
    printf("\n  Simulating recovery after power loss...\n");
    
    /* 重新初始化（模拟重启） */
    fault_clear_rules(&g_fault_ctx);  /* 清除故障规则 */
    rdb_err_t ret = rdb_kvdb_init(&db, &part, meta_buf);
    TEST_ASSERT_RDB_OK(ret);
    
    printf("  Recovery successful!\n");
    
    fault_print_report(&g_fault_ctx);
    
    return 0;
}

/* ═══════════════════════════════════════════════════════════════════════════
 *  测试用例 5：数据损坏注入
 * ═══════════════════════════════════════════════════════════════════════════ */

TEST_CASE(fault_data_corruption, "Fault", "Data corruption in specific range")
{
    (void)ctx;
    
    printf("\n[Fault Test 5] Corrupt data at address 0x1000-0x1100\n");
    
    /* 初始化 Flash */
    sim_flash_init(&g_flash, g_flash_buf, FLASH_SIZE, SECTOR_SIZE, 256, 0);
    
    /* 配置故障：损坏特定地址范围 */
    fault_init(&g_fault_ctx, 0xBBBBB);
    fault_quick_corrupt_data(&g_fault_ctx, 0x1000, 0x100, 0x55);
    sim_flash_set_fault_ctx(&g_flash, &g_fault_ctx);
    
    /* 初始化 KVDB */
    rdb_partition_t part = {
        .name = "test",
        .base_addr = 0,
        .total_size = KVDB_PART_SIZE,
        .sector_size = SECTOR_SIZE,
        .write_gran = 0,
        .ops = &g_ops
    };
    
    uint8_t meta_buf[512];
    rdb_kvdb_t db;
    db.part = &part;
    db.sectors = (rdb_kv_sector_meta_t*)meta_buf;
    rdb_kvdb_format(&db);
    rdb_kvdb_init(&db, &part, meta_buf);

    /* 写入数据 */
    rdb_kvdb_set(&db, "test_key", "test_value", 10);
    
    /* 读取数据（可能检测到 CRC 错误） */
    uint8_t buf[32];
    uint16_t len;
    rdb_err_t ret = rdb_kvdb_get(&db, "test_key", buf, sizeof(buf), &len);
    
    if (ret == RDB_ERR_CRC) {
        printf("  CRC error detected (data corruption injected)\n");
    } else if (ret == RDB_OK) {
        printf("  Data read successfully (corruption may not affect this key)\n");
    }
    
    fault_print_report(&g_fault_ctx);
    
    return 0;
}

/* ═══════════════════════════════════════════════════════════════════════════
 *  主测试入口
 * ═══════════════════════════════════════════════════════════════════════════ */

int main(void)
{
    printf("========================================\n");
    printf("RocketDB Fault Injection Tests\n");
    printf("========================================\n");
    
    /* 配置测试框架 */
    test_config_t config = {
        .log_file = fopen(test_make_log_path("fault_injection"), "w"),
        .verbose = 1,
        .stop_on_fail = 0,
        .filter = NULL
    };
    
    test_framework_init(&config);
    
    /* 注册测试用例 */
    test_suite_t *suite = test_get_default_suite();
    test_register_case(suite, &test_case_fault_write_fail_nth);
    test_register_case(suite, &test_case_fault_write_fail_probability);
    test_register_case(suite, &test_case_fault_erase_fail);
    test_register_case(suite, &test_case_fault_power_loss);
    test_register_case(suite, &test_case_fault_data_corruption);
    
    /* 运行测试 */
    test_run_all(NULL);
    
    /* 打印报告 */
    test_print_report();
    
    /* 清理 */
    if (config.log_file) {
        fclose(config.log_file);
    }
    
    test_stats_t stats;
    test_get_stats(&stats);
    
    return (stats.failed_cases == 0) ? 0 : 1;
}
