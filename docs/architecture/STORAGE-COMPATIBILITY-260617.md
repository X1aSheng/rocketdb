# RocketDB 存储器兼容性分析

**日期:** 2026-06-17  
**版本:** v1.6.0

---

## 1. RocketDB 的存储假设

RocketDB 通过 `rdb_flash_ops_t` 抽象层访问存储器，核心假设：

| # | 假设 | 实现依赖 |
|---|------|----------|
| A1 | **扇区擦除** — 擦除后扇区全为 0xFF | `fl_erase()` 回调 |
| A2 | **1→0 NOR 编程** — 单字节可从 0xFF 编程为任意值, 无需预擦除 | `fl_write()` 回调 |
| A3 | **顺序写入** — 扇区内记录追加写入, 不原地修改 | append-only 日志结构 |
| A4 | **单字节状态过渡** — 0xFF→0xFE→0xFC (WRITING→VALID→DEAD) | NOR-safe single-byte program |
| A5 | **写粒度对齐** — 支持 1/2/4/8 字节编程对齐 | `write_gran` 参数 |
| A6 | **CRC 校验** — 软件 CRC16 校验, 不依赖硬件 ECC | `rdb_crc16()` |
| A7 | **32-bit 寻址** — `uint32_t` Flash 地址 | `rdb_partition_t.base_addr` |
| A8 | **无坏块管理** — 假设所有扇区出厂完好 | GC 和 init 流程 |
| A9 | **扇区大小幂等** — 擦除/写入不影响其他扇区 | 独立扇区操作 |
| A10 | **对称读写** — 读粒度 ≤ 写粒度, 可随机读取任意地址 | `fl_read()` 回调 |

---

## 2. 各存储器兼容性评估

### 2.1 NOR Flash (当前支持)

| 类型 | 兼容度 | 说明 |
|------|--------|------|
| W25QXX 系列 (4KB/32KB/64KB sector) | **100%** | 原生支持, 已验证 |
| GD25QXX / PY25QXX / MP25PXX | **100%** | 协议兼容 W25QXX |
| 片上 Flash (STM32 内部) | **100%** | sector_size 按页大小配置 |
| SST26VF 系列 | **100%** | 需配置 write_gran=0 |
| **大容量 NOR (>16MB)** | **95%** | 需 32-bit 地址模式; 当前 total_size=uint32_t 上限 4GB |

**大容量 NOR 注意事项:**
- W25Q256 以上需 4-byte 地址模式 — RocketDB 使用 `uint32_t addr` 已支持
- 部分器件有 2×256KB + N×4KB 混合扇区 — RocketDB 要求 `sector_size` 统一, 需以最小扇区为 sector_size 或将不同扇区类型分到不同 partition
- 超 4GB 需 `uint64_t addr` — 需要 API 变更 (breaking change)

---

### 2.2 EEPROM (如 AT24C512, 24LC1025)

| 假设 | 满足? | 说明 |
|------|--------|------|
| A1 扇区擦除为 0xFF | ❌ | EEPROM 无需擦除, 可直接覆写任意字节 |
| A2 1→0 NOR 编程 | ⚠️ | EEPROM 可写 1 也可写 0, 比 NOR 更强 |
| A3 顺序追加写入 | ✅ | 可实现 |
| A4 单字节状态过渡 | ✅ | EEPROM 支持字节写入 |
| A8 无坏块管理 | ✅ | EEPROM 出厂完好 |

**兼容度: 40%** — 需要适配层

**关键问题:** EEPROM 不需要 `erase()` 操作。RocketDB 的 GC 和 format 流程大量依赖擦除:
- `rdb_kvdb_format()` 擦除所有扇区
- `gc_execute()` 擦除 victim 扇区
- `init_sector()` 擦除扇区后写头
- `is_erased()` 三点探测验证擦除

**适配方案:**
```c
// EEPROM erase → 将扇区写为 0xFF (模拟擦除)
static int eeprom_erase(void *ctx, uint32_t addr) {
    // 将 sector_size 字节写入 0xFF
    // 缺点: 擦除一个扇区需要 N 次 I2C 写入, 比 NOR 慢 100×
}
```

**结论:** 理论可行但**不推荐**。EEPROM 无需擦除的特性与 RocketDB 的 erase-before-write 模型矛盾。对于 EEPROM 更适合直接用字节级键值存储方案。

---

### 2.3 NAND Flash (SLC/MLC, 如 W29N01GV)

| 假设 | 满足? | 说明 |
|------|--------|------|
| A1 扇区擦除为 0xFF | ⚠️ | NAND 擦除后为 0xFF, 但可能有坏块 |
| A2 1→0 NOR 编程 | ❌ | **NAND 页只能编程一次** (per erase cycle), 不支持 NOR 的多次部分编程 |
| A3 顺序追加写入 | ❌ | NAND 要求页内顺序写入, 且页只能写一次 |
| A4 单字节状态过渡 | ❌ | NAND 最小写单元是页 (2KB+), 不是字节 |
| A5 写粒度对齐 | ❌ | NAND 写粒度是页 (2KB-16KB), 远超 8B |
| A6 CRC 校验 | ✅ | 可实现 |
| A7 32-bit 寻址 | ✅ | 可实现 |
| A8 无坏块管理 | ❌ | NAND 出厂有坏块, 使用时新增坏块 |

**兼容度: 10%** — 需要重大架构变更

**关键问题:**

1. **页编程限制:** RocketDB 的 `kv_mark_dead()` 通过单字节写入将 VALID (0xFE) 变为 DEAD (0xFC)。NAND 不支持页内部分重写。

2. **append-only 模式冲突:** KVDB set() 在扇区内追加头+key+value, 然后 commit 字节。NAND 每页只能编程一次, 无法追加。

3. **坏块管理:** RocketDB 假设所有扇区出厂完好。NAND 需要:
   - 出厂坏块表
   - 运行时坏块检测 (ECC 失败 → 标记坏块)
   - 坏块跳过 + 备用块替换

4. **ECC 需求:** NAND 需要硬件或软件 ECC (BCH/LDPC) 纠正 bit 错误。RocketDB 的 CRC16 只能检测不能纠正。

**适配方案 (工作量极大):**
- 将 NAND 的一个 block (64-128 页) 映射为 RocketDB 的一个 sector
- 页内追加 → 每次写满一整页 (填充 0xFF)
- `kv_mark_dead()` → 不在原位置修改, 而是写入新页后标记旧页无效
- 增加坏块管理层 + ECC 层

**结论: 不兼容。** NAND 的页编程模型与 RocketDB 的 NOR 字节编程模型根本不同。建议在 NAND 上使用为 NAND 设计的文件系统 (如 YAFFS2, UBIFS) 或使用 FTL (Flash Translation Layer) 模拟 NOR 行为。

---

### 2.4 FRAM / MRAM (铁电/磁性 RAM)

| 假设 | 满足? | 说明 |
|------|--------|------|
| A1 扇区擦除为 0xFF | ⚠️ | FRAM 无需擦除, 可直接覆写 |
| A2 1→0 NOR 编程 | ✅ | FRAM 支持任意字节写入 |
| A3 顺序追加写入 | ✅ | 可实现 |
| A4 单字节状态过渡 | ✅ | 字节级写入 |
| A8 无坏块管理 | ✅ | FRAM 无限寿命 |

**兼容度: 85%** — 需要适配层

**优势:**
- FRAM 无需擦除, `erase()` 可以是空操作或快速写 0xFF
- 字节写入延迟 ~100ns (vs NOR ~10µs+), **100× 更快**
- 写入寿命 **10^13-10^15** 次 (vs NOR 10^5), 实际上无需 GC

**适配方案:**
```c
// FRAM erase → 空操作 (FRAM 可直接覆写)
static int fram_erase(void *ctx, uint32_t addr) {
    (void)ctx; (void)addr;
    return 0;  // no-op, FRAM doesn't need erase
}

// 或: 将扇区写为 0xFF 以保持语义一致
static int fram_erase(void *ctx, uint32_t addr) {
    uint8_t ff[64];
    memset(ff, 0xFF, sizeof(ff));
    for (int i = 0; i < SECTOR_SIZE; i += 64)
        fram_write(ctx, addr + i, ff, 64);
    return 0;
}
```

**额外优化可能 (FRAM 特有):**
- 关闭 GC: 设置 `sector_cnt` 极大, 永不触发 GC
- 原地更新: 修改 `rdb_kvdb_set` 支持 in-place value 更新 (不需要 append)
- 关闭 Bloom: FRAM 读取延迟接近 SRAM, 全表扫描开销可忽略

**结论: 良好兼容。** FRAM 是 RocketDB 的理想存储器 — 字节写入 + 无限寿命 + 无需擦除消除了所有 NOR 的痛点。仅需简单的 `erase()` no-op 适配层。

---

### 2.5 DataFlash (Atmel/Microchip AT45DB 系列)

| 假设 | 满足? | 说明 |
|------|--------|------|
| A1 扇区擦除为 0xFF | ⚠️ | DataFlash 使用 page erase (256B/264B) 或 block erase |
| A2 1→0 NOR 编程 | ✅ | 支持字节编程, 使用 SRAM buffer |
| A3 顺序追加写入 | ✅ | 可实现 |
| A4 单字节状态过渡 | ✅ | 支持 |

**兼容度: 80%** — 需要适配层

DataFlash 与 NOR Flash 最接近, 主要差异:
- 擦除单位是 page (256B) 或 block (4KB), 不是 sector
- 写入通过内部 SRAM buffer (buffer 1/2 → program to page)
- 可以直接将 page_size 配置为 sector_size

**适配方案:**
```c
// DataFlash: page erase as sector erase
static int df_erase(void *ctx, uint32_t addr) {
    return df_page_erase(ctx, addr);  // 256B page erase
}
```
配置 `sector_size = 256` 或 `512` 即可。

**结论: 良好兼容。** DataFlash 的 SRAM buffer 编程可封装为 `fl_read`/`fl_write`/`fl_erase`, 几乎无需修改 RocketDB 核心。

---

### 2.6 SD/eMMC (块设备)

| 假设 | 满足? | 说明 |
|------|--------|------|
| A1 扇区擦除为 0xFF | ❌ | SD 无擦除概念, 覆写直接进行 (FTL 处理) |
| A2 1→0 NOR 编程 | ✅ | 支持 512B 扇区写入 |
| A3 顺序追加写入 | ✅ | 可实现 |
| A4 单字节状态过渡 | ❌ | SD 最小写单元是 512B block |
| A8 无坏块管理 | ✅ | FTL 内部处理 |

**兼容度: 30%** — 需要重大适配

**关键问题:**
1. **单字节写入 → 读-改-写:** `kv_mark_dead()` 写 1 字节 → 需要读出整块 512B, 修改 1 字节, 写回整块
2. **无擦除语义:** SD 卡内部 FTL 处理擦除, 写 0xFF 可能不会物理擦除
3. **写放大:** RocketDB 的小 record + commit byte 模式在 512B 块设备上产生严重写放大
4. **掉电风险:** SD 卡内部 FTL 可能在掉电时丢失未刷新数据

**适配方案 (不推荐):**
- 使用 read-modify-write 模拟单字节写入
- 但性能和可靠性损失严重

**结论: 不兼容。** RocketDB 的字节级 NOR 优化模式与块设备根本冲突。对于 SD/eMMC, 使用 LittleFS 或 FAT 文件系统更合适。

---

### 2.7 虚拟 Flash (RAM Disk / 文件模拟)

| 假设 | 满足? | 说明 |
|------|--------|------|
| 全部 10 项假设 | ✅ | 可在 RAM 中完全模拟 NOR 行为 |

**兼容度: 100%** — 当前测试框架已支持

RocketDB 的仿真测试框架 (`tests/sim/sim_flash.c`) 使用 RAM 模拟 NOR Flash:
- `sim_flash_write()`: 写入前检查 1→0 过渡, 模拟写粒度
- `sim_flash_erase()`: 将扇区填充为 0xFF
- `sim_flash_read()`: 直接 memcpy

---

## 3. 兼容性总结

| 存储器 | 兼容度 | 适配工作量 | 推荐? | 说明 |
|--------|--------|-----------|-------|------|
| **NOR Flash (W25QXX, GD25QXX)** | **100%** | 无 | ✅ 推荐 | 原生目标平台 |
| **大容量 NOR (>16MB)** | **95%** | 低 | ✅ 推荐 | 注意 4-byte 地址模式和混合扇区 |
| **DataFlash (AT45DB)** | **80%** | 低 | ✅ 推荐 | page/block 映射为 sector |
| **FRAM / MRAM** | **85%** | 低 | ✅ 推荐 | erase() = no-op, GC 可关闭 |
| **EEPROM (I2C/SPI)** | **40%** | 中 | ⚠️ 不推荐 | 擦除模拟开销大, 不如用专用方案 |
| **SD/eMMC** | **30%** | 高 | ❌ 不推荐 | 512B 块设备 + 字节编程矛盾 |
| **NAND Flash (SLC/MLC)** | **10%** | 极高 | ❌ 不兼容 | 页编程 + 坏块 + ECC 需全新架构 |

### 兼容度评分逻辑

```
100%  — 所有 10 项假设均满足, 零适配
80-95% — 1-2 项假设需轻量适配 (<100 行代码)
40-60% — 多项假设需中等适配, 性能可能下降
10-30% — 核心假设冲突, 需架构变更
```

---

## 4. 适配指南

### 4.1 新存储器适配清单

| 步骤 | 内容 | 关键点 |
|------|------|--------|
| 1 | 实现 `rdb_flash_ops_t` | read/write/erase 回调 |
| 2 | 配置 `rdb_partition_t` | sector_size = 擦除单元大小 |
| 3 | 设置 `write_gran` | 匹配硬件最小编程单元 |
| 4 | 处理 erase 语义 | 无擦除存储器 → 写 0xFF 或 no-op |
| 5 | 处理坏块 | 在 read/write/erase 中检测并返回错误 |
| 6 | 配置缓存/Bloom | 根据读写延迟调整 |

### 4.2 FRAM 最佳配置

```c
// FRAM 优化配置
#define RDB_KV_CACHE_SIZE   0     // 无需缓存 (FRAM 读取 ≈ RAM 速度)
#define RDB_BLOOM_BITS      0     // 无需 Bloom (全表扫描极快)
#define RDB_DEDUP_SLOTS     8     // 最小去重表
#define RDB_STACK_BUF_SIZE  256   // 增大合并缓冲 (无 GC 压力)
#define RDB_GC_GARBAGE_PCT  90    // 极高阈值 (几乎不触发 GC)
```

### 4.3 大容量 NOR 最佳配置

```c
// 大容量 NOR (>128MB)
// sector_size 用最小扇区大小
// 混合扇区: 将不同扇区类型分配到不同 partition
// W25Q256 示例:
rdb_partition_t p1 = { .base_addr=0,      .total_size=128*4096, .sector_size=4096 };
rdb_partition_t p2 = { .base_addr=128*4096, .total_size=64*65536, .sector_size=65536 };
// p1: 底部 128 个 4KB 扇区 → KVDB
// p2: 剩余 64KB 扇区 → TSDB
```
