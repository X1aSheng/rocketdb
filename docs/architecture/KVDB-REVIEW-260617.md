# RocketDB KVDB 模块审核 & 优化方向分析

**日期:** 2026-06-17  
**版本:** post-optimization (v1.5.2+)  
**对比参考:** https://github.com/armink/FlashDB 2.2.0
              https://github.com/littlefs-project/littlefs 2.11.3 

---

## 1. 当前资源占用

### 1.1 ROM

```
rocketdb_kvdb.c:  3,214 行 → .text 26,338 B (~26 KB x86-64, 估 ~12-15 KB ARM Thumb2)
rocketdb_tsdb.c:  2,120 行 → .text 13,555 B (~13 KB x86-64, 估  ~6-8 KB ARM Thumb2)
rocketdb.h:       1,388 行 → header only
static library:   52,856 B (.lib 含元数据)

合计: 6,722 行源码, ~40 KB .text (x86-64), ~18-23 KB (ARM估)
```

### 1.2 RAM

| 组件 | 典型 | 最小 | FlashDB等效 |
|------|------|------|------------|
| KVDB 句柄 (CACHE=64, BLOOM=256) | **1,108 B** | 87 B | ~120 B |
| KVDB meta buffer (16扇区) | 768 B | 256 B | ~128 B/扇区 |
| TSDB 句柄 | 88 B | 88 B | ~80 B |
| TSDB EC buffer | 64 B | 64 B | ~32 B |
| 去重表 (stack, init期间) | 512 B | 512 B | — |
| Stack peak (init) | ~800 B | ~200 B | ~500 B |
| **总计 (典型)** | **~2,652 B** | **~1,119 B** | **~200-500 B** |

### 1.3 执行效率

| 操作 | RocketDB | FlashDB | 优势方 |
|------|----------|---------|--------|
| KV GET (hot key) | **O(1)** cache | O(S×R) 全扫 | RocketDB |
| KV GET (cold key) | O(S×R×0.27) Bloom | O(S×R) | RocketDB |
| KV SET | O(1)+GC | O(S×R)+GC | RocketDB |
| KV DELETE | O(S×R) | O(S×R) | 持平 |
| TS APPEND | O(1) | O(1) | 持平 |
| INIT scan | O(S×R)×**1** | O(S×R)×3 | RocketDB |
| GC | 4阶段评分 | 简单阈值 | RocketDB |

---

## 2. 对比 FlashDB 设计

### 2.0 FlashDB 2.2.0 实测数据 (STM32F4, IAR 8.20)

| 模块 | ro code | ro data | rw data | 说明 |
|------|---------|---------|---------|------|
| `fdb.o` (核心) | 276 B | 232 B | **1 B** | 全局状态 |
| `fdb_kvdb.o` | **4,584 B** | 356 B | 1 B | KVDB 引擎 |
| `fdb_tsdb.o` | 1,160 B | 236 B | — | TSDB 引擎 |
| `fdb_utils.o` | 418 B | 1,024 B | — | 工具函数 |
| **合计** | **6,438 B** | 1,848 B | **2 B** | |

**TSDB 性能 (W25Q64 NOR Flash):**
- 插入: 250 条/秒, 4.00 ms/条
- 查询 (1,251 条): 平均 1.77 ms

**TSDB 性能 (STM32F2 片上 Flash):**
- 插入: 2,684 条/秒, 0.37 ms/条
- 查询 (13,422 条): 平均 0.11 ms

> RocketDB 对比: KVDB 引擎 ~26.3 KB (x86-64), 估 ~14 KB ARM Thumb2 — 约为 FlashDB KVDB (4.6 KB) 的 **3×**。但 RocketDB 包含 cache/bloom/4段GC/去重表等 FlashDB 不具备的功能。

### 2.1 FlashDB 的优势模式

**A. 极致精简的句柄 (120 B vs 1108 B)**
FlashDB 的 KVDB 句柄不含 cache/bloom，核心字段 <20 个。RocketDB 默认携带 1024B cache + 32B/sector bloom。
- **优化方向 1:** 将默认 `RDB_KV_CACHE_SIZE` 从 64 降为 0（或 8），让用户显式 opt-in
- **收益:** 默认 RAM -1023 B，降低入门门槛
- **风险:** 冷启动场景 GET 性能退化到 O(S×R)

**B. 更简单的扇区头 (12 B vs 16 B)**
FlashDB 扇区头不含 version 字段和独立 CRC。RocketDB 使用 16B 支持版本兼容 CRC。
- **优化方向 2:** 移除 `RDB_KV_VERSION_OLD` 支持，统一为新格式
- **收益:** ROM -60 B（简化 CRC 验证分支），扇区头可降至 14 B
- **风险:** 不再向后兼容旧版本格式

**C. 无 Bloom 依赖快速路径**
FlashDB 无 Bloom filter，对小型配置（<32 keys）全表扫描开销可接受。
- **优化方向 3:** 当 `sector_cnt ≤ 2` 时自动跳过 Bloom（扇区太少，过滤收益低）
- **收益:** 减少不必要的 Flash 读取（跳过 Bloom 查询）
- **风险:** 无（纯优化）

### 2.2 RocketDB 对 FlashDB 的已有优势

| 特性 | RocketDB 优势 | 保持 |
|------|-------------|------|
| 4 段 GC | 零存活极速回收 + 评分选择 + 强制降级 + 静态磨损 | ✅ |
| CLOCK Cache | 近似 LRU，热点命中 >90% | ✅ |
| Bloom Filter | 快速跳过不含 key 的扇区 (-73% 无效读) | ✅ |
| Init 合并扫描 | 1 遍 vs 3+ 遍 | ✅ |
| 降级 ACTIVE 恢复 | seal CRC 失败保留数据 (vs FlashDB 擦除) | ✅ |
| 去重表 (hash-set) | O(1) 去重 vs O(N²) find_latest | ✅ |

---

## 3. 对比 LittleFS 设计

### 3.0 LittleFS 2.11.3 架构要点

| 参数 | 典型值 | 说明 |
|------|--------|------|
| `read_size` | 16 B | 最小读取单元 |
| `prog_size` | 16 B | 最小编程单元 |
| `block_size` | 4,096 B | 擦除块大小 |
| `block_count` | 128 | 总块数 (~512 KB) |
| `cache_size` | 16 B | 每文件/元数据 RAM 缓冲 |
| `lookahead_size` | 16 B | 空闲块位图缓冲 |
| `block_cycles` | 500 | 每块分配周期擦除上限 |

**双层架构:**
```
Small logs (metadata pairs)  →  快速元数据更新，两副本原子切换
Larger COW data structures   →  紧凑存储文件数据，无写放大
```

**Bounded RAM:** RAM 使用量完全由 `cache_size` + `lookahead_size` 静态配置决定，不随文件数或文件系统大小增长。无动态内存分配，无递归。

**动态磨损均衡:** 通过 `block_cycles` 限制每块在分配周期内的擦除次数，配合遍历所有块的 block allocator 实现全局磨损均衡。

### 3.1 LittleFS 的优势模式

**A. COW 元数据对 (Metadata Pairs)**
LittleFS 使用两个可互换的元数据块，写入新版本时原子替换。掉电后总有一个有效版本。
- **优化方向 4:** 将 KVDB 的 ACTIVE/SEALED 扇区管理借鉴 COW 思想
  - 当前: 单个 ACTIVE 扇区 + 多个 SEALED，切换需 rotate
  - 可考虑: 双 ACTIVE 扇区轮换写入，减少 rotate 频率
- **收益:** 减少 rotate 次数 → 减少擦除 → 延长寿命
- **风险:** 中（需重新设计扇区状态机）

**B. Bounded RAM (不随文件系统增大)**
LittleFS 的核心保证: RAM 使用量与文件系统大小无关。
- **优化方向 5:** RocketDB 的 Bloom 使用 `N × 32B` RAM（随扇区数线性增长）
  - 可考虑: 全局 Bloom（固定大小，不随扇区增长）
  - 或: 仅对 ACTIVE 扇区维护 Bloom
- **收益:** 16 扇区省 480 B RAM（仅保留 ACTIVE 扇区 Bloom），32 扇区省 992 B
- **风险:** GET 性能退化（需扫描无 Bloom 的 SEALED 扇区）

**C. 动态磨损均衡**
LittleFS 在运行时根据擦除计数动态重新分配块，而非仅在 GC 时触发。
- **优化方向 6:** KVDB 的 Phase 4 静态磨损均衡仅在所有 GC 阶段失败后触发
  - 可改为: 每次 rotate 时主动选择最低 EC 的 erased 扇区（已实现）
  - 进一步: 定期检查所有 SEALED 扇区的 EC 分布，主动迁移冷数据
- **收益:** 更均匀的磨损分布（当前 KV spread=2 已很好）
- **风险:** 低（spread=2 已经接近完美，改进空间有限）

**D. 内联文件 (Inline Files)**
LittleFS 对小文件（≤1/4 block）直接存储在目录元数据中，避免单独分配 block。
- **优化方向 7:** KVDB 可对小 value（≤8B）直接嵌入 record header 的保留字段
  - 当前 record header 有 `_pad0` (1B) + `_pad1` (2B) = 3B 可用
  - 可将 val_len ≤ 某个阈值时使用内联存储
- **收益:** 小 value 记录减少 1 次 Flash 读取（不需单独读 value 段）
- **风险:** 需修改 record header 格式（breaking change）

---

## 4. 深度实现对比 (源码级)

### 4.1 扇区头设计

| 字段 | RocketDB (16 B) | FlashDB (~20 B aligned) | 说明 |
|------|----------------|------------------------|------|
| magic | 4 B `KVSN` | 4 B `FDB1` | 相同 |
| 状态 | 1 B (RAM only) | 2× status_table (on-flash) | FlashDB 状态持久化在 Flash 上 |
| 序列号 | 4 B create_seq | — | RocketDB 独有 (支持 dedup) |
| 擦除计数 | 4 B | — | RocketDB 独有 (磨损追踪) |
| CRC | 2 B (version-aware) | — (magic 隐式校验) | RocketDB 更安全 |
| 组合/剩余 | — | 8 B combined + reserved | FlashDB 支持多扇区块 |
| **RAM 副本** | 16 B/扇区 (meta) | 缓存按需 | RocketDB 全量缓存, FlashDB 惰性 |

**洞察:** RocketDB 将元数据全部缓存在 RAM (16B/扇区), FlashDB 使用小缓存 + 按需从 Flash 读取。RocketDB 的空间换时间。

### 4.2 记录头设计

| 字段 | RocketDB (16 B) | FlashDB (~24 B aligned) | 说明 |
|------|----------------|------------------------|------|
| magic | 1 B | 4 B | RocketDB 更紧凑 |
| state | 1 B | status_table | NOR-safe 单字节过渡 |
| key_len | 1 B | 1 B | 相同 |
| val_len | 2 B | 4 B | RocketDB 更紧凑 |
| key_hash | 2 B | — (嵌入 CRC32) | RocketDB 快速过滤 |
| seq | 2 B | — | RocketDB 独有 (版本控制) |
| data_crc | 2 B | 4 B CRC32 | CRC16 vs CRC32 |
| name/value | 分离存储 | 连续存储 | — |

**洞察:** RocketDB 的 16B 记录头比 FlashDB 的 24B 更紧凑。FlashDB 使用 CRC32 (4B) 比 RocketDB 的 CRC16 (2B) 更强但更大。

### 4.3 GC 策略对比

| 维度 | RocketDB | FlashDB | LittleFS |
|------|----------|---------|----------|
| 触发条件 | 空间不足 + K-1 水位 | 空间不足 + dirty sector 阈值 | revision % block_cycles |
| 选择策略 | **4 阶段评分** | 遍历 dirty sectors | 遍历 metadata blocks |
| 迁移方式 | 逐条 migrate_one | 逐条 move_kv | 目录压缩 (compact) |
| 擦除后 | 更新 EC + 标记 ERASED | 格式化扇区 | 释放旧 block |
| 磨损均衡 | Phase 4 静态 + Phase 2 评分 | 无显式策略 | 动态 (revision-based) |
| 掉电恢复 | GC 中断 → 下次 init 重做 | PRE_DELETE 状态恢复 | COW 原子切换 |

### 4.4 缓存策略对比

| 维度 | RocketDB (CLOCK) | FlashDB (LRU-aging) |
|------|-----------------|---------------------|
| 槽大小 | **16 B** (hash+klen+prefix+addr) | ~12 B (addr+name_crc+active) |
| 淘汰策略 | CLOCK (1-bit reference) | LRU aging (active counter) |
| 冲突解决 | 线性探测 | 线性探测 |
| 完整性验证 | prefix(8B) + 可选全 key 回读 | name_crc + 全 name 回读 |
| 默认大小 | 64 槽 (1024 B) | 可配置 (通常更小) |

**洞察:** RocketDB 的 cache slot 通过 8B prefix 提供更强的冲突分辨能力 (vs FlashDB 的 2B CRC16)，但代价是每槽 +6B。

### 4.5 初始化恢复对比

| 步骤 | RocketDB | FlashDB |
|------|----------|---------|
| 扇区分类 | 读所有头 + CRC 验证 → ERASED/ACTIVE/CORRUPT | 读所有头 → 检查 magic |
| WRITING 恢复 | CRC-based promote/demote | PRE_WRITE → ERR_HDR, PRE_DELETE → move_kv |
| 去重 | **hash-set + newest-first** | 无 (每次 SET 覆盖旧值) |
| GC 恢复 | 安全水位检查 | GC resume + retry loop |
| 扫描遍数 | **1 遍** (合并) | 2+ 遍 |

---

## 5. 具体优化方案 (含实现细节)

### 5.1 借鉴 FlashDB: 扇区状态持久化

**当前:** RocketDB 扇区状态 (ERASED/ACTIVE/SEALED/CORRUPT) 仅存 RAM，init 时重新分类。
**FlashDB 方式:** 状态写入 Flash 扇区头，掉电后直接读取。
**方案:** 在扇区头增加 1B `status` 字段 (NOR-safe: 0xFF→0xFE→0xFC)。

```c
// 当前扇区头 (16B):
magic(4) + seq(2) + ec(4) + create_seq(4) + hdr_crc(2)

// 优化后扇区头 (16B, 重排):
magic(4) + status(1) + seq(2) + ec(4) + create_seq(4) + hdr_crc(1)
// 从 version-aware CRC 改为固定 CRC，释放 1B
```

**收益:** 跳过 `is_erased()` 三点探测 (每 erased 扇区省 3 次 Flash 读取)，`RDB_SEC_SEALED` 可直接从 Flash 读取
**ROM:** +10 B (状态写入逻辑)
**风险:** 低（仅影响 init 路径）

### 5.2 借鉴 LittleFS: 内联小 Value

**当前:** 所有 value 独立存储在记录头之后，需单独的 Flash 读取。
**LittleFS 方式:** `LFS_F_INLINE` — 小文件直接嵌入目录元数据。
**方案:** 利用 record header 的 `_pad0` (1B) + `_pad1` (2B) = 3B + 可扩展至 8B。

```c
// 当前 record header (16B):
magic(1) + state(1) + key_len(1) + _pad0(1) + val_len(2) 
+ key_hash(2) + seq(2) + data_crc(2) + _pad1(2)

// 优化: val_len ≤ 8 时, value 嵌入 _pad0+_pad1 区域
// 新增 RDB_KV_INLINE_VAL 标志位 (复用 state 的高位)
```

**收益:** 小 value (≤8B) 省 1 次 Flash 读取，存储效率 +20% (省 alignment padding)
**ROM:** +30 B (内联读取路径)
**风险:** 中（修改 record header 语义，需版本号升级）

### 5.3 FlashDB 风格: Sector Cache 替代全量 Meta Buffer

**当前:** RocketDB 在 RAM 中缓存所有扇区的 `rdb_kv_sector_meta_t` (16B/扇区)。
**FlashDB 方式:** Sector cache table + 按需从 Flash 读取扇区信息。
**方案:** 可选配置 — 小 RAM 场景使用 sector cache (如 4 槽)，大 RAM 场景保持全量缓存。

```c
#if RDB_KV_SECTOR_CACHE_SIZE > 0
  // 使用 sector cache (LRU, N 槽)
#else
  // 全量 meta buffer (当前行为)
#endif
```

**收益:** 16 扇区省 256 B RAM，32 扇区省 512 B
**风险:** 中（需处理 cache miss → Flash 读取路径）

### 5.4 LittleFS 风格: 去重表可配置大小

**当前:** 去重表固定 32 槽 (512 B)，`RDB_DEDUP_SLOTS` 硬编码。
**LittleFS 方式:** 所有缓冲区大小由 `lfs_config` 静态配置。
**方案:** 将 `RDB_DEDUP_SLOTS` 改为编译时配置 `RDB_DEDUP_SLOTS`。

```c
#ifndef RDB_DEDUP_SLOTS
#define RDB_DEDUP_SLOTS 32u  // 默认 32, 可 override
#endif
```

**收益:** 小配置 (8 扇区) 可设为 8 槽 (128 B), 省 384 B
**ROM:** 0 (纯配置变更)
**风险:** 低

---

## 6. 更新优先级路线图 (含实现细节)

### 6.1 ROM 优化

| # | 方向 | 节省 | 难度 |
|---|------|------|------|
| R1 | `rdb_kvdb_format` 扇区头 CRC 验证复用 `kv_validate_sector_hdr` (已部分完成) | -30 B | 低 |
| R2 | 合并 `recalc_garbage` + `reconcile_live` 为单次扫描 (GC 路径, 参考 init 合并) | -80 B | 低 |
| R3 | `gc_execute` 的 ec_range 计算可缓存 (Phase 2 中重复计算) | -40 B | 低 |
| R4 | 移除 `RDB_KV_VERSION_OLD` 支持 | -60 B | 中 |
| | **ROM 合计** | **-210 B** | |

### 6.2 RAM 优化

| # | 方向 | 节省 | 难度 |
|---|------|------|------|
| M1 | 默认 `RDB_KV_CACHE_SIZE=0` (用户 opt-in) | **-1023 B** | 低 |
| M2 | 去重表从 32→16 槽 (N≤16 扇区足够) | **-256 B** | 低 |
| M3 | 仅 ACTIVE 扇区维护 Bloom | **-480 B** (16扇) | 中 |
| M4 | `rdb_kv_cache_slot_t` 压缩: prefix 8→4B (用 hash 低 32bit) | **-256 B** (64槽) | 中 |
| | **RAM 合计** | **-2,015 B** (从 2,652→637 B) | |

### 6.3 执行效率优化

| # | 方向 | 效果 | 难度 |
|---|------|------|------|
| E1 | GC Phase 1 零存活快速路径: 跳过 cache 构建 | -1 次 O(S) 扫描 | 低 |
| E2 | `find_latest` 使用 newest-first 扫描顺序 (利用 create_seq) | GET 提早退出 | 中 |
| E3 | Cache 预取: SET 后立即将 key 插入 cache (已实现) | — | ✅ |
| E4 | Bloom 查询批量化: 一条指令查多个扇区 | — | 低 |

---

## 7. 优先级路线图

### Phase A: 低风险快速见效 (ROM -150 B, RAM -1,279 B)

```
A1. 默认 RDB_KV_CACHE_SIZE=0 (RAM -1023 B)
A2. 去重表 32→16 槽 (RAM -256 B)
A3. R1+R2: 合并 GC 扫描 + cache ec_range (ROM -110 B)
A4. R4: 移除旧版 CRC 支持 (ROM -60 B)
```

### Phase B: 中风险架构改进

```
B1. 仅 ACTIVE 扇区 Bloom (RAM -480 B for 16 sector)
B2. LittleFS 风格双 ACTIVE 轮换 (减少 rotate 擦除)
B3. 小 value 内联存储 (≤8B value → 省 Flash 读)
```

### Phase C: 长期探索

```
C1. COW 元数据对 → 更简单的掉电恢复
C2. 全局 Bloom → RAM 不随扇区增长
C3. 动态磨损均衡增强
```

---

## 8. 与 FlashDB/LittleFS 的终极定位

| 维度 | 当前 RocketDB | 优化后 RocketDB | FlashDB 2.2.0 | LittleFS 2.11.3 |
|------|-------------|----------------|---------------|-----------------|
| KVDB ROM (ARM) | ~14 KB | ~13.5 KB | **4.6 KB** | N/A (~13-17 KB total) |
| Total ROM (ARM) | ~21 KB | ~20 KB | **6.4 KB** | ~13-17 KB |
| Total rw data | ~2,652 B | **~637 B** | **2 B** (!) | ~4 KB (可配) |
| KVDB 句柄 | 1,108 B | **87 B** | ~120 B | N/A |
| GET 热点 | **O(1)** | O(S×R) | O(S×R) | O(log N) |
| GET 冷门 | O(S×R×0.27) | O(S×R×0.27) | O(S×R) | O(log N) |
| GC/空间回收 | **4 段评分** | 4 段评分 | 简单阈值 | **COW 元数据对** |
| Init 扫描 | **1 遍** | 1 遍 | 3+ 遍 | 2 遍 |
| 掉电恢复 | 降级 ACTIVE | 降级 ACTIVE | 基础 CRC | **元数据对原子提交** |
| 磨损均衡 | spread=**2** | spread=**2** | 简单 | **动态 (block_cycles=500)** |
| TSDB 插入 (NOR) | ~250 条/s (估) | — | **250 条/s** | N/A |
| TSDB 插入 (片上) | — | — | **2,684 条/s** | N/A |

### 关键洞察

1. **FlashDB 的 rw data = 2 B** 是极致精简的标杆 — 所有状态存储在 Flash 上，RAM 仅用于全局标志位。RocketDB 通过关闭 cache+bloom 可达 87 B 句柄，但仍有 meta buffer 开销。
2. **LittleFS 的 COW 元数据对** 是掉电安全的最优雅方案 — 两个副本永远至少有一个有效。RocketDB 的降级 ACTIVE 恢复更复杂但保留了更多数据。
3. **RocketDB 的 4 段 GC + Bloom + CLOCK Cache** 在 FlashDB 和 LittleFS 中无对应实现，是差异化优势。
4. **RocketDB 优化后 RAM ~637 B** 介于 FlashDB (2 B) 和 LittleFS (~4 KB) 之间，提供的功能也介于两者之间。

**核心结论:** 通过将默认配置调整为 `CACHE=0` + 去重表减半 + 仅保留 ACTIVE Bloom，RocketDB 可在保持所有核心优势（4 段 GC、Bloom 加速、降级 ACTIVE 恢复）的同时，将默认 RAM 从 2,652 B 降至 ~637 B。与 FlashDB 的 2 B 仍有差距，但 RocketDB 提供的功能远超 FlashDB。
