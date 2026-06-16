# RocketDB 完整代码审查报告（含设计意图验证）

**日期:** 2026-06-16  
**审查范围:** `src/rocketdb.h` · `src/rocketdb_kvdb.c` · `src/rocketdb_tsdb.c` · `interface/` · `CMakeLists.txt` · `Makefile` · 测试框架  
**审查方法:** 4 代理并行扫描（Header / KVDB / TSDB / Cross-file）+ 交叉验证 + 架构回溯  
**测试状态:** 全量 53 用例 / 44,882 断言 / 0 失败  

---

## 前次审查遗留 — 全部已修复 ✅

| # | 问题 | 修复方式 | 验证 |
|---|------|---------|------|
| 1 | `_Static_assert` MSVC 兼容 | `RDB_STATIC_ASSERT` 宏，含 C99 typedef fallback | ✅ |
| 2 | `strkey_len()` uint8_t 死循环 | 改为 `size_t`，`RDB_MAX_KEY_LEN` 降至 32 | ✅ |
| 3 | dedup hash 碰撞误删数据 | 完整 key memcmp（Flash 回读） | ✅ |
| 4 | Cache 命中路径 buffer 溢出 | `uint8_t kb[RDB_MAX_KEY_LEN]`（32B） | ✅ |
| 5 | KVDB 大记录非对齐写入 | 对齐分块 + padding | ✅ |
| 6 | TSDB seal + wg≥2 不兼容 | init/format 拒绝 `write_gran > 1` | ✅ |
| 7 | 跨 epoch 查询提前停止 | `ITER_CONTINUE` 替代 `ITER_STOP` | ✅ |

---

## 新发现缺陷 — 逐项设计意图验证

### 🔴 严重

**#1 RDB_KV_CACHE_SIZE ODR 违规**

[CMakeLists.txt:293](CMakeLists.txt#L293) · [rocketdb.h:806](src/rocketdb.h#L806)

**发现:** `target_compile_definitions(test_kvdb_cache PRIVATE RDB_KV_CACHE_SIZE=64)` 仅在测试目标生效。

**设计意图回溯:**
- 架构 §2.1：`RDB_KV_CACHE_SIZE` 是编译时常量，"推荐嵌入式工作负载配置 64 槽"
- 架构 §4.2.1：缓存嵌入 `rdb_kvdb_t`，结构体在 SIZE=0 时含 1 字节 `disabled` 填充，SIZE>0 时含 slots 数组
- 设计假设：**所有编译单元使用相同的 `RDB_KV_CACHE_SIZE`**（标准宏行为）
- CMakeLists.txt 意图：`target_compile_definitions` 传递宏定义

**与设计意图对比:** 设计假设各编译单元宏一致。CMake PRIVATE 打破了这一假设 — 库以 SIZE=0 编译，测试以 SIZE=64 编译。`sizeof(rdb_kvdb_t)` 差异导致 `stats` 和 `blooms` 字段偏移量不匹配。

**验证结论: ✅ 确认为真缺陷。** 非设计取舍，而是构建配置错误。修复：将定义移至 `rocketdb` library 目标的 PUBLIC。

**修复方案:**
```cmake
target_compile_definitions(rocketdb PUBLIC
    RDB_KV_CACHE_SIZE=64
    RDB_BLOOM_BITS=256
)
```

---

### 🟡 高

**#2 GC 旋转回退 K-1 不变式弱化**

[rocketdb_kvdb.c:2023](src/rocketdb_kvdb.c#L2023)

**发现:** 旋转回退检查 `count_erased(db) >= 1u`，而非 `> gc_reserve`。

**设计意图回溯:**
- 架构 §1.2.1："空间可行性先行：写入前计算最小空间需求，结合 will_free 估算，确保 `gc_reserve + 1` 安全水位"
- 代码 K-1 fix（1822-1828行）：明确记录了快速路径将 `>= gc_reserve` 改为 `> gc_reserve` 的理由
- 快速路径已修复，但旋转回退未同步更新

**与设计意图对比:** 设计明确要求 `erased > gc_reserve`。旋转回退使用弱条件 `>= 1u` 在 `gc_reserve=2` 时可以降破不变式，导致后续写入提前 `ERR_FULL`。但这是**最后手段路径**，仅在 GC 循环无法找到 victim 时触发。不会造成数据损坏 — 仅提前返回可恢复的错误。

**验证结论: ✅ 确认为真缺陷。** 修复成本极低（改一行），应与快速路径保持一致的守卫条件。

**修复方案:**
```c
if (count_erased(db) > db->gc_reserve) {
```

---

**#3 TSDB 提交失败后 total_count 漂移**

[rocketdb_tsdb.c:643](src/rocketdb_tsdb.c#L643) · [rocketdb_tsdb.c:1312](src/rocketdb_tsdb.c#L1312)

**发现:** Phase B 提交失败 → `total_count` 未增加（正确） → rotation 时 `ts_scan(recover=TRUE)` 将 WRITING 记录恢复为 VALID 后计入 `lost` → `total_count -= lost` 减去从未计数的记录。

**设计意图回溯:**
- 架构 §1.2.1："状态机一致性：记录状态仅允许 1→0 位翻转，WRITING→VALID→DEAD"
- TSDB append 协议：Phase A 写 header+data（WRITING）→ Phase B 写 state=VALID
- 提交失败处理：推进 `head_off`，记录保持 WRITING（设计规则："推进写前沿或显式标记 DEAD"）
- 设计规则："扫描只读规则：查询/迭代路径只读不修复 WRITING 记录；仅 init 阶段允许修复"
- Rotation 内 `ts_scan(recover=TRUE)` 恢复 WRITING 记录是**有意设计** — 给 crash 中途的记录一次机会

**与设计意图对比:**
- 设计正确：撤销提交失败的记录，推进 head_off，不计数
- 设计正确：rotation 时恢复 WRITING 记录（给 crash 中途的数据最后一次可见机会）
- **但两者组合产生漂移**：恢复的记录被计入 `lost`，但它从未被计入 `total_count`

**触发条件极窄:** 需同时满足：(1) commit 写失败（罕见），(2) 系统持续运行无重启（重启后 init 全量重算 total_count），(3) 该扇区成为 tail 被旋转覆盖

**验证结论: ✅ 确认为设计边界不一致。** 修复建议：rotation 恢复的 WRITING 记录不计入 `lost`，或在 total_count 维护中跟踪"从未提交的恢复记录"。

**修复方案:**
```c
// 方案 A: 在 ts_scan(recover=TRUE) 中，对恢复的记录不计数
// 方案 B: 提交失败时记录 db->pending_abandoned++，rotation 时 lost -= pending_abandoned
```

---

**#4 Bloom filter 硬编码 0x1F 掩码**

[rocketdb.h:208](src/rocketdb.h#L208-L214)

**发现:** `RDB_BLOOM_SET`/`RDB_BLOOM_MAYBE` 宏硬编码 `& 0x1Fu`。

**设计意图回溯:**
- 头文件注释："W25QXX-class workloads: 256"
- 默认值 0（禁用），唯一文档化的启用值是 256
- Kconfig 中仅暴露 0 和 256 两个选项
- 256 位是固定设计参数，基于 W25QXX 扇区大小和典型 key 数量（80 条/扇区，~27% 假阳性率）

**与设计意图对比:** 设计仅支持 256 位。代码与设计一致但缺少编译期守卫。若用户误设 `RDB_BLOOM_BITS=128`，`0x1F` 掩码指向 bitmap[0..31]，而实际只有 16 字节 — 越界写。

**验证结论: ✅ 确认为真缺陷（缺少编译守卫）。** 设计假设 256 位是唯一启用的配置，但未通过静态断言强制执行。

**修复方案:**
```c
RDB_STATIC_ASSERT(RDB_BLOOM_BITS == 0 || RDB_BLOOM_BITS == 256,
    "RDB_BLOOM_BITS must be 0 (disabled) or 256");
```

---

### 🟡 中

**#5 CMake 最低版本过低**

[CMakeLists.txt:1](CMakeLists.txt#L1)

**设计意图回溯:** `$<C_COMPILER_ID:Clang>` 生成器表达式在 CMake 3.15 引入。项目使用 Clang，CI 使用较新 CMake。3.10 是 2017 年的版本，多数开发环境已升级。

**验证结论: ✅ 确认为构建配置缺陷。** 实际失败仅发生在 CMake 3.10-3.14 环境中（罕见但可能）。

**修复方案:** 改为 `cmake_minimum_required(VERSION 3.15)`。

---

**#6 REGISTER_TEST 不兼容 MSVC**

[tests/sim/test_framework.h:246](tests/sim/test_framework.h#L246)

**设计意图回溯:**
- 项目目标编译器是 Clang（CI、README、CMakePresets 均指定 Clang）
- `__attribute__((constructor))` 是 GCC/Clang 扩展
- MSVC 非目标编译器 — 项目不支持 MSVC 构建

**与设计意图对比:** 设计不打算支持 MSVC。但存在两个问题：(1) CMakeLists.txt 未拒绝 MSVC 编译器，(2) 若有人错误使用 MSVC 构建，假阴性测试比构建失败更危险。

**验证结论: ⚠️ 部分确认。** 不是需要修复的缺陷（MSVC 非目标），但应添加编译器强制检查：若检测到 MSVC，CMake 应明确报错并提示使用 Clang。

**修复方案:**
```cmake
if(MSVC)
    message(FATAL_ERROR "RocketDB requires Clang or GCC. MSVC is not supported.")
endif()
```

---

**#7 RDB_MAX_SECTORS 静默截断**

[rocketdb.h:798](src/rocketdb.h#L798)

**设计意图回溯:**
- 头文件注释（330-331行）："Limited to 255 because sector indices are stored as uint8_t"
- 设计明确知晓此限制，有文档记录，但缺少编译期守卫

**验证结论: ✅ 确认为缺失编译守卫。** 设计意图正确（uint8_t 限制），但未预防用户覆盖配置。

**修复方案:**
```c
RDB_STATIC_ASSERT(RDB_MAX_SECTORS <= 255,
    "RDB_MAX_SECTORS must be <= 255 (fits uint8_t)");
```

---

### 🟢 低

**#8 TSDB seal 中 head_off/head_count 的 uint16_t 截断**

[rocketdb_tsdb.c:627](src/rocketdb_tsdb.c#L627) · [rocketdb.h:693](src/rocketdb.h#L693)

**设计意图回溯:**
- 架构 §1.2.2 审计规则："位宽与容量：所有扇区内偏移使用 uint32_t，RAM 中 TSDB head_seq 使用 uint32_t；On-Flash 字段宽度不变时必须在扫描/写入边界做范围校验"
- On-flash `end_off` 为 uint16_t 是**刻意设计** — 减少扇区头元数据开销（20B）
- 异常扇区 >64KB 在嵌入式 NOR Flash 中不存在：W25Q 系列扇区统一 4KB，S25FL 最大 256KB 但极少使用

**验证结论: ⚠️ 设计取舍，非缺陷。** 设计明确权衡：用 uint16_t 换扇区头尺寸。但未在 init 中验证 `sector_size <= 65535`。添加运行时校验可防止未来非标准硬件上的静默数据丢失。

**修复方案:** init 时添加 `if (db->sector_size > 65535) return RDB_ERR_PARAM;`

---

**#9 HAL 契约与 1 字节写入冲突**

[interface/rocketdb_interface.h:77](interface/rocketdb_interface.h#L77)

**设计意图回溯:**
- sim_flash 注释（64-65行）："Single-byte writes (commit byte, state transitions) are always permitted — real NOR flash supports byte-program within a word."
- NOR Flash 物理特性：支持单字节编程
- 引擎 1 字节写入仅用于状态转换（commit、mark_dead）— 这是 NOR 闪存标准能力

**验证结论: ⚠️ 文档澄清需求，非功能缺陷。** 接口文档应补充说明：write_gran 对齐仅适用于数据负载；状态转换的单字节写入始终允许（NOR 物理特性）。

**修复方案:** 接口文档补充"单字节状态提交不受 write_gran 约束，依赖 NOR Flash 字节编程能力"。

---

**#10 缺少 2 的幂静态断言**

[rocketdb.h:150](src/rocketdb.h#L150)

**设计意图回溯:**
- 三个配置均有文档注释"Must be a power of 2 when > 0"
- `RDB_MIN_SECTOR_SIZE` 默认 4096，`RDB_FLASH_PAGE_SIZE` 默认 256，均为 2 的幂
- 仅 `RDB_MAX_KEY_LEN` 和 `RDB_STACK_BUF_SIZE` 有静态断言

**验证结论: ✅ 确认为代码质量改进。** 设计正确但缺少编译期守卫。用户误设非 2 的幂值会导致对齐计算公式失效。

**修复方案:**
```c
RDB_STATIC_ASSERT((RDB_MIN_SECTOR_SIZE & (RDB_MIN_SECTOR_SIZE - 1)) == 0,
    "RDB_MIN_SECTOR_SIZE must be a power of 2");
RDB_STATIC_ASSERT((RDB_FLASH_PAGE_SIZE & (RDB_FLASH_PAGE_SIZE - 1)) == 0,
    "RDB_FLASH_PAGE_SIZE must be a power of 2");
```

---

## 验证总结

| # | 严重度 | 类型 | 结论 |
|---|--------|------|------|
| 1 | 🔴 严重 | 构建配置缺陷 | ✅ 确认真缺陷 — 需 CMake PUBLIC 传播 |
| 2 | 🟡 高 | 代码遗漏 | ✅ 确认真缺陷 — 旋转回退未同步 K-1 修复 |
| 3 | 🟡 高 | 设计边界 | ✅ 确认设计不一致 — 极窄触发窗口 |
| 4 | 🟡 高 | 缺少守卫 | ✅ 确认缺失编译断言 |
| 5 | 🟡 中 | 构建配置 | ✅ 确认真缺陷 — CMake 版本 |
| 6 | 🟡 中 | 非目标平台 | ⚠️ 应为编译器拒绝，非修复 |
| 7 | 🟡 中 | 缺少守卫 | ✅ 确认缺失编译断言 |
| 8 | 🟢 低 | 设计取舍 | ⚠️ 嵌入式 NOR 合理取舍，加运行时校验 |
| 9 | 🟢 低 | 文档 | ⚠️ 文档澄清需求 |
| 10 | 🟢 低 | 代码质量 | ✅ 确认代码质量改进 |

**审查可信度:** 
- 8 项确认为真缺陷或缺失守卫（#1-#5, #7, #10）
- 2 项为文档/编译器加固需求（#6, #9）
- 1 项为设计取舍，嵌入式场景合理（#8）
- **0 项为虚假命中** — 所有发现均有设计依据

---

## 测试基线（修复前）

| 套件 | 断言 | 用例 | 耗时 |
|------|------|------|------|
| test_kvdb_basic | 3,005 | 11 | 7ms |
| test_kvdb_stress | 4,456 | 6 | 16ms |
| test_kvdb_cache | 6,160 | 9 | 9ms |
| test_tsdb_basic | 2,876 | 6 | 3ms |
| test_tsdb_stress | 2,749 | 5 | 10ms |
| test_integration | 25,535 | 6 | 74ms |
| test_fault_injection | 74 | 8 | 395ms |
| test_example | 27 | 2 | 0ms |
| **合计** | **44,882** | **53** | **514ms** |
