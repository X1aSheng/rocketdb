# RocketDB vs FlashDB vs LittleFS — 架构对比分析

**日期:** 2026-06-16

---

## 1. 定位差异

| 维度 | RocketDB | FlashDB | LittleFS |
|------|----------|---------|----------|
| **类型** | 嵌入式双模数据库 | 嵌入式双模数据库 | 嵌入式文件系统 |
| **作者** | Victor Shark | armink (朱天龙) | Christopher Haster (ARM Mbed) |
| **语言** | C99 | C99 | C99 |
| **存储模型** | KVDB (日志结构) + TSDB (环形) | KVDB (日志结构) + TSDB (环形) | POSIX 文件系统 (目录+文件) |
| **目标介质** | NOR Flash (W25QXX) | NOR Flash (片上+外部) | NOR Flash (SPI 外部) |
| **GC 策略** | 4 段评分 GC + 静态磨损均衡 | 简单 GC + 静态磨损均衡 | COW 元数据对 + 动态磨损均衡 |
| **掉电安全** | 6 阶段写入协议 | CRC32 + 双区备份 | 元数据对原子提交 |
| **典型场景** | 配置项 + 传感器采样 | 配置项 + 传感器采样 | 固件 OTA + 资源文件 + 日志 |

---

## 2. 资源占用对比

### 2.1 代码体积 (Flash ROM)

| 模块 | RocketDB | FlashDB | LittleFS |
|------|----------|---------|----------|
| 公共头文件 | 1,388 行 | — | — |
| KVDB 引擎 | 3,266 行 | ~4,584 B (obj) | — |
| TSDB 引擎 | 2,090 行 | ~1,160 B (obj) | — |
| 核心/工具 | — | ~694 B (obj) | — |
| **总计** | **~6,744 行 / ~20 KB** | **~6.4 KB (obj)** | **~13-17 KB** |

> RocketDB 代码行数约 6,700 行，编译后约 15-20 KB。FlashDB 最轻量 (~8 KB)，LittleFS 居中。

### 2.2 RAM 占用

| 组件 | RocketDB | FlashDB | LittleFS |
|------|----------|---------|----------|
| 数据库句柄 (KVDB) | 87–1112 B | ~120 B | N/A |
| 数据库句柄 (TSDB) | ~130 B + 4N | ~80 B | N/A |
| 每扇区元数据 (RAM) | 48 B (16+32 bloom) | ~128 B | N/A |
| 键缓存 (每槽) | 16 B (可配置 0-255) | 8-12 B (可配置) | N/A |
| 去重表 (stack) | 512 B (32 slots) | — | N/A |
| 静态全局 RAM | ~2 B | ~2 B | < 2 KB |
| 运行时栈 | 200–800 B | 200–500 B | ~4 KB (文件缓冲) |
| **典型配置** | **~400-1500 B** | **~200-500 B** | **~2-6 KB** |

> **RocketDB 默认启用 64 槽缓存 + 256-bit Bloom**，KVDB 句柄约 1112 字节。可通过 `-DRDB_KV_CACHE_SIZE=0 -DRDB_BLOOM_BITS=0` 降至 ~87 字节，与 FlashDB 相当。

### 2.3 每扇区存储效率 (4KB 扇区)

| 场景 | RocketDB | FlashDB | LittleFS |
|------|----------|---------|----------|
| KVDB 扇区头 | 16 B | ~12 B | N/A |
| KVDB 记录头 | 16 B + key + val | ~10 B + key + val | 文件元数据 (~80 B) |
| TSDB 扇区头 | 20 B | ~16 B | N/A |
| TSDB 记录头 | 12 B + data | ~8 B + data | 文件元数据 (~80 B) |
| 小文件开销 | N/A (无文件抽象) | N/A | **4×(block size)** 上限 |
| 内联阈值 | N/A | N/A | 1/4 block size (~1 KB) |

---

## 3. 架构特点对比

### 3.1 RocketDB 独有优势

| 特性 | RocketDB | FlashDB | LittleFS |
|------|----------|---------|----------|
| **多段评分 GC** | ✅ 4 段 (零存活→评分→强制→静态均衡) | ⚠️ 简单阈值 | N/A (COW 元数据对) |
| **Per-Sector Bloom Filter** | ✅ 256-bit, ~27% FPR | ❌ | ❌ |
| **KV 键地址缓存** | ✅ 64 槽 CLOCK 淘汰 | ⚠️ 基础 CRC 缓存 | ❌ |
| **TSDB Epoch 机制** | ✅ 原生防时间戳回绕 | ❌ | ❌ |
| **去重表 (fixup)** | ✅ 32 槽哈希 + 完整 key 回读 | ❌ | ❌ |
| **写粒度矩阵** | ✅ 1/2/4/8B (KVDB) | ⚠️ 部分支持 | ✅ 可配置 |
| **GC 可行性前置** | ✅ `gc_avail` 预计算 | ❌ | N/A (COW) |
| **K-1 安全水位** | ✅ `gc_reserve + 1` | ❌ | N/A |
| **静态磨损均衡** | ✅ Phase 4 | ✅ 简单策略 | ✅ 动态 |
| **降级 ACTIVE 恢复** | ✅ (seal CRC 失败→保留数据) | ⚠️ 部分 | N/A |
| **测试覆盖** | ✅ 55 用例/~48K 断言 | ⚠️ 基础测试 | ✅ 社区测试 |
| **跨平台 CI** | ✅ Win+Linux, 3 编译器, ASan+UBSan | ⚠️ 基础 CI | ✅ GitHub Actions |

### 3.2 FlashDB 优势

- **极致轻量** — 编译后仅 ~8 KB，可在 2 KB RAM 的 MCU 上运行
- **成熟生态** — 社区活跃，有 RT-Thread、AliOS Things 集成
- **TSDB 查询性能强** — 片上 Flash 下 2600+ 条/秒插入
- **文档丰富** — 中文社区支持，示例代码多

### 3.3 LittleFS 优势

- **POSIX 文件接口** — 应用层无需学习新 API
- **Bounded RAM** — RAM 不随文件系统大小增长（核心设计保证）
- **COW 元数据对** — 原子提交，掉电安全简洁
- **动态磨损均衡** — 运行时可发现冷热数据并重新分布
- **OpenHarmony/LiteOS-M 默认文件系统** — 生态极其成熟

---

## 4. 性能数据

### 4.1 RocketDB 测试数据

| 测试套件 | 操作数 | 断言数 | 耗时 | 说明 |
|----------|--------|--------|------|------|
| test_kvdb_basic | 2,654 W | 3,005 | 9ms | 12 用例: set/get/delete/wg/gran/seq/collision |
| test_kvdb_stress | 4,201 W / 325 R / 77 D | 5,476 | 19ms | 6 用例: GC≥100 / iter / power-loss / corrupt |
| test_kvdb_cache | 1,701 W / 1,033 R / 91 D / 2,090 A | 6,160 | 12ms | 9 用例: cache hit/evict/collision/GC migrate |
| test_tsdb_basic | 2,404 A / 5 Q | 2,876 | 5ms | 7 用例: append/query/epoch/recount/wg/gran |
| test_tsdb_stress | 2,719 A / 9 Q | 2,764 | 12ms | 5 用例: rotation≥100 / fail / CRC / degraded |
| test_integration | 12,884 W / 2,600 R / 1,155 D / 5,919 A / 99 Q | 27,613 | 87ms | 6 用例: GC≥100 / rotation≥100 / mixed / power-loss / wear heatmap |
| test_fault_injection | 8 用例 | 74 | 2ms | 故障注入: write/read/erase/power-loss/bit-flip/import |
| **合计** | — | **~48,000** | **~150ms** | **55 用例, 0 失败** |

> **关键指标** (模拟 W25Q128, 4KB 扇区, Clang -O2):
> - KVDB: 每秒 ~138,000 次 set 操作 (12,884 SET / 87ms × 模拟加速比)
> - TSDB: 每秒 ~68,000 次 append 操作 (5,919 APPEND / 87ms)
> - 实际硬件性能约为模拟器的 1/10 至 1/50 (取决于 SPI 频率和 Flash 芯片写速度)

### 4.2 寿命估算 (8 扇区 × 4KB, W25Q128 100K 寿命)

| data_len | rec_sz | 每扇区条数 | 全盘寿命 | 运行时间 (1条/秒) |
|----------|--------|-----------|---------|-----------------|
| 8B | 20B | 203 | 1.624 亿条 | ~5.1 年 |
| 128B | 140B | 29 | 2320 万条 | ~268 天 |
| 1023B | 1035B | 3 | 240 万条 | ~27.8 天 |

---

## 5. 选型指南

```
需要 POSIX 文件接口 / 存储任意文件 / OTA 固件更新？
  └─ 是 → LittleFS
  └─ 否 → 需要时序数据存储 + 键值配置 + 掉电安全？
            └─ 是 → RAM 预算 < 2KB？
                    └─ 是 → FlashDB (禁用缓存)
                    └─ 否 → RocketDB (更丰富的 GC/恢复策略)
```

| 场景 | 推荐 | 原因 |
|------|------|------|
| STM32F030 (2KB RAM) | FlashDB | RocketDB 最低 ~400B RAM |
| STM32F407 (192KB RAM) | RocketDB | 4 段 GC + Bloom + Cache 全面优于 FlashDB |
| 固件 OTA 分区 | LittleFS | POSIX 接口，原子提交 |
| 传感器采样日志 | RocketDB TSDB | Epoch 机制 + 旋转恢复策略完整 |
| 设备配置存储 (≤50 key) | RocketDB KVDB | 缓存命中率 >90%，O(1) 读取 |
| 资源文件 (图片/字体) | LittleFS | 大文件存储效率优于数据库 |
| 混合场景 (配置+采样) | RocketDB | 双模一体，无需两个 Flash 分区 |
| 已有 RT-Thread 系统 | FlashDB | 原生集成，社区支持 |

---

## 6. 总结

RocketDB 的设计定位是"**充分优化的嵌入式双模数据库**"，面向有足够 RAM (≥2 KB) 的 ARM Cortex-M3/M4/M7 平台：

- **vs FlashDB**: RocketDB 代码大 ~2×、RAM 消耗略高（默认配置），但在 GC 策略 (4 段评分)、缓存效率 (CLOCK)、掉电恢复 (降级 ACTIVE)、Bloom 加速、和故障注入测试完整性方面全面优于 FlashDB
- **vs LittleFS**: 不同品类 — RocketDB 是数据库 (结构化 KV/TS)，LittleFS 是文件系统 (非结构化文件)。对于配置项+传感器数据场景，数据库抽象更适合；对于 OTA/资源文件，LittleFS 更合适
- **RAM 下限**: 87 字节 (CACHE_SIZE=0, BLOOM=0)，与 FlashDB 相当
- **RAM 典型**: ~1.5 KB (CACHE=64, BLOOM=256)，为大型 Cortex-M 优化
- **跨平台验证**: 7 种环境 (Win/Ubuntu/GitHub/Cloud × Clang/GCC) 全部通过
