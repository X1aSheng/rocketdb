# EXAMPLES（示例程序集）

本文档通过实际代码示例展示如何在不同场景下使用 RocketDB。

---

## 示例 1：基础键值存储与检索

**场景**：最简单的 CRUD 操作

```c
#include "rocketdb.h"
#include <stdio.h>
#include <string.h>

// Flash HAL 的最小实现（模拟版）
static uint8_t flash_sim[64 * 1024];

int hal_flash_read(uint32_t addr, uint8_t *buf, size_t len) {
    memcpy(buf, flash_sim + addr, len);
    return 0;
}

int hal_flash_write(uint32_t addr, const uint8_t *buf, size_t len) {
    memcpy(flash_sim + addr, buf, len);
    return 0;
}

int hal_flash_erase(uint32_t addr) {
    memset(flash_sim + addr, 0xFF, 4096);  // 假设 4KB 扇区
    return 0;
}

void hal_flash_lock(void)   { /* disable interrupts */ }
void hal_flash_unlock(void) { /* enable interrupts */ }
void hal_flash_yield(void)  { /* feed watchdog */ }

int main(void) {
    // 1. 定义 Flash 分区
    rdb_flash_ops_t flash_ops = {
        .read = hal_flash_read,
        .write = hal_flash_write,
        .erase = hal_flash_erase,
        .lock = hal_flash_lock,
        .unlock = hal_flash_unlock,
        .yield = hal_flash_yield
    };

    rdb_partition_t kvdb_part = {
        .name = "KVDB",
        .base_addr = 0,
        .total_size = 64 * 1024,
        .sector_size = 4 * 1024,
        .write_gran = 0,  // 1 字节粒度
        .ops = &flash_ops
    };

    // 2. 初始化数据库
    rdb_kv_sector_meta_t sectors[16];
    rdb_kvdb_t db = {0};
    db.part = &kvdb_part;
    db.sectors = sectors;
    db.sector_cnt = 16;

    if (rdb_kvdb_init(&db, &kvdb_part, sectors) != RDB_OK) {
        printf("Failed to init KVDB\n");
        return 1;
    }

    printf("KVDB initialized (sectors=%u, total=%u bytes)\n", 
           db.sector_cnt, db.part->total_size);

    // 3. 写入数据
    uint8_t value[32];
    sprintf((char*)value, "temperature=25.5C");
    
    int ret = rdb_kvdb_set(&db, "sensor_temp", value, strlen((char*)value));
    if (ret == RDB_OK) {
        printf("✓ Set key=sensor_temp, value=%s\n", value);
    } else {
        printf("✗ Set failed: err=%d\n", ret);
    }

    // 4. 读取数据
    uint8_t  read_buf[32] = {0};
    uint16_t read_len = 0;
    
    ret = rdb_kvdb_get(&db, "sensor_temp", read_buf, sizeof(read_buf), &read_len);
    if (ret == RDB_OK) {
        printf("✓ Get key=sensor_temp, value=%.*s (len=%u)\n", 
               (int)read_len, read_buf, (unsigned)read_len);
    } else {
        printf("✗ Get failed: err=%d\n", ret);
    }

    // 5. 删除数据
    ret = rdb_kvdb_delete(&db, "sensor_temp");
    if (ret == RDB_OK) {
        printf("✓ Deleted key=sensor_temp\n");
    }

    // 验证删除
    ret = rdb_kvdb_get(&db, "sensor_temp", read_buf, sizeof(read_buf), &read_len);
    if (ret == RDB_ERR_NOT_FOUND) {
        printf("✓ Confirmed: key not found after delete\n");
    }

    return 0;
}
```

**预期输出**
```
KVDB initialized (sectors=16, total=65536 bytes)
✓ Set key=sensor_temp, value=temperature=25.5C
✓ Get key=sensor_temp, value=temperature=25.5C (len=17)
✓ Deleted key=sensor_temp
✓ Confirmed: key not found after delete
```

---

## 示例 2：迭代存储的所有键值对

**场景**：枚举数据库中所有活跃键值

```c
#include "rocketdb.h"
#include <stdio.h>

int iterate_all_kvs(rdb_kvdb_t *db) {
    rdb_kv_iter_t it;
    int count = 0;

    // 初始化迭代器
    int ret = rdb_kv_iter_init(&it, db);
    if (ret != RDB_OK) {
        printf("Failed to init iterator: err=%d\n", ret);
        return -1;
    }

    printf("Iterating all KV pairs:\n");

    // 遍历所有键值
    char     key_buf[64];
    uint8_t  val_buf[256];
    uint16_t key_len, val_len;

    while (1) {
        ret = rdb_kv_iter_next(&it, key_buf, sizeof(key_buf),
                               val_buf, sizeof(val_buf), &key_len, &val_len);
        
        if (ret == RDB_ERR_ITER_END) {
            // 迭代结束
            break;
        } else if (ret == RDB_ERR_BUSY) {
            // 数据库在迭代期间被修改，需要重新开始
            printf("Database modified during iteration, restarting...\n");
            rdb_kv_iter_init(&it, db);
            continue;
        } else if (ret != RDB_OK) {
            printf("Iteration error: err=%d\n", ret);
            return -1;
        }

        // 打印这对键值
        printf("  [%d] key=%-20s val=%.*s (klen=%u, vlen=%u)\n",
               count, key_buf, (int)val_len, val_buf,
               (unsigned)key_len, (unsigned)val_len);
        count++;
    }

    printf("Total: %d pairs\n", count);
    return count;
}

// 使用示例
int main(void) {
    rdb_kvdb_t db = {0};
    // ... 初始化 db（同示例 1）...

    // 写入几个键值
    rdb_kvdb_set(&db, "key1", (uint8_t*)"value1", 6);
    rdb_kvdb_set(&db, "key2", (uint8_t*)"value2", 6);
    rdb_kvdb_set(&db, "key3", (uint8_t*)"value3", 6);

    // 迭代
    int total = iterate_all_kvs(&db);
    printf("Found %d entries\n", total);

    return 0;
}
```

**预期输出**
```
Iterating all KV pairs:
  [0] key=key1                 val=value1 (klen=4, vlen=6)
  [1] key=key2                 val=value2 (klen=4, vlen=6)
  [2] key=key3                 val=value3 (klen=4, vlen=6)
Total: 3 pairs
Found 3 entries
```

---

## 示例 3：时间序列数据存储与查询

**场景**：记录传感器读数到 TSDB，按时间范围查询

```c
#include "rocketdb.h"
#include <stdio.h>
#include <time.h>

/* Query callback — invoked once per matching record */
typedef struct {
    int count;
    struct { uint16_t sensor_id; uint16_t value; uint16_t checksum; } dp;
} ts_query_ctx_t;

static int ts_query_cb(uint32_t time, const void *data, uint16_t len, void *arg)
{
    ts_query_ctx_t *ctx = (ts_query_ctx_t *)arg;
    if (len >= sizeof(ctx->dp)) {
        memcpy(&ctx->dp, data, sizeof(ctx->dp));
        printf("  [%d] time=%u, sensor=%u, temp=%u°C, crc=%u\n",
               ctx->count, time, ctx->dp.sensor_id,
               ctx->dp.value, ctx->dp.checksum);
        ctx->count++;
    }
    return RDB_ITER_CONTINUE;
}

int main(void) {
    // ... 初始化 TSDB（类似 KVDB）...
    rdb_tsdb_t ts_db = {0};
    rdb_partition_t ts_part = {0};
    uint32_t ec_buf[16];
    
    // ts_db.part = &ts_part;
    // ts_db.erase_cnts = ec_buf;
    // ts_db.sector_cnt = 16;
    // rdb_tsdb_init(&ts_db, &ts_part, ec_buf);

    // 1. 添加时间序列数据点
    printf("Appending sensor data to TSDB:\n");

    struct {
        uint16_t sensor_id;
        uint16_t value;
        uint16_t checksum;
    } data_point = {0};

    // 添加 5 个数据点
    for (int i = 0; i < 5; i++) {
        data_point.sensor_id = 101;
        data_point.value = 20 + i;  // 温度：20, 21, 22, 23, 24℃
        data_point.checksum = data_point.sensor_id + data_point.value;

        uint32_t epoch = 1609459200 + (i * 3600);  // 每小时一个数据点
        int ret = rdb_tsdb_append(&ts_db, epoch, 
                                   (uint8_t*)&data_point, sizeof(data_point));
        
        if (ret == RDB_OK) {
            printf("  ✓ Appended epoch=%u, value=%u\n", epoch, data_point.value);
        } else {
            printf("  ✗ Append failed: err=%d\n", ret);
        }
    }

    // 2. 查询指定时间范围内的数据（回调模式）
    printf("\nQuerying TSDB (time range 1609459200 to 1609478400):\n");

    ts_query_ctx_t qctx = { 0 };
    int ret = rdb_tsdb_query(&ts_db, 1609459200, 1609478400, ts_query_cb, &qctx);
    if (ret != RDB_OK) {
        printf("Query failed: err=%d\n", ret);
        return 1;
    }

    printf("Total records in range: %d\n", qctx.count);

    return 0;
}
```

**预期输出**
```
Appending sensor data to TSDB:
  ✓ Appended epoch=1609459200, value=20
  ✓ Appended epoch=1609462800, value=21
  ✓ Appended epoch=1609466400, value=22
  ✓ Appended epoch=1609470000, value=23
  ✓ Appended epoch=1609473600, value=24

Querying TSDB (time range 1609459200 to 1609478400):
  [0] time=1609459200, sensor=101, temp=20°C, crc=121
  [1] time=1609462800, sensor=101, temp=21°C, crc=122
  [2] time=1609466400, sensor=101, temp=22°C, crc=123
  [3] time=1609470000, sensor=101, temp=23°C, crc=124
  [4] time=1609478400, sensor=101, temp=24°C, crc=125
Total records in range: 5
```

---

## 示例 4：掉电安全性验证

**场景**：模拟掉电发生在关键操作中，验证恢复

```c
#include "rocketdb.h"
#include <stdio.h>
#include <string.h>

// 简单的掉电模拟（实际应使用故障注入框架）
static volatile int g_power_fail_at_write = 0;

int hal_flash_write_with_fault(uint32_t addr, const uint8_t *buf, size_t len) {
    static int write_count = 0;
    
    if (++write_count == g_power_fail_at_write) {
        printf("[FAULT] Power loss at write #%d (addr=0x%x, len=%zu)\n", 
               write_count, addr, len);
        // 模拟掉电：不完成这次写操作，返回失败
        return -1;  // 或直接退出程序
    }
    
    // 正常写入
    memcpy((void*)addr, buf, len);
    return 0;
}

int main(void) {
    rdb_kvdb_t db = {0};
    // ... 初始化 db ...

    // 场景 1：掉电在第 3 次写操作
    printf("=== Scenario 1: Power loss at 3rd write ===\n");
    g_power_fail_at_write = 3;
    
    rdb_kvdb_set(&db, "test_key", (uint8_t*)"test_val", 8);
    // 程序模拟掉电退出

    printf("\n=== After reboot: Recovering ===\n");
    // 重新初始化（模拟重启后）
    g_power_fail_at_write = 0;
    rdb_kvdb_init(&db, NULL, NULL);

    // 验证数据一致性
    uint8_t  buf[32] = {0};
    uint16_t len = 0;
    int ret = rdb_kvdb_get(&db, "test_key", buf, sizeof(buf), &len);
    
    if (ret == RDB_OK) {
        printf("✓ Recovered successfully: key=%s\n", buf);
    } else if (ret == RDB_ERR_NOT_FOUND) {
        printf("✓ Key was not committed before power loss (expected)\n");
    } else {
        printf("✗ Corruption detected: err=%d\n", ret);
    }

    return 0;
}
```

**设计保证**
- ✅ 如果 `set()` 返回 RDB_OK，数据**永不丢失**
- ✅ 如果掉电发生在写操作中，恢复后数据库仍保持一致
- ✅ 不会因掉电产生数据损坏

---

## 示例 5：自定义 Flash 驱动集成

**场景**：在 STM32F4 MCU 上使用真实 Flash 驱动

```c
#include "rocketdb.h"
#include "stm32f4xx_hal.h"  // 假设使用 STM32 HAL

// STM32F4 Flash 参数
#define FLASH_BASE       0x08000000
#define KVDB_ADDR        0x08040000   // 256 KB 偏移
#define KVDB_SIZE        (64 * 1024)
#define SECTOR_SIZE      (16 * 1024)

// Write granule = 0 (1 字节粒度)

static int stm32_flash_read(uint32_t addr, uint8_t *buf, size_t len) {
    // STM32 Flash 可直接读取
    memcpy(buf, (void*)(KVDB_ADDR + addr), len);
    return 0;
}

static int stm32_flash_write(uint32_t addr, const uint8_t *buf, size_t len) {
    HAL_StatusTypeDef status;
    uint32_t abs_addr = KVDB_ADDR + addr;
    
    // 解锁 Flash
    HAL_FLASH_Unlock();
    
    // 逐字节写入（使用 HAL_FLASH_Program）
    for (size_t i = 0; i < len; i++) {
        status = HAL_FLASH_Program(FLASH_TYPEPROGRAM_BYTE, 
                                   abs_addr + i, buf[i]);
        if (status != HAL_OK) {
            HAL_FLASH_Lock();
            return -1;
        }
    }
    
    HAL_FLASH_Lock();
    return 0;
}

static int stm32_flash_erase(uint32_t addr) {
    HAL_StatusTypeDef status;
    uint32_t sector;
    FLASH_EraseInitTypeDef erase_init = {
        .TypeErase = FLASH_TYPEERASE_SECTORS,
        .VoltageRange = FLASH_VOLTAGE_RANGE_3,
        .NbSectors = 1
    };

    // 计算包含 addr 的扇区号
    sector = (KVDB_ADDR + addr - FLASH_BASE) / SECTOR_SIZE;
    erase_init.Sector = sector;

    uint32_t error;
    HAL_FLASH_Unlock();
    status = HAL_FLASHEx_Erase(&erase_init, &error);
    HAL_FLASH_Lock();

    return (status == HAL_OK) ? 0 : -1;
}

static void stm32_flash_lock(void) {
    __disable_irq();  // 禁用中断
}

static void stm32_flash_unlock(void) {
    __enable_irq();   // 启用中断
}

static void stm32_flash_yield(void) {
    // 喂狗（如果启用了 WDT）
    HAL_IWDG_Refresh(&hiwdg);
}

// 初始化函数
void app_init_rocketdb(void) {
    // 配置 Flash 操作
    static rdb_flash_ops_t stm32_ops = {
        .read = stm32_flash_read,
        .write = stm32_flash_write,
        .erase = stm32_flash_erase,
        .lock = stm32_flash_lock,
        .unlock = stm32_flash_unlock,
        .yield = stm32_flash_yield
    };

    // 配置分区
    static rdb_partition_t kvdb_partition = {
        .name = "config_db",
        .base_addr = 0,              // 相对于分区
        .total_size = KVDB_SIZE,     // 64 KB
        .sector_size = SECTOR_SIZE,  // 16 KB
        .write_gran = 0,             // 1 字节
        .ops = &stm32_ops
    };

    // 初始化数据库
    static rdb_kv_sector_meta_t sectors[4];  // 4 个 16KB 扇区
    static rdb_kvdb_t config_db = {0};
    
    config_db.part = &kvdb_partition;
    config_db.sectors = sectors;
    config_db.sector_cnt = 4;

    int ret = rdb_kvdb_init(&config_db, &kvdb_partition, sectors);
    if (ret != RDB_OK) {
        printf("Failed to init RocketDB: err=%d\n", ret);
        Error_Handler();
    }

    printf("RocketDB initialized on STM32F4\n");
}

// 使用示例：存储设备配置
void app_save_config(void) {
    extern rdb_kvdb_t config_db;
    
    // 存储网络配置
    struct {
        uint32_t ip;
        uint16_t port;
    } config = {
        .ip = 0xC0A80101,    // 192.168.1.1
        .port = 8080
    };

    int ret = rdb_kvdb_set(&config_db, "net_config", 
                           (uint8_t*)&config, sizeof(config));
    if (ret == RDB_OK) {
        printf("✓ Network config saved\n");
    }
}

void app_load_config(void) {
    extern rdb_kvdb_t config_db;
    
    struct {
        uint32_t ip;
        uint16_t port;
    } config = {0};
    
    uint16_t len = 0;
    int ret = rdb_kvdb_get(&config_db, "net_config", 
                           (uint8_t*)&config, sizeof(config), &len);
    if (ret == RDB_OK) {
        printf("✓ Net config: ip=0x%08x, port=%u\n", config.ip, config.port);
    }
}
```

---

## 示例 6：批量导入与离线刷写

**场景**：从外部源（USB、SD卡）批量写入数据

```c
#include "rocketdb.h"
#include <stdio.h>

// 从 CSV 文件中读取数据并写入 KVDB
int bulk_import_from_csv(const char *csv_path, rdb_kvdb_t *db) {
    FILE *f = fopen(csv_path, "r");
    if (!f) {
        printf("Cannot open %s\n", csv_path);
        return -1;
    }

    int count = 0;
    char line[256];
    
    // CSV 格式: KEY,VALUE
    // 例: sensor_1,temperature=25.5C
    
    while (fgets(line, sizeof(line), f)) {
        char *comma = strchr(line, ',');
        if (!comma) continue;
        
        *comma = '\0';
        char *key = line;
        char *value = comma + 1;
        
        // 移除换行符
        char *newline = strchr(value, '\n');
        if (newline) *newline = '\0';
        
        // 写入数据库
        int ret = rdb_kvdb_set(db, key, (uint8_t*)value, strlen(value));
        if (ret == RDB_OK) {
            printf("  ✓ Imported: %s=%s\n", key, value);
            count++;
        } else {
            printf("  ✗ Failed to import %s (err=%d)\n", key, ret);
        }
    }
    
    fclose(f);
    return count;
}

// 导出 KVDB 到 CSV 文件
int bulk_export_to_csv(const char *csv_path, rdb_kvdb_t *db) {
    FILE *f = fopen(csv_path, "w");
    if (!f) {
        printf("Cannot open %s for writing\n", csv_path);
        return -1;
    }

    rdb_kv_iter_t it;
    rdb_kv_iter_init(&it, db);

    char     key_buf[64];
    uint8_t  val_buf[256];
    uint16_t key_len, val_len;
    int count = 0;

    while (1) {
        int ret = rdb_kv_iter_next(&it, key_buf, sizeof(key_buf),
                                   val_buf, sizeof(val_buf), &key_len, &val_len);
        if (ret != RDB_OK) break;

        // 写入 CSV 行
        fprintf(f, "%.*s,%.*s\n", (int)key_len, key_buf, (int)val_len, val_buf);
        count++;
    }

    fclose(f);
    printf("Exported %d entries to %s\n", count, csv_path);
    return count;
}

int main(void) {
    rdb_kvdb_t db = {0};
    // ... 初始化 db ...

    // 导入
    printf("=== Importing from config.csv ===\n");
    int imported = bulk_import_from_csv("config.csv", &db);
    printf("Total imported: %d\n", imported);

    // 导出
    printf("\n=== Exporting to backup.csv ===\n");
    int exported = bulk_export_to_csv("backup.csv", &db);
    printf("Total exported: %d\n", exported);

    return 0;
}
```

---

## 示例 7：监视与统计

**场景**：实时监视数据库状态和 GC 活动

```c
#include "rocketdb.h"
#include <stdio.h>

void print_db_stats(rdb_kvdb_t *db) {
    printf("\n=== Database Statistics ===\n");
    printf("Total sectors:    %u\n", db->sector_cnt);
    printf("Total size:       %u bytes\n", db->part->total_size);
    printf("Sector size:      %u bytes\n", db->part->sector_size);
    printf("Write granule:    %u\n", db->part->write_gran);
    printf("Current seq:      %u\n", db->write_seq);
    printf("Garbage bytes:    %u\n", db->garbage_bytes);
    printf("Iter gen:         %u\n", db->iter_gen);
    printf("GC reserve:       %u sectors\n", db->gc_reserve);
    printf("GC candidate:     sector %u\n", db->gc_candidate_idx);

    printf("\nSector Status:\n");
    printf("  Sec | Used   | State\n");
    printf("  ----+--------+----------\n");
    
    for (uint32_t i = 0; i < db->sector_cnt; i++) {
        rdb_kv_sector_meta_t *sm = &db->sectors[i];
        const char *state = "";
        
        switch (sm->status) {
        case RDB_SEC_ACTIVE: state = "ACTIVE"; break;
        case RDB_SEC_SEALED: state = "SEALED"; break;
        case RDB_SEC_ERASED: state = "ERASED"; break;
        case RDB_SEC_CORRUPT: state = "CORRUPT"; break;
        default: state = "UNKNOWN"; break;
        }
        
        printf("  %3u | %6u | %s\n", i, sm->write_off, state);
    }
}

void print_gc_progress(rdb_kvdb_t *db, int phase) {
    printf("[GC Phase %d] ", phase);
    
    switch (phase) {
    case 0:
        printf("Scanning trash\n");
        break;
    case 1:
        printf("Rewriting records\n");
        break;
    case 2:
        printf("Migrating to backup\n");
        break;
    case 3:
        printf("Compacting sectors\n");
        break;
    case 4:
        printf("Balancing wear\n");
        break;
    default:
        printf("Unknown phase\n");
    }
}

int main(void) {
    rdb_kvdb_t db = {0};
    // ... 初始化 db ...

    // 写入一些数据
    for (int i = 0; i < 10; i++) {
        char key[32], val[64];
        sprintf(key, "key_%d", i);
        sprintf(val, "value_%d_with_more_data", i);
        
        rdb_kvdb_set(&db, key, (uint8_t*)val, strlen(val));
    }

    print_db_stats(&db);

    // 删除一些数据以产生垃圾
    rdb_kvdb_delete(&db, "key_0");
    rdb_kvdb_delete(&db, "key_5");

    printf("\nAfter deletions:\n");
    print_db_stats(&db);

    // 手动触发 GC（如果有相关 API）
    // print_gc_progress(&db, 0);
    // rdb_kvdb_gc(&db);
    // print_gc_progress(&db, 4);

    return 0;
}
```

---

## 示例 8：并发访问（RTOS 环境）

**场景**：在 FreeRTOS 中安全地从多个任务访问 RocketDB

```c
#include "rocketdb.h"
#include "FreeRTOS.h"
#include "semphr.h"
#include <stdio.h>

// 全局数据库和互斥锁
static rdb_kvdb_t g_db;
static SemaphoreHandle_t g_db_mutex;

// 初始化
void app_init_rocketdb_rtos(void) {
    // 创建互斥锁
    g_db_mutex = xSemaphoreCreateMutex();
    if (!g_db_mutex) {
        printf("Failed to create mutex\n");
        return;
    }

    // 初始化数据库（在 RTOS 启动前）
    // ... rdb_kvdb_init(&g_db, ...) ...

    printf("RocketDB + FreeRTOS initialized\n");
}

// 线程安全的 set 操作
int safe_kv_set(const char *key, const uint8_t *val, uint16_t val_len) {
    if (xSemaphoreTake(g_db_mutex, pdMS_TO_TICKS(5000)) != pdTRUE) {
        printf("Timeout waiting for DB lock\n");
        return -1;
    }

    int ret = rdb_kvdb_set(&g_db, key, val, val_len);

    xSemaphoreGive(g_db_mutex);
    return ret;
}

// 线程安全的 get 操作
int safe_kv_get(const char *key, uint8_t *buf, uint16_t buf_cap, uint16_t *out_len) {
    if (xSemaphoreTake(g_db_mutex, pdMS_TO_TICKS(5000)) != pdTRUE) {
        printf("Timeout waiting for DB lock\n");
        return -1;
    }

    int ret = rdb_kvdb_get(&g_db, key, buf, buf_cap, out_len);

    xSemaphoreGive(g_db_mutex);
    return ret;
}

// 任务 1：传感器读取与存储
void task_sensor_logger(void *arg) {
    uint8_t sensor_data[8];

    while (1) {
        // 读取传感器
        sensor_data[0] = 25;  // 模拟温度读数
        sensor_data[1] = 60;  // 湿度

        // 保存到 RocketDB
        int ret = safe_kv_set("latest_sensor", sensor_data, 2);
        if (ret == RDB_OK) {
            printf("[SENSOR] Logged temp=%d°C, humidity=%d%%\n", 
                   sensor_data[0], sensor_data[1]);
        }

        vTaskDelay(pdMS_TO_TICKS(2000));  // 2 秒间隔
    }
}

// 任务 2：配置读取（可能与任务 1 并发）
void task_config_reader(void *arg) {
    uint8_t  config[16];
    uint16_t len;

    while (1) {
        len = 0;
        int ret = safe_kv_get("device_config", config, sizeof(config), &len);
        if (ret == RDB_OK) {
            printf("[CONFIG] Read config (len=%u): ", (unsigned)len);
            for (uint16_t i = 0; i < len; i++) {
                printf("%02x ", config[i]);
            }
            printf("\n");
        }

        vTaskDelay(pdMS_TO_TICKS(1500));
    }
}

// 在 main 中创建任务
void app_main(void) {
    app_init_rocketdb_rtos();

    xTaskCreate(task_sensor_logger, "Sensor", 256, NULL, 1, NULL);
    xTaskCreate(task_config_reader, "Config", 256, NULL, 1, NULL);

    vTaskStartScheduler();
}
```

---

## 故障排查提示

- 编译问题？→ 查看 [TROUBLESHOOTING.md](TROUBLESHOOTING.md#编译问题)
- 测试失败？→ 查看 [TROUBLESHOOTING.md](TROUBLESHOOTING.md#测试与功能问题)
- API 用法？→ 查看 [specify.md](SpecKit/specify.md)
- 设计决策？→ 查看 [clarify.md](SpecKit/clarify.md)

---

**最后更新**：2026年4月29日  
**版本**：RocketDB v1.1.2
