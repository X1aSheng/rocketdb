# RocketDB STM32F407 硬件测试方案

**日期:** 2026-06-19
**平台:** STM32F407 + W25Q128 (16MB NOR Flash)
**驱动版本:** v1.6.0

---

## 1. 硬件配置

### 1.1 分区布局

```
W25Q128 16MB 分区方案 (3-byte address, 4KB sectors):

┌──────────┬──────────┬──────────────┬──────────────────────────────┐
│ Offset   │ Size     │ Sectors      │ 用途                         │
├──────────┼──────────┼──────────────┼──────────────────────────────┤
│ 0x000000 │  256 KB  │  64 × 4KB    │ KVDB 配置存储                 │
│ 0x040000 │ 1024 KB  │ 256 × 4KB    │ TSDB 传感器日志               │
│ 0x140000 │  256 KB  │  64 × 4KB    │ KVDB 故障注入测试区 (可选)     │
│ 0x180000 │   剩余   │  —           │ 保留                         │
└──────────┴──────────┴──────────────┴──────────────────────────────┘
```

### 1.2 编译配置

```cmake
# arm-none-eabi-gcc 工具链
set(CMAKE_C_FLAGS "-mcpu=cortex-m4 -mthumb -mfloat-abi=hard -mfpu=fpv4-sp-d16 -Os")

# RocketDB 配置 (硬件推荐)
target_compile_definitions(rocketdb PUBLIC
    RDB_BLOOM_BITS=256          # 256-bit Bloom per sector
    RDB_KV_CACHE_SIZE=0         # 关闭缓存 (节省 RAM)
    RDB_DEDUP_SLOTS=16          # 去重表 16 槽 (256B)
    RDB_STACK_BUF_SIZE=128      # 合并写入缓冲 128B (匹配 W25Q page)
)
```

### 1.3 RAM 预算

| 组件 | 大小 |
|------|------|
| KVDB 句柄 | 87 B |
| KVDB meta buffer (64 扇区 × 48B) | 3,072 B |
| TSDB 句柄 | 88 B |
| TSDB EC buffer (256 扇区 × 4B) | 1,024 B |
| 去重表 (16 槽) | 256 B |
| Stack peak | ~1 KB |
| **总计** | **~5.5 KB** (192KB 可用, 占比 2.9%) |

---

## 2. 测试用例设计

### 2.1 KVDB 基础功能 (Test Suite 1)

| # | 用例 | 操作 | 验证点 | 预期耗时 |
|---|------|------|--------|----------|
| 1.1 | `kv_init_format` | 格式化 → init | `write_seq ≥ 1`, 扇区状态正确 | ~2s |
| 1.2 | `kv_set_get_basic` | SET 10 keys → GET 验证 | 值完整性, key 匹配 | ~1s |
| 1.3 | `kv_set_update` | SET → 修改 → GET | 旧值被覆盖, 新值正确 | ~1s |
| 1.4 | `kv_delete_exists` | SET → DELETE → EXISTS | DELETE 后 EXISTS 返回 NOT_FOUND | ~1s |
| 1.5 | `kv_mixed_lengths` | 随机大小 (1-255B) × 50 keys | Monte Carlo 读写 | ~3s |
| 1.6 | `kv_boundary` | MAX_KEY_LEN=32 + MAX_VAL_LEN=1023 | 边界值不溢出 | ~2s |
| 1.7 | `kv_space_wear` | 查询 space_info + wear_info | 返回值合理 | ~1s |

**小计: 7 用例, ~12s**

### 2.2 KVDB GC 压力 (Test Suite 2)

| # | 用例 | 操作 | 验证点 | 预期耗时 |
|---|------|------|--------|----------|
| 2.1 | `kv_gc_cycles_30` | 30 keys × 循环写入直到 GC≥30 次 | GC 正确回收, 数据完整 | ~30s |
| 2.2 | `kv_power_loss` | 写入中途复位 → init 恢复 | 数据不丢失, 状态一致 | ~5s |

**小计: 2 用例, ~35s**

### 2.3 TSDB 基础功能 (Test Suite 3)

| # | 用例 | 操作 | 验证点 | 预期耗时 |
|---|------|------|--------|----------|
| 3.1 | `ts_init_format` | 格式化 → init | head/tail 正确 | ~2s |
| 3.2 | `ts_append_query` | APPEND 100 条 → QUERY 区间 | 记录数匹配, 时间戳有序 | ~2s |
| 3.3 | `ts_latest_oldest` | APPEND 50 条 → GET_LATEST/OLDEST | 最旧/最新值正确 | ~1s |
| 3.4 | `ts_count_range` | COUNT + TIME_RANGE 验证 | 计数准确 | ~1s |
| 3.5 | `ts_reset_epoch` | 重置 epoch → 追加新数据 → 查询跨 epoch | 跨 epoch 查询不中断 | ~2s |

**小计: 5 用例, ~8s**

### 2.4 TSDB Rotation 压力 (Test Suite 4)

| # | 用例 | 操作 | 验证点 | 预期耗时 |
|---|------|------|--------|----------|
| 4.1 | `ts_rotation_30` | APPEND 直到 rotation≥30 次 | 旋转后数据连续 | ~20s |
| 4.2 | `ts_rotation_recovery` | 旋转中途复位 → init 恢复 | total_count 正确 | ~5s |

**小计: 2 用例, ~25s**

### 2.5 集成测试 (Test Suite 5)

| # | 用例 | 操作 | 验证点 | 预期耗时 |
|---|------|------|--------|----------|
| 5.1 | `mixed_kv_ts` | KVDB SET 20 keys + TSDB APPEND 200 条 | 双引擎互不干扰 | ~5s |
| 5.2 | `wear_heatmap` | KVDB + TSDB 持续写入, 查询磨损分布 | spread 合理 | ~30s |

**小计: 2 用例, ~35s**

---

## 3. 性能基准

### 3.1 预期值 (W25Q128 @ 40MHz SPI)

| 操作 | 预期延迟 | 计算方法 |
|------|---------|----------|
| **KV GET (cold, 64 扇区)** | ~2-5 ms | SPI 读 64×16B headers + Bloom 过滤扫描 |
| **KV SET (small, 20B)** | ~1-2 ms | SPI 写 header(16B)+key+value+commit + page program |
| **KV DELETE** | ~1-3 ms | SPI 读 + 1B mark_dead |
| **TS APPEND (64B)** | ~1-2 ms | SPI 写 record(12B)+data(64B)+commit |
| **TS QUERY (100 条)** | ~3-8 ms | SPI 顺序读 100×76B |
| **GC (zero-live)** | ~200-500 ms | SPI 擦除 4KB sector |
| **GC (50% live)** | ~1-3 s | 擦除 + 迁移 ~15 条 live 记录 |
| **INIT (64 扇区空)** | ~50-200 ms | SPI 读 64×16B headers |
| **INIT (64 扇区满)** | ~1-3 s | 扫描所有记录 + WRITING 恢复 |

### 3.2 性能验证脚本

```c
// 基准测试: 测量 100 次 SET + GET 的平均延迟
void bench_kv_set_get(void) {
    uint32_t t0 = HAL_GetTick();
    for (int i = 0; i < 100; i++) {
        char key[8]; snprintf(key, sizeof(key), "K%03d", i);
        rdb_kvdb_set(&db, key, test_val, sizeof(test_val));
    }
    uint32_t t_set = HAL_GetTick() - t0;

    t0 = HAL_GetTick();
    for (int i = 0; i < 100; i++) {
        char key[8]; snprintf(key, sizeof(key), "K%03d", i);
        uint16_t len; uint8_t buf[64];
        rdb_kvdb_get(&db, key, buf, sizeof(buf), &len);
    }
    uint32_t t_get = HAL_GetTick() - t0;

    printf("BENCH: 100 SET = %lums (avg %.1fms), 100 GET = %lums (avg %.1fms)\n",
           t_set, t_set/100.0f, t_get, t_get/100.0f);
}

// 基准测试: TSDB APPEND 吞吐
void bench_ts_append(void) {
    uint8_t data[64]; memset(data, 0xA5, sizeof(data));
    uint32_t t0 = HAL_GetTick();
    for (int i = 0; i < 500; i++)
        rdb_tsdb_append(&ts, i, data, sizeof(data));
    uint32_t elapsed = HAL_GetTick() - t0;
    printf("BENCH: 500 APPEND = %lums (%.0f records/s)\n",
           elapsed, 500000.0f / elapsed);
}
```

---

## 4. 掉电测试

### 4.1 测试方法

利用 STM32F407 的 BKP 寄存器 + RTC 实现受控掉电:

```c
// 掉电模拟流程:
// 1. 开始写操作
// 2. 在指定时间点触发 HardFault / NVIC_SystemReset()
// 3. 重启后检查 RTC BKP 寄存器 → 得知上次是掉电测试
// 4. 调用 rdb_kvdb_init() 恢复
// 5. 验证数据完整性

#define BKP_MAGIC   0x5A5A0001
#define BKP_STAGE   0x5A5A0002

void power_loss_test_write(void) {
    RTC->BKP0R = BKP_MAGIC;
    RTC->BKP1R = 1; // Stage 1: 写入 header 后掉电

    rdb_kvdb_set(&db, "PL_KEY", val, 64);
    // 在 set() 内部 write header 后触发 NVIC_SystemReset()
}

void power_loss_test_gc(void) {
    RTC->BKP0R = BKP_MAGIC;
    RTC->BKP1R = 2; // Stage 2: GC 迁移中掉电

    // 触发 GC 并在迁移中途复位
    for (int i = 0; i < 50; i++)
        rdb_kvdb_set(&db, key, val, 256);
    // GC 触发后 → NVIC_SystemReset()
}

void check_power_loss_recovery(void) {
    if (RTC->BKP0R == BKP_MAGIC) {
        uint32_t stage = RTC->BKP1R;
        printf("[RECOVERY] Power-loss stage %lu detected\n", stage);
        RTC->BKP0R = 0;
        // 验证所有已提交数据完整可读
        // 验证 write_seq 连续性
    }
}
```

### 4.2 掉电测试矩阵

| # | 掉电时机 | 预期行为 | 验证 |
|---|---------|---------|------|
| PL1 | SET: header 写入后, commit 前 | 旧值保留, 新值不存在 | GET 返回旧值或 NOT_FOUND |
| PL2 | SET: commit 写入中 | 旧值保留 或 新值有效 | GET 返回其中之一, 无损坏 |
| PL3 | SET: mark_dead 旧值前 | 两个 VALID 副本 | init dedup 清理旧副本 |
| PL4 | GC: 迁移中 | 部分记录在目标扇区 | init 重新 GC |
| PL5 | GC: 擦除 victim 中 | victim 可能 CORRUPT | init CORRUPT 恢复 |
| PL6 | TS APPEND: commit 前 | 记录不存在 | QUERY 不包含该记录 |
| PL7 | TS ROTATION: seal 中 | 旧 head 可能未 seal | init 降级 ACTIVE 恢复 |

---

## 5. 测试输出格式

### 5.1 UART 日志格式

```
========================================
  RocketDB STM32F407 Hardware Test v1.6.0
========================================
Build: Jun 19 2026 10:09:26
Flash: W25Q128 16MB @ 40MHz SPI
RAM free: ~186 KB

--- KVDB Basic Tests ---
[PASS] 1.1 kv_init_format          (1980ms)
[PASS] 1.2 kv_set_get_basic        ( 847ms)
[PASS] 1.3 kv_set_update           ( 612ms)
[PASS] 1.4 kv_delete_exists        ( 534ms)
[PASS] 1.5 kv_mixed_lengths        (2847ms)
[PASS] 1.6 kv_boundary             (1523ms)
[PASS] 1.7 kv_space_wear           ( 423ms)

--- KVDB GC Stress Tests ---
[PASS] 2.1 kv_gc_cycles_30         (28456ms, GC=32, set=420 get=85 del=12)
[PASS] 2.2 kv_power_loss           ( 4120ms, 5 stages recovered)

--- TSDB Basic Tests ---
[PASS] 3.1 ts_init_format          (1890ms)
[PASS] 3.2 ts_append_query         ( 1678ms)
[PASS] 3.3 ts_latest_oldest        ( 734ms)
[PASS] 3.4 ts_count_range          ( 512ms)
[PASS] 3.5 ts_reset_epoch          ( 1523ms)

--- TSDB Rotation Stress ---
[PASS] 4.1 ts_rotation_30          (18234ms, rot=32, lost=245)
[PASS] 4.2 ts_rotation_recovery    ( 3876ms)

--- Integration ---
[PASS] 5.1 mixed_kv_ts             ( 4234ms)
[PASS] 5.2 wear_heatmap            (28765ms)
  KV wear: min=2 max=5 avg=3 spread=3
  TS wear: min=2 max=8 avg=4 spread=6

--- Benchmarks ---
[BENCH] 100 KV SET: 184ms (avg 1.84ms)
[BENCH] 100 KV GET: 312ms (avg 3.12ms)
[BENCH] 500 TS APPEND: 967ms (517 rec/s)

========================================
  RESULTS: 18/18 PASSED (0 failures)
  Total time: 98s
========================================
```

---

## 6. 实施步骤

### Phase 1: 基础移植 (30 min)
1. 实现 `rdb_flash_ops_t` — 对接现有 `SPI_FLASH` 驱动
2. 配置 3 个 `rdb_partition_t` (KVDB / TSDB / 故障测试)
3. 编写 UART printf 重定向

### Phase 2: 基本功能验证 (30 min)
4. Test Suite 1: KVDB 7 用例
5. Test Suite 3: TSDB 5 用例
6. 集成测试 5.1 (KV+TS 混合)

### Phase 3: 压力与恢复 (30 min)
7. Test Suite 2: GC 压力 2 用例
8. Test Suite 4: Rotation 压力 2 用例
9. 磨损均衡测试 5.2

### Phase 4: 性能与掉电 (可选)
10. 性能基准测试
11. 受控掉电测试 (需要 NVIC_SystemReset + BKP 支持)

---

## 7. 预期资源占用 (ARM Thumb2)

| 组件 | ROM | RAM |
|------|-----|-----|
| RocketDB KVDB | ~12 KB | ~3.2 KB |
| RocketDB TSDB | ~7 KB | ~1.1 KB |
| SPI Flash driver | ~2 KB | ~0.5 KB |
| HAL / UART / SysTick | ~4 KB | ~0.5 KB |
| Test harness | ~3 KB | ~0.5 KB |
| **总计** | **~28 KB** | **~5.8 KB** |
| STM32F407 可用 | 1024 KB | 192 KB |
| 占用比例 | 2.7% | 3.0% |
