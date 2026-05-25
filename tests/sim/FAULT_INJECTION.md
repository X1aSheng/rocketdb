# Flash 故障注入使用指南

## 概述

RocketDB 提供了一套完整的 Flash 故障注入系统，用于测试系统在各种故障场景下的鲁棒性和恢复能力。

## 故障类型

系统支持以下故障类型：

| 故障类型 | 说明 | 典型用例 |
|---------|------|---------|
| `FAULT_TYPE_READ_FAIL` | 读取失败 | 模拟 Flash 读取错误 |
| `FAULT_TYPE_WRITE_FAIL` | 写入失败 | 模拟 Flash 写入错误 |
| `FAULT_TYPE_ERASE_FAIL` | 擦除失败 | 模拟扇区擦除失败 |
| `FAULT_TYPE_POWER_LOSS` | 掉电中断 | 模拟突然断电 |
| `FAULT_TYPE_DATA_CORRUPT` | 数据损坏 | 模拟 Flash 数据损坏 |
| `FAULT_TYPE_BIT_FLIP` | 单比特翻转 | 模拟磨损导致的比特翻转 |

## 触发模式

故障可以通过多种方式触发：

| 触发模式 | 说明 | 参数 |
|---------|------|------|
| `FAULT_TRIGGER_COUNT` | 基于计数 | 在第 N 次操作时触发 |
| `FAULT_TRIGGER_PROBABILITY` | 基于概率 | 每次操作有 P% 概率触发 |
| `FAULT_TRIGGER_ADDRESS` | 基于地址 | 在特定地址范围触发 |
| `FAULT_TRIGGER_PATTERN` | 基于模式 | 检测特定数据模式时触发 |

## 快速开始

### 1. 初始化故障上下文

```c
#include "sim_fault.h"

fault_ctx_t fault_ctx;
fault_init(&fault_ctx, 0x12345);  /* 随机数种子 */
```

### 2. 绑定到 Flash 模拟器

```c
#include "sim_flash.h"

sim_flash_t flash;
sim_flash_init(&flash, buffer, size, sector_size, page_size, write_gran);

/* 启用故障注入 */
sim_flash_set_fault_ctx(&flash, &fault_ctx);
```

### 3. 配置故障规则

使用便捷函数快速配置常见故障：

```c
/* 第 5 次写入失败 */
fault_quick_write_fail(&fault_ctx, 5);

/* 第 3 次擦除失败 */
fault_quick_erase_fail(&fault_ctx, 3);

/* 20% 概率写入失败 */
fault_quick_write_fail_probability(&fault_ctx, 20);

/* 第 10 次写入的第 8 字节时掉电 */
fault_quick_power_loss(&fault_ctx, 10, 8);

/* 损坏地址 0x1000-0x1100 的数据 */
fault_quick_corrupt_data(&fault_ctx, 0x1000, 0x100, 0x55);
```

### 4. 运行测试并观察行为

```c
/* 正常使用 RocketDB API */
rdb_kvdb_set(&db, "key", "value", 5);

/* 故障会在满足条件时自动注入 */
```

### 5. 查看故障报告

```c
fault_print_report(&fault_ctx);
```

输出示例：
```
========================================
Fault Injection Report
========================================

Operations:
  Reads:   123
  Writes:  45
  Erases:  3

Faults Injected: 2
  READ_FAIL:     0
  WRITE_FAIL:    1
  ERASE_FAIL:    1
  POWER_LOSS:    0
  DATA_CORRUPT:  0
  BIT_FLIP:      0

Active Rules: 2
  [0] type=2, mode=0, ENABLED
  [1] type=3, mode=0, ENABLED
========================================
```

## 高级用法

### 手动添加故障规则

对于复杂场景，可以手动创建故障规则：

```c
fault_rule_t rule = {
    .type = FAULT_TYPE_WRITE_FAIL,
    .trigger_mode = FAULT_TRIGGER_PROBABILITY,
    .probability_pct = 15,
    .enabled = 1
};

int rule_id = fault_add_rule(&fault_ctx, &rule);
```

### 基于地址范围的故障

```c
fault_rule_t rule = {
    .type = FAULT_TYPE_WRITE_FAIL,
    .trigger_mode = FAULT_TRIGGER_ADDRESS,
    .addr_start = 0x4000,    /* 从扇区 1 开始 */
    .addr_end = 0x8000,      /* 到扇区 2 结束 */
    .probability_pct = 30,   /* 在此范围内 30% 概率失败 */
    .enabled = 1
};

fault_add_rule(&fault_ctx, &rule);
```

### 动态启用/禁用规则

```c
/* 临时禁用规则 */
fault_set_rule_enabled(&fault_ctx, rule_id, 0);

/* 重新启用规则 */
fault_set_rule_enabled(&fault_ctx, rule_id, 1);

/* 清除所有规则 */
fault_clear_rules(&fault_ctx);
```

### 组合多个故障

可以同时配置多种故障，系统会按顺序检查：

```c
fault_init(&fault_ctx, seed);

/* 第 5 次写入失败 */
fault_quick_write_fail(&fault_ctx, 5);

/* 第 10 次擦除失败 */
fault_quick_erase_fail(&fault_ctx, 10);

/* 10% 概率随机写入失败 */
fault_quick_write_fail_probability(&fault_ctx, 10);

/* 损坏特定地址 */
fault_quick_corrupt_data(&fault_ctx, 0x2000, 64, 0xAA);
```

## 典型测试场景

### 场景 1：GC 期间擦除失败

```c
/* 配置：第 2 次擦除失败 */
fault_quick_erase_fail(&fault_ctx, 2);

/* 写入大量数据触发 GC */
for (int i = 0; i < 500; i++) {
    char key[16];
    snprintf(key, sizeof(key), "key%d", i);
    ret = rdb_kvdb_set(&db, key, large_value, len);
    
    if (ret != RDB_OK) {
        printf("GC failed due to erase failure\n");
    }
}
```

### 场景 2：写入过程中掉电

```c
/* 配置：第 3 次写入的第 8 字节时掉电 */
fault_quick_power_loss(&fault_ctx, 3, 8);

rdb_kvdb_set(&db, "key1", "value1", 6);  /* OK */
rdb_kvdb_set(&db, "key2", "value2", 6);  /* OK */
rdb_kvdb_set(&db, "key3", "value3", 6);  /* Power loss! */

/* 模拟重启：重新初始化 */
fault_clear_rules(&fault_ctx);
rdb_kvdb_init(&db, &part, meta_buf);

/* 验证数据一致性 */
ret = rdb_kvdb_get(&db, "key1", buf, len, &out_len);  /* 应该成功 */
ret = rdb_kvdb_get(&db, "key2", buf, len, &out_len);  /* 应该成功 */
ret = rdb_kvdb_get(&db, "key3", buf, len, &out_len);  /* 可能失败或部分数据 */
```

### 场景 3：CRC 损坏检测

```c
/* 配置：损坏扇区 1 的数据 */
fault_quick_corrupt_data(&fault_ctx, 0x1000, 0x1000, 0xFF);

/* 写入数据 */
rdb_kvdb_set(&db, "test", "data", 4);

/* 读取数据 */
ret = rdb_kvdb_get(&db, "test", buf, len, &out_len);

if (ret == RDB_ERR_CRC) {
    printf("CRC error detected - corruption properly handled\n");
}
```

### 场景 4：概率性故障压力测试

```c
/* 配置：5% 写入失败，3% 擦除失败 */
fault_quick_write_fail_probability(&fault_ctx, 5);
fault_quick_erase_fail(&fault_ctx, 3);

/* 高强度读写 */
for (int i = 0; i < 10000; i++) {
    char key[16];
    snprintf(key, sizeof(key), "k%d", i % 100);
    uint8_t val[128];
    memset(val, i, sizeof(val));
    
    ret = rdb_kvdb_set(&db, key, val, sizeof(val));
    
    if (ret != RDB_OK) {
        error_count++;
        /* RocketDB 应该能够处理这些错误 */
    }
}

printf("Total errors: %d out of 10000 operations\n", error_count);
fault_print_report(&fault_ctx);
```

## 故障记录与重放

### 导出故障规则

```c
char export_buf[4096];
int len = fault_export_rules(&fault_ctx, export_buf, sizeof(export_buf));

/* 保存到文件 */
FILE *f = fopen("fault_rules.txt", "w");
fwrite(export_buf, 1, len, f);
fclose(f);
```

### 导入故障规则

```c
/* 从文件加载 */
FILE *f = fopen("fault_rules.txt", "r");
char import_buf[4096];
fread(import_buf, 1, sizeof(import_buf), f);
fclose(f);

/* 导入规则 */
fault_import_rules(&fault_ctx, import_buf);
```

## 统计信息

### 获取详细统计

```c
uint32_t ops[3];      /* read/write/erase 计数 */
uint32_t faults[8];   /* 按类型的故障计数 */

fault_get_stats(&fault_ctx, ops, faults);

printf("Operations: R=%u, W=%u, E=%u\n", ops[0], ops[1], ops[2]);
printf("Faults: %u total\n", 
       faults[FAULT_TYPE_READ_FAIL] + 
       faults[FAULT_TYPE_WRITE_FAIL] + 
       faults[FAULT_TYPE_ERASE_FAIL]);
```

### 重置统计

```c
fault_reset_stats(&fault_ctx);
```

## 调试技巧

### 1. 详细日志

在故障注入前后打印调试信息：

```c
printf("Before operation: write_count=%u\n", fault_ctx.write_count);
ret = rdb_kvdb_set(&db, "key", "val", 3);
printf("After operation: ret=%d, injected=%u\n", 
       ret, fault_ctx.fault_injected);
```

### 2. 单步故障注入

逐步增加故障触发点，定位问题：

```c
for (int n = 1; n <= 100; n++) {
    fault_init(&fault_ctx, seed);
    fault_quick_write_fail(&fault_ctx, n);
    
    /* 运行测试 */
    ret = run_test(&db);
    
    if (ret != RDB_OK) {
        printf("Test failed when write fails at N=%d\n", n);
        break;
    }
}
```

### 3. 验证恢复

在注入故障后验证系统能否恢复：

```c
/* 注入故障 */
fault_quick_power_loss(&fault_ctx, 5, 10);
ret = run_workload(&db);

/* 清除故障并重新初始化 */
fault_clear_rules(&fault_ctx);
ret = rdb_kvdb_init(&db, &part, meta_buf);

if (ret == RDB_OK) {
    printf("Recovery successful\n");
    /* 验证数据一致性 */
    verify_data_integrity(&db);
}
```

## API 参考

完整 API 文档请参见 [sim_fault.h](sim_fault.h)。

## 示例代码

完整的故障注入测试示例：[test_fault_injection.c](test_fault_injection.c)

## 最佳实践

1. **从简单开始**：先测试单一故障类型，再组合多种故障
2. **使用固定种子**：便于重现问题
3. **记录故障规则**：保存导致失败的故障配置
4. **验证恢复**：每次故障后都要验证系统能否恢复
5. **压力测试**：使用概率性故障进行长时间压力测试
6. **监控统计**：定期检查故障注入统计，确保按预期工作
