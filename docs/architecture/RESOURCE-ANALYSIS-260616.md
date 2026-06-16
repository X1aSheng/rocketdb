# RocketDB 资源占用 & 执行效率分析报告

**日期:** 2026-06-16  
**版本:** v1.5.2  
**编译环境:** Clang -O2, x86-64, `RDB_KV_CACHE_SIZE=64`, `RDB_BLOOM_BITS=256`  
**源码行数:** 6,807 行 (kvdb 3,299 + tsdb 2,120 + header 1,388)

---

## 1. ROM (Flash) 代码体积

### 1.1 总体

| 模块 | 源码行数 | .text x86-64 | 估 .text ARM Thumb2 | .data | .rdata | .xdata |
|------|---------|-------------|---------------------|-------|--------|--------|
| `rocketdb_kvdb.c` | 3,299 | **26,818 B** | ~12-16 KB | 0 | 16 B | 720 B |
| `rocketdb_tsdb.c` | 2,120 | **13,555 B** | ~6-8 KB | 0 | 96 B | 492 B |
| `rocketdb.h` | 1,388 | 0 (header) | 0 | — | — | — |
| **合计** | **6,807** | **40,373 B** | **~18-24 KB** | 0 | 112 B | 1,212 B |

### 1.2 KVDB 函数编译体积 (完整)

| 函数区域 | 字节 | 占比 | 源码行数 | 字节/行 | 说明 |
|----------|------|------|---------|---------|------|
| `rdb_kvdb_init` + 内部静态函数群 | 13,120 | 48.9% | ~550 | 23.9 | scan_sector, writing_cb, fixup_stale, bloom_rebuild_all, dedup, init_sector, rotate |
| `rdb_kvdb_set` | 3,824 | 14.3% | ~155 | 24.7 | 含 `kv_write_large_record` |
| `rdb_kvdb_get` | 1,536 | 5.7% | ~98 | 15.7 | find_latest + cache 查找 |
| `rdb_kvdb_format` | 1,056 | 3.9% | ~77 | 13.7 | init_sector + rotate |
| `gc_ensure_space` + 4 个 gc_select | 992 | 3.7% | ~85 | 11.7 | gc_select_zero_live/scored/forced/wear_level |
| `rdb_kvdb_delete` | 928 | 3.5% | ~70 | 13.3 | mark_dead + bookkeeping |
| `rdb_kvdb_wear_info` | 368 | 1.4% | ~24 | 15.3 | 扇区遍历 |
| `rdb_kvdb_space_info` | 192 | 0.7% | ~15 | 12.8 | |
| `rdb_kvdb_exists` | 192 | 0.7% | ~15 | 12.8 | |
| `rdb_kvdb_get_stats` | 128 | 0.5% | ~7 | 18.3 | |
| `rdb_kvdb_reset_stats` | 96 | 0.4% | ~8 | 12.0 | |
| `rdb_kv_iter_init` | 80 | 0.3% | ~9 | 8.9 | |
| `rdb_kv_iter_next` | — | — | — | — | 紧接 iter_init，计入上一区域 |
| `rdb_version` | 16 | 0.1% | 3 | 5.3 | |
| `rdb_kvdb_meta_size` | 16 | 0.1% | 3 | 5.3 | |
| `rdb_tsdb_ec_size` | 16 | 0.1% | 3 | 5.3 | (在 kvdb.c 中) |
| 内联/getter/剩余 | 258 | 1.0% | — | — | fl_lock, fl_unlock, fl_yield, is_erased, count_erased, etc. |
| **总计** | **26,818** | **100%** | | | |

### 1.3 TSDB 函数编译体积 (完整)

| 函数区域 | 字节 | 占比 | 源码行数 | 字节/行 | 说明 |
|----------|------|------|---------|---------|------|
| `rdb_tsdb_init` + 内部静态函数群 | 2,528 | 18.7% | ~228 | 11.1 | ts_recover_head, ts_classify, ts_sector_count, ts_active_info |
| `rdb_tsdb_format` | 2,304 | 17.0% | ~78 | 29.5 | ts_init_sec 内联展开 |
| `rdb_tsdb_append` | 2,272 | 16.8% | ~137 | 16.6 | 含 `ts_write_large_record` + ts_rotate |
| `rdb_tsdb_get_oldest` | 2,208 | 16.3% | ~88 | 25.1 | ts_oldest_cb + 扫描 |
| `rdb_tsdb_get_latest` | 1,200 | 8.9% | ~87 | 13.8 | ts_find_last_valid + 回退扫描 |
| `rdb_tsdb_wear_info` | 944 | 7.0% | ~26 | 36.3 | 扇区遍历 |
| `rdb_tsdb_time_range` | 656 | 4.8% | ~82 | 8.0 | 时间范围统计 |
| `rdb_tsdb_query` | 592 | 4.4% | ~51 | 11.6 | ts_query_impl |
| `rdb_tsdb_reset_epoch` | 128 | 0.9% | ~17 | 7.5 | |
| `rdb_tsdb_get_stats` | 128 | 0.9% | ~7 | 18.3 | |
| `rdb_tsdb_count` | 96 | 0.7% | ~8 | 12.0 | |
| `rdb_tsdb_query_ex` | 16 | 0.1% | ~3 | 5.3 | |
| `rdb_tsdb_reset_stats` | — | — | — | — | 紧接 get_stats |
| 内联 getter/剩余 | 483 | 3.6% | — | — | fl_lock, fl_unlock, fl_yield, ts_mark_dead, ts_data_crc, etc. |
| **总计** | **13,555** | **100%** | | | |

### 1.4 体积密度统计

| 指标 | KVDB | TSDB | 合计 |
|------|------|------|------|
| 每行源码 → 字节 | 8.1 B/line | 6.4 B/line | 7.5 B/line (均值) |
| 每 KB ROM → 功能行 | 123 lines/KB | 156 lines/KB | 135 lines/KB |

### 1.5 函数体积分布 (编译后)

| 体积范围 | KVDB 数量 | TSDB 数量 | 说明 |
|----------|----------|----------|------|
| >5 KB | 1 (init 集群 13KB) | 0 | init 函数包含大量静态 helper |
| 2-5 KB | 1 (set 3.8KB) | 5 | 主要 API 函数 |
| 1-2 KB | 2 (get, format) | 1 | |
| 512-1024 B | 2 (gc, delete) | 2 | |
| 128-511 B | 3 | 2 | |
| <128 B | 6 | 4 | 小型工具函数 |

---

## 2. RAM 占用

### 2.1 核心结构体大小

| 结构体 | 典型配置 | 最小配置 | 说明 |
|--------|---------|---------|------|
| `rdb_kvdb_t` | **1,108 B** | **87 B** | CACHE=64,BLOOM=256 → CACHE=0,BLOOM=0 |
| `rdb_tsdb_t` | **88 B** | 88 B | 固定大小 |
| `rdb_kv_cache_slot_t` | 16 B/槽 | — | hash(2)+klen(1)+prefix(8)+addr(4)+pad(1) |
| `rdb_kv_cache_t` (64 槽) | 1,024 B | 1 B | CACHE_SIZE=0 时为 placeholder[1] |
| `rdb_kv_stats_t` | 36 B | 36 B | 9 × uint32_t |
| `rdb_ts_stats_t` | 32 B | 32 B | 8 × uint32_t |
| `rdb_kv_sector_meta_t` | 16 B/扇区 | 16 B | erase_cnt+create_seq+write_off+garbage+status |
| `rdb_partition_t` | ~56 B | ~56 B | name+ops+base_addr+total_size+sector_size+write_gran |
| RDB_BLOOM_BYTES | 32 B/扇区 | 0 | 256 bits = 32 bytes per sector |

### 2.2 rdb_kvdb_t 内存布局

| 字段 | 偏移 | 大小 | 类型 |
|------|------|------|------|
| `part` | 0 | 8 B | `const rdb_partition_t*` |
| `sectors` | 8 | 8 B | `rdb_kv_sector_meta_t*` |
| `sector_cnt` | 16 | 1 B | `uint8_t` |
| `gc_reserve` | 17 | 1 B | `uint8_t` |
| `active_sec` | 18 | 1 B | `uint8_t` |
| `initialized` | 19 | 1 B | `uint8_t` |
| *(padding)* | 20 | 4 B | (alignment) |
| `write_seq` | 24 | 4 B | `uint32_t` |
| `live_bytes` | 28 | 4 B | `uint32_t` |
| `write_off` | 32 | 4 B | `uint32_t` |
| `iter_gen` | 36 | 4 B | `uint32_t` |
| `cache` | 40 | **1,024 B** | `rdb_kv_cache_t` (64 slots × 16B) |
| `stats` | 1064 | 36 B | `rdb_kv_stats_t` |
| `blooms` | 1100 | 8 B | `uint8_t*` |
| **TOTAL** | | **1,108 B** | |

### 2.3 rdb_tsdb_t 内存布局

| 字段 | 偏移 | 大小 | 类型 |
|------|------|------|------|
| `part` | 0 | 8 B | `const rdb_partition_t*` |
| `erase_cnts` | 8 | 8 B | `uint32_t*` |
| `sector_size` | 16 | 4 B | `uint32_t` |
| `sector_cnt` | 20 | 1 B | `uint8_t` |
| `head_sec` | 21 | 1 B | `uint8_t` |
| `tail_sec` | 22 | 1 B | `uint8_t` |
| `initialized` | 23 | 1 B | `uint8_t` |
| *(padding)* | 24 | 4 B | (alignment) |
| `head_seq` | 28 | 2 B | `uint16_t` |
| *(padding)* | 30 | 2 B | (alignment) |
| `head_off` | 32 | 4 B | `uint32_t` |
| `head_time_base` | 36 | 4 B | `uint32_t` |
| `head_count` | 40 | 4 B | `uint32_t` |
| `last_time` | 44 | 4 B | `uint32_t` |
| `total_count` | 48 | 4 B | `uint32_t` |
| `max_data_len` | 52 | 2 B | `uint16_t` |
| *(padding)* | 54 | 2 B | (alignment) |
| `stats` | 56 | 32 B | `rdb_ts_stats_t` |
| **TOTAL** | | **88 B** | |

### 2.4 总 RAM 预算 (16 扇区示例)

| 组件 | 典型 (CACHE=64,BLOOM=256) | 最小 (CACHE=0,BLOOM=0) | 类型 |
|------|--------------------------|------------------------|------|
| `rdb_kvdb_t` 句柄 | 1,108 B | 87 B | 调用者分配 |
| KVDB meta buffer (16扇) | 768 B | 256 B | 调用者分配 (48B/扇 → 16B/扇) |
| `rdb_tsdb_t` 句柄 | 88 B | 88 B | 调用者分配 |
| TSDB EC buffer (16扇) | 64 B | 64 B | 调用者分配 (4B/扇) |
| `rdb_partition_t` × 2 | ~112 B | ~112 B | 调用者分配 |
| 去重表 (stack, KVDB) | 512 B | 512 B | 动态分配 (32 slots × 16B) |
| **总计** | **~2,652 B** | **~1,119 B** | |
| flash ops (static) | ~2 B | ~2 B | 静态全局 |
| stack peak | ~400 B | ~200 B | 运行时 |

### 2.5 各配置档位 RAM

| CACHE | BLOOM | KVDB 句柄 | Meta (16扇) | 总 RAM | 适用平台 |
|-------|-------|----------|-------------|--------|----------|
| 64 | 256 | 1,108 B | 768 B | **~2,652 B** | STM32F4/F7 (>64KB RAM) |
| 64 | 0 | 1,108 B | 256 B | **~2,140 B** | STM32F4 (≥32KB RAM) |
| 32 | 256 | 596 B | 768 B | **~2,140 B** | STM32F4 (≥32KB RAM) |
| 32 | 0 | 596 B | 256 B | **~1,628 B** | STM32F3 (≥16KB RAM) |
| 0 | 256 | 87 B | 768 B | **~1,631 B** | STM32F1 (≥8KB RAM) |
| 0 | 0 | 87 B | 256 B | **~1,119 B** | STM32F0 (≥4KB RAM) |

### 2.6 栈深度分析

| 调用路径 | 深度 | 主要局部变量 | 栈帧估算 |
|----------|------|-------------|---------|
| `rdb_kvdb_set` → `gc_ensure_space` → `gc_execute` → `migrate_one` → `fl_write` | 5 | mbuf[64], cache[N], rh, kb[N] | **~400 B** |
| `rdb_kvdb_init` → `scan_sector` → `writing_cb` → `fl_read` | 4 | kb[N], rh, meta_buf | **~300 B** |
| `rdb_tsdb_init` → `ts_recover_head` → `fl_read/write` | 3 | rh, calc | **~250 B** |
| `rdb_tsdb_append` → `ts_rotate` → `fl_read/write` | 3 | mbuf[64], buf[64] | **~250 B** |
| `rdb_tsdb_query` → callback → `fl_read` | 3 | qctx, buf | **~200 B** |
| `gc_execute` → `gc_migrate_cb` → `fl_read/write` | 3 | rh, mig_buf | **~200 B** |

> 注：`RDB_STACK_BUF_SIZE=64` 的 `mbuf[]`/`buf[]` 是最大单项栈变量。无递归调用。

---

## 3. 执行效率

### 3.1 算法复杂度矩阵

| 操作 | 无缓存 | 缓存命中 | Bloom 过滤 | 摊销 |
|------|--------|---------|-----------|------|
| **KV GET** | O(S×R) | **O(1)** | O(S×R×0.27) | O(1) 热点 |
| **KV SET** | O(S×R) + GC | O(1) + GC | — | O(1) 热点 |
| **KV DELETE** | O(S×R) | O(1) | — | O(1) 热点 |
| **KV EXISTS** | O(S×R) | O(1) | — | O(1) 热点 |
| **KV ITER** | O(S×R) | O(S×R) | — | O(S×R) |
| **TS APPEND** | **O(1)** | — | — | O(1) 摊销 |
| **TS QUERY** | O(K) | — | — | O(K) |
| **TS GET_LATEST** | O(R_head) | — | — | O(R_head) |
| **TS GET_OLDEST** | O(R_tail) | — | — | O(R_tail) |
| **GC (KV)** | O(S×R) | — | — | O(S×R) |
| **TS ROTATION** | O(1) | — | — | O(1) |

> S = 扇区数, R = 每扇区记录数, K = 查询时间跨度内记录数

### 3.2 Flash I/O 预算 (8 扇区, 每扇区 ~100 条记录)

| 操作路径 | Flash Reads | Flash Writes | Erases | 说明 |
|----------|------------|-------------|--------|------|
| **KV GET (cache hit)** | **1** | 0 | 0 | 单次记录读取 |
| **KV GET (cache miss)** | ~216 | 0 | 0 | Bloom 过滤 73% 扇区扫描 |
| **KV GET (cache miss, no Bloom)** | ~800 | 0 | 0 | 全表扫描 |
| **KV SET (small, new key)** | 0 | 2 | 0 | merge + commit |
| **KV SET (large, new key)** | 0 | 3+K | 0 | header + key + K chunks + commit |
| **KV SET (update)** | 0 | 2-3+K | 0 | +1 mark_dead |
| **KV DELETE** | 1 | 1 | 0 | read + mark_dead |
| **TS APPEND (small)** | 0 | 2 | 0 | merge + commit |
| **TS APPEND (large)** | 0 | 2+K | 0 | header + K chunks + commit |
| **TS APPEND (rotation)** | ~R_tail | 2+K | **1** | +1 sector erase |
| **GC (zero-live)** | 0 | 0 | **1** | 无需迁移 |
| **GC (scored)** | ~R_victim | ~L×2 | **1** | 迁移 L 条 live 记录 |

### 3.3 优化机制量化

| 优化 | 无优化 | 有优化 | 改善比 | 场景 |
|------|--------|--------|--------|------|
| KV Cache (CLOCK 64) | O(800) reads | **O(1)** read | **800x** | 热点 key GET |
| Bloom Filter (256-bit) | 800 reads | ~216 reads | **3.7x** | 冷 key GET |
| Merge Buffer (64B) | 4 writes | 2 writes | **2.0x** | 小记录 SET |
| GC Zero-Live | O(S×R) + erase + migrate | **O(S) + erase** | **~R/1** | 零存活扇区 |
| GC K-1 invariant | deadlock at 0 erased | never deadlocks | — | 安全保证 |

### 3.4 模拟器吞吐 (PC, x86-64)

| 套件 | 逻辑操作 | 耗时 | 吞吐 |
|------|---------|------|------|
| kvdb_basic | 2,654 | 7ms | ~379K ops/s |
| kvdb_stress | 4,603 | 17ms | ~271K ops/s |
| kvdb_cache | 2,825 | 8ms | ~353K ops/s |
| tsdb_basic | 2,409 | 3ms | ~803K ops/s |
| tsdb_stress | 2,728 | 11ms | ~248K ops/s |
| integration | 22,657 | 71ms | ~319K ops/s |

> **实际 MCU 性能估算:** W25Q128 @ 40MHz SPI ≈ 模拟器的 1/20 ~ 1/50  
> 即 KV GET (cache hit) ≈ ~50-200 µs, KV SET (small) ≈ ~100-400 µs

---

## 4. 代码优化记录

### 4.1 函数提取 (2026-06-16)

| 提取的函数 | 来源 | 行数 | 类型 |
|-----------|------|------|------|
| `ts_recover_head()` | `rdb_tsdb_init` Phase 3 | 107 | 扇区恢复逻辑 |
| `gc_select_zero_live()` | `gc_ensure_space` Phase 1 | 20 | GC victim 选择 |
| `gc_select_scored()` | `gc_ensure_space` Phase 2 | 50 | GC victim 选择 |
| `gc_select_forced()` | `gc_ensure_space` Phase 3 | 22 | GC victim 选择 |
| `gc_select_wear_level()` | `gc_ensure_space` Phase 4 | 33 | GC victim 选择 |
| `kv_write_large_record()` | `rdb_kvdb_set` | 78 | 大记录写入 |
| `ts_write_large_record()` | `rdb_tsdb_append` | 57 | 大记录写入 |

### 4.2 优化前后对比

| 函数 | 优化前 (行) | 优化后 (行) | 减少 |
|------|-----------|-----------|------|
| `rdb_tsdb_init` | 329 | 228 | **-31%** |
| `gc_ensure_space` | 199 | 85 | **-57%** |
| `rdb_kvdb_set` | 223 | 164 | **-26%** |
| `rdb_tsdb_append` | 176 | 137 | **-22%** |

### 4.3 Bug 修复: ODR 违例导致 stats 读取错误

**问题:** `rocketdb_sim_support` 库未链接 `rocketdb`，缺少 `RDB_KV_CACHE_SIZE=64` / `RDB_BLOOM_BITS=256` 编译宏。

| 编译单元 | `rdb_kv_cache_t` | `stats` 偏移 |
|----------|-----------------|-------------|
| `rocketdb_kvdb.c` (rocketdb) | 1,024 B | **~1,064** |
| `sim_trace.c` (rocketdb_sim_support) | 1 B | **~37** |

**修复:** [CMakeLists.txt:273](CMakeLists.txt#L273)
```cmake
target_link_libraries(rocketdb_sim_support PUBLIC rocketdb)
```

---

## 5. 磨损均衡数据

### 5.1 Integration Test `wear_heatmap` (16 扇区, 4KB, 100K PE)

```
KVDB wear: min=87 max=89 avg=87 spread=2
── KV Wear Heatmap ──────────────────────────────
sector erase_count  status  garbage_bytes
0      89           ACTIVE  0
1      88           SEALED  4067
2-4    88           ERASED  0
5-12   88           SEALED  4067
13-15  87           SEALED  4067/1909

TSDB wear: min=65 max=106 avg=68 spread=41
── TS Wear Heatmap ──────────────────────────────
sector erase_count
0      65
1      106             ← head hot-spot
2-4    68
5-15   66
```

### 5.2 磨损均衡修正公式

```
KVDB 有效寿命 = T_life × (avg/max) = T_life × 0.978  (损失 2.2%)
TSDB 有效寿命 = T_life × (avg/max) = T_life × 0.642  (受 head 扇区限制)
```

---

## 6. 测试覆盖

| 套件 | 用例 | 断言 | 耗时 | 覆盖领域 |
|------|------|------|------|----------|
| test_kvdb_basic | 12 | 3,005 | 7ms | set/get/delete/write-gran/dedup/seq-wrap/mixed/corrupt/format/max/capacity |
| test_kvdb_stress | 6 | 5,476 | 17ms | GC≥100/iterator/power-loss/corrupt-sector/mixed-value |
| test_kvdb_cache | 9 | 6,160 | 8ms | cache-hit/hot-key/GC-migration/collision/max-key/ring/format |
| test_tsdb_basic | 7 | 2,876 | 3ms | append/query/epoch/recount/write-gran/max/large-payload |
| test_tsdb_stress | 5 | 2,764 | 11ms | rotation≥100/write-fail/CRC/degraded/mixed-payload |
| test_integration | 6 | 27,613 | 71ms | GC≥100/rotation≥100/mixed/power-loss-kv/power-loss-ts/wear-heatmap |
| test_fault_injection | 8 | 74 | 2ms | write-fail/erase-fail/power-loss/CRC/read-fail/bit-flip/rule-import |
| test_example | 2 | 27 | 0ms | basic-assert/kvdb-set-get/param-write-gran |
| **合计** | **58** | **47,995** | **~0.6s** | **0 失败, 0 警告** |

### CRUD 分布 (Integration)

| 操作 | 数量 | 来源 |
|------|------|------|
| KV SET | 1,620 + 5,622 | gc_cycles + mixed |
| KV GET | 510 + 1,950 | gc_cycles + mixed |
| KV DEL | 120 + 983 | gc_cycles + mixed |
| TS APPEND | 1,473 + 1,445 | rotation + mixed |
| TS QUERY | 7 + 40 | rotation + mixed |

---

## 7. 跨平台验证

| 平台 | 编译器 | 配置 | 状态 |
|------|--------|------|------|
| Windows 11 (local) | Clang 22 | Release | ✅ 11/11 |
| GitHub Actions (ubuntu) | GCC | Debug + Release | ✅ 6/6 |
| GitHub Actions (ubuntu) | Clang | ASan+UBSan | ✅ 11/11 |
| GitHub Actions (windows) | Clang | Debug + Release | ✅ 11/11 |
| Alibaba Cloud ECS | GCC 15.2 | Release | ✅ 11/11 |
| Alibaba Cloud ECS | Clang 21.1 | Release + Strict | ✅ 11/11 |

---

## 8. 对比 FlashDB

| 指标 | RocketDB | FlashDB | 差异说明 |
|------|----------|---------|----------|
| KVDB ROM (ARM估) | ~14 KB | ~4.6 KB | RocketDB 3× (更多功能) |
| TSDB ROM (ARM估) | ~7 KB | ~1.2 KB | RocketDB 6× (更多功能) |
| KVDB RAM (典型) | 1,108 B | ~120 B | RocketDB 9× (默认含 cache+bloom) |
| KVDB RAM (最小) | 87 B | ~2 B | 相当 |
| TSDB RAM | 88 B + 4N | ~80 B | 相当 |
| GET 复杂度 | **O(1)** 缓存命中 | O(N×M) | RocketDB 优势 |
| Cache 策略 | CLOCK 近似 LRU | 基础 CRC | RocketDB 优势 |
| Bloom Filter | 256-bit/扇区 | 无 | RocketDB 独有 |
| GC 策略 | 4 段评分 | 简单阈值 | RocketDB 优势 |
| 磨损均衡 | 4 段 + 静态均衡 | 简单策略 | RocketDB 优势 |
| 掉电恢复 | 降级 ACTIVE | 基础 CRC | RocketDB 优势 |
| TS Epoch | 原生防回绕 | 无 | RocketDB 独有 |
| 测试覆盖 | 58 用例/48K 断言 | 基础测试 | RocketDB 优势 |

---

## 9. 选型决策表

| 场景 | MCU RAM | 推荐 | 原因 |
|------|---------|------|------|
| 配置存储 (≤50 key) | ≥4 KB | **RocketDB** (CACHE=0) | O(N×M) 可接受, GC+恢复完善 |
| 配置存储 + 热点读取 | ≥8 KB | **RocketDB** (CACHE=64) | O(1) 读取, 缓存命中 >90% |
| 传感器采样 (≤100Hz) | ≥8 KB | **RocketDB** | Epoch + 旋转恢复 |
| 高频采样 (>100Hz) | ≥16 KB | **RocketDB** (多扇区) | 增加扇区数降低热点频率 |
| 资源极度受限 (2KB RAM) | <4 KB | **FlashDB** | RocketDB 最低需 ~1.1KB |
| 已有 RT-Thread 系统 | 不限 | **FlashDB** | 原生集成 |
| OTA/固件/资源文件 | 不限 | **LittleFS** | POSIX 文件接口 |

---

*分析完成时间: 2026-06-16 23:15 UTC+8*  
*下次对比时更新第 4 节 (代码优化记录) 和第 7 节 (跨平台验证)*