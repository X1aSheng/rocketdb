# RocketDB 修复方案与影响分析

**日期:** 2026-06-16  
**基于:** [CODE-REVIEW-260616.md](../reports/CODE-REVIEW-260616.md) v2（含设计意图验证）  
**待修复:** 10 项发现（8 真缺陷 + 2 加固 + 1 运行时校验）

---

## 概述

| 分类 | 数量 | 影响范围 | 风险 |
|------|------|---------|------|
| 单行修复 | 6 | #2, #4, #5, #7, #8, #10 | 极低 |
| 编译配置 | 1 | #1 (CMakeLists.txt) | 低 — 需验证 cache 测试 |
| 行为变更 | 1 | #3 (ts_rotate 恢复策略) | 低 — 仅影响未提交数据 |
| 编译器策略 | 1 | #6 (拒绝 MSVC) | 极低 |
| 文档 | 1 | #9 (HAL 接口说明) | 无 |

---

## #1 RDB_KV_CACHE_SIZE ODR 违规

**文件:** `CMakeLists.txt`  
**严重度:** 🔴 严重  
**缺陷类型:** 构建配置 — 结构体布局不匹配

### 根因

```cmake
# Line 293: 定义仅在 test_kvdb_cache 目标
target_compile_definitions(test_kvdb_cache PRIVATE
    RDB_KV_CACHE_SIZE=64
    RDB_BLOOM_BITS=256
)
```

`PRIVATE` 不传播到链接的 `rocketdb` 库。库以 `RDB_KV_CACHE_SIZE=0`（默认）编译，测试以 `=64` 编译。`rdb_kvdb_t` 中 `cache` 字段大小不同（1B vs 1025B），后续 `stats`/`blooms` 字段偏移量全部错位。

### 修复

将定义移至 `rocketdb` library 目标，使用 `PUBLIC` 传播：

```cmake
# 在 rocketdb_configure_target(rocketdb) 之后添加（Line 202 之后）
target_compile_definitions(rocketdb PUBLIC
    RDB_KV_CACHE_SIZE=64
    RDB_BLOOM_BITS=256
)

# 删除 Line 292-296 的 test_kvdb_cache PRIVATE 定义块
```

### 影响分析

| 维度 | 修复前 | 修复后 |
|------|--------|--------|
| 库 sizeof(rdb_kvdb_t) | ~200B (SIZE=0) | ~1200B (SIZE=64) |
| 测试 sizeof(rdb_kvdb_t) | ~1200B (SIZE=64) | ~1200B (一致) |
| cache 代码路径 | `#if` 排除，桩函数 | 真实线性探测 + CLOCK 淘汰 |
| test_kvdb_cache 行为 | 测试桩（假阳性） | 测试真实缓存逻辑 |
| 存量测试影响 | 无（结构体已匹配） | 无破坏 — 所有测试一致使用 SIZE=64 |
| RAM 开销 | 库无额外 RAM | 库每例化 +1028B（64 槽 × 16B + 1B 时钟指针） |

**风险评估:** 低。修复后全部 53 用例应继续通过。test_kvdb_cache 日志中 `[CACHE]` 输出将从桩报告变为真实槽位占用统计，可据此验证。若需禁用缓存（嵌入式目标），通过 `-DRDB_KV_CACHE_SIZE=0` 重定义即可 — 这是标准 C 宏覆盖行为。

---

## #2 GC 旋转回退 K-1 不变式

**文件:** `src/rocketdb_kvdb.c` line 2023  
**严重度:** 🟡 高  
**缺陷类型:** 代码遗漏 — 快速路径已修复但回退路径未同步

### 根因

K-1 修复（lines 1822-1828）将两条快速路径的条件从 `>= gc_reserve` 改为 `> gc_reserve`，但最后手段旋转回退（line 2023）仍使用弱条件 `>= 1u`。

### 修复

```c
// Line 2023: 替换守卫条件
// 修复前:
if (count_erased(db) >= 1u) {
// 修复后:
if (count_erased(db) > db->gc_reserve) {
```

### 影响分析

| 场景 | gc_reserve | 修复前 | 修复后 |
|------|-----------|--------|--------|
| 正常：erased=5 | 2 | 旋转，erased→4 | 旋转，erased→4（一致） |
| 边界：erased=2 | 2 | 旋转，erased→1 **违反** | 跳过旋转，ERR_FULL |
| 边界：erased=1 | 1 | 旋转，erased→0 | 旋转，erased→0（一致） |
| 边缘：erased=0 | any | 跳过，ERR_FULL | 跳过，ERR_FULL（一致） |

**风险评估:** 极低。仅影响 gc_reserve=2 且 erased=2 的极端边界。差异：修复后在此边界提前返回 ERR_FULL 而非消耗最后一个已擦除扇区。ERR_FULL 是**可恢复的** — 调用者删除 key 释放空间后可重试。降破不变式则导致**不可恢复的死锁**。

---

## #3 TSDB 提交失败后 total_count 漂移

**文件:** `src/rocketdb_tsdb.c` line 643  
**严重度:** 🟡 高  
**缺陷类型:** 设计边界 — 旋转恢复与增量计数语义不一致

### 根因

```
时序：
  t1: Phase A 成功 → 记录 WRITING (state=0xFF)，CRC 正确
  t2: Phase B 失败 → head_off += rsz，total_count 未增加（设计正确）
  t3: 多次旋转后该扇区成为 tail
  t4: ts_rotate → ts_scan(recover=RDB_TRUE) → 恢复 WRITING→VALID → 计入 lost
  t5: total_count -= lost → 减去从未计数的记录 → 漂移
```

`ts_scan(recover=TRUE)` 无法区分两种 WRITING 记录：
- **断电中断** — 已写入数据但未提交（应恢复 → 数据曾"意图"提交）
- **提交失败** — 主动放弃的记录（不应恢复 → 调用者收到 ERR_FLASH，数据未被接受）

两种情况的 CRC 均正确，ts_scan 同等对待。

### 修复

旋转期间停止恢复 WRITING 记录：

```c
// Line 643: 旋转不应恢复 WRITING 记录
// 修复前:
lost = ts_scan(db, old_tail, h.time_base, max_off, NULL, NULL, RDB_TRUE);
// 修复后:
lost = ts_scan(db, old_tail, h.time_base, max_off, NULL, NULL, RDB_FALSE);
```

同时更新 lines 636-638 的注释，说明不走恢复路径的原因。

### 影响分析

| 场景 | 修复前 | 修复后 |
|------|--------|--------|
| 提交失败，持续运行 | WRITING 记录在 rotation 中被恢复 → 计入 lost → total_count 漂移 | WRITING 记录在 rotation 中被跳过 → 不计入 lost → total_count 正确 |
| 断电中断，重启 | init Phase 3 恢复 head 扇区 WRITING 记录 → total_count 正确 | 同左，不受影响（init 路径独立于 rotation） |
| 断电中断，非 head 扇区有 WRITING | rotation 恢复 → 计入 lost → 漂移（该记录 init 从未计入） | rotation 跳过 → 不计入 lost → total_count 正确 |
| 提交失败，调用者视角 | 被告知 ERR_FLASH，数据未被接受 | 同左，不变 |

**核心原则:** 旋转不应赋予未提交数据"最后一次可见机会"。未提交数据在语义上不属于数据库，不应影响 total_count。断电中断的数据恢复由 init 负责（仅 head 扇区），不受此变更影响。

**风险评估:** 低。旋转期间被跳过的 WRITING 记录在语义上从未属于数据库（提交从未完成）。`test_kvdb_cache` 的 `ts_safety_recover_faults` 用例覆盖了提交失败路径，可验证 total_count 准确性。

---

## #4-#10 轻量修复

### #4 Bloom filter 静态断言

**文件:** `src/rocketdb.h` line 205（`#define RDB_BLOOM_BYTES` 之后）

```c
RDB_STATIC_ASSERT(RDB_BLOOM_BITS == 0 || RDB_BLOOM_BITS == 256,
    "RDB_BLOOM_BITS must be 0 (disabled) or 256");
```

**影响:** 编译期检查，零运行时开销。误设 `RDB_BLOOM_BITS=128` 将在编译时报错而非越界破坏内存。

### #5 CMake 最低版本

**文件:** `CMakeLists.txt` line 1

```cmake
# 修复前:
cmake_minimum_required(VERSION 3.10)
# 修复后:
cmake_minimum_required(VERSION 3.15)
```

**影响:** CMake 3.10-3.14 用户配置阶段收到清晰的版本要求错误，而非晦涩的生成器表达式错误。CI 和开发者环境不受影响（均使用 ≥3.15）。

### #6 拒绝 MSVC 编译器

**文件:** `CMakeLists.txt` line 1 附近

```cmake
if(MSVC)
    message(FATAL_ERROR "RocketDB requires Clang or GCC. "
        "MSVC is not supported due to __attribute__((constructor)) and _Static_assert usage. "
        "Install Clang: https://llvm.org")
endif()
```

**影响:** MSVC 用户收到明确指引，而非静默的假阴性测试通过（无测试注册）。不影响 Clang/GCC 用户。

### #7 RDB_MAX_SECTORS 静态断言

**文件:** `src/rocketdb.h` line 338（现有断言之后）

```c
RDB_STATIC_ASSERT(RDB_MAX_SECTORS <= 255,
    "RDB_MAX_SECTORS must be <= 255 (sector indices are uint8_t)");
```

**影响:** 编译期检查。误设 `RDB_MAX_SECTORS=300` 将在编译时报错。

### #8 TSDB 扇区大小运行时校验

**文件:** `src/rocketdb_tsdb.c` line 748（现有 `sector_size <= ts_data_start` 检查之后）

```c
/* On-flash end_off is uint16_t (max 65535, 0xFFFF sentinel for unsealed).
   Reject sectors larger than 65535 to prevent silent data loss. */
if (db->sector_size > 65535u)
    return RDB_ERR_PARAM;
```

**影响:** 标准 W25QXX（4KB 扇区）不受影响。使用超大扇区（>64KB）的异类硬件在 init 时收到明确错误。这是 uint16_t on-flash 字段的设计取舍的运行时防护。

### #9 HAL 接口文档补充

**文件:** `interface/rocketdb_interface.h` line 77

```c
 * @note      Must respect NOR flash 1->0 bit-flip semantics.
 *            Caller guarantees addr and len are write-granularity aligned
 *            for data payloads.  Single-byte state-transition writes
 *            (commit byte, mark_dead) occur regardless of write_gran
 *            and rely on NOR flash byte-program capability.
```

**影响:** 纯文档，无代码变更。HAL 实现者从文档即可理解单字节写入是合法的 NOR 操作。

### #10 2 的幂静态断言

**文件:** `src/rocketdb.h` line 338（现有断言之后）

```c
RDB_STATIC_ASSERT((RDB_MIN_SECTOR_SIZE & (RDB_MIN_SECTOR_SIZE - 1u)) == 0,
    "RDB_MIN_SECTOR_SIZE must be a power of 2");
RDB_STATIC_ASSERT(RDB_FLASH_PAGE_SIZE == 0 ||
    (RDB_FLASH_PAGE_SIZE & (RDB_FLASH_PAGE_SIZE - 1u)) == 0,
    "RDB_FLASH_PAGE_SIZE must be a power of 2 when non-zero");
```

**影响:** 编译期检查。`RDB_ALIGN_UP` 宏依赖 2 的幂运算，非 2 的幂值产生垃圾对齐结果。

---

## 修复执行计划

### 阶段 1：编译守卫（立即 — 零风险）

| 顺序 | # | 文件 | 变更 |
|------|---|------|------|
| 1 | #5 | `CMakeLists.txt:1` | `VERSION 3.15` |
| 2 | #6 | `CMakeLists.txt` | MSVC 拒绝块 |
| 3 | #4 | `rocketdb.h:205` | Bloom 断言 |
| 4 | #7 | `rocketdb.h:338` | MAX_SECTORS 断言 |
| 5 | #10 | `rocketdb.h:338` | 2 的幂断言 × 3 |

- 全部为编译期检查，无运行时影响
- 可直接提交，无需测试

### 阶段 2：运行时校验（编译后验证）

| 顺序 | # | 文件 | 变更 |
|------|---|------|------|
| 6 | #8 | `rocketdb_tsdb.c:748` | 扇区大小校验 |

- 运行时检查，标准硬件不受影响
- 现有测试中扇区大小均为 4096 → 通过

### 阶段 3：行为修复（需测试验证）

| 顺序 | # | 文件 | 变更 |
|------|---|------|------|
| 7 | #2 | `rocketdb_kvdb.c:2023` | 旋转回退守卫 |
| 8 | #3 | `rocketdb_tsdb.c:643` | rotation 恢复策略 |

- 各 1 行代码变更
- 需运行全量测试验证：`test_kvdb_stress`（GC 场景）、`test_tsdb_stress`（旋转场景）、`test_fault_injection`（故障注入）
- **验收标准:** 53 用例、44,882 断言、0 失败

### 阶段 4：构建修复（需 cache 测试验证）

| 顺序 | # | 文件 | 变更 |
|------|---|------|------|
| 9 | #1 | `CMakeLists.txt:202,293` | 移动 RDB_KV_CACHE_SIZE 到 library PUBLIC |

- 最重要的修复 — 当前缓存测试未测试真实逻辑
- **验收标准:** `test_kvdb_cache` 全部 9 用例通过，日志中 `[CACHE]` 输出显示真实槽位占用变化（0% → 100% → 0%）

### 阶段 5：文档

| 顺序 | # | 文件 | 变更 |
|------|---|------|------|
| 10 | #9 | `interface/rocketdb_interface.h:77` | 注释补充 |

---

## 全面影响评估

### 对存量代码的影响

| 受影响的子系统 | 影响描述 | 兼容性 |
|---------------|---------|--------|
| KVDB set/get/delete | #1：缓存现在真正运行。热点 key 查找从 O(N) 降至 O(1)+1 次 Flash 读取 | ✅ 完全向后兼容 |
| KVDB GC | #2：极端边界提前返回 ERR_FULL 而非降破不变式 | ✅ 可恢复错误，行为更安全 |
| TSDB append/rotation | #3：未提交的 WRITING 记录不再在旋转中被恢复 | ✅ 语义正确，调用者已被告知失败 |
| 编译配置 | #4-#7, #10：新增编译期检查 | ✅ 仅对**错误配置**报错 |
| 运行时 | #8：>64KB 扇区现在被拒绝 | ✅ 标准硬件无影响 |
| HAL 接口 | #9：文档补充 | ✅ 无代码变更 |

### 对嵌入式部署的影响

| 关注点 | 评估 |
|--------|------|
| RAM 增加 | #1 使库的 `rdb_kvdb_t` 增加 ~1028B。嵌入式用户通过 `-DRDB_KV_CACHE_SIZE=0` 可完全移除（恢复当前行为） |
| Flash 占用 | 无变化 |
| CPU 开销 | #1 略微增加（缓存查找），但消除全表扫描，净减少。其他修复无影响 |
| 掉电安全 | #3 不影响掉电恢复路径（init 独立于 rotation） |
| API 兼容 | 无破坏性变更 |

### 测试覆盖

| 测试 | 覆盖的修复 |
|------|----------|
| `test_kvdb_cache` (9 用例) | #1 — 缓存逻辑从桩变为真实代码路径 |
| `test_kvdb_stress` (6 用例) | #2 — GC 压力场景覆盖旋转回退路径 |
| `test_tsdb_stress` (5 用例) | #3 — 旋转压力场景 |
| `test_fault_injection` (8 用例) | #3 — 故障注入覆盖提交失败路径 |
| `test_integration` (6 用例) | #2, #3 — 混用场景 |

---

## 修复后验证清单

- [ ] CMake 配置通过（`cmake -S . -B build`）
- [ ] 全量编译通过（53 可执行文件）
- [ ] 全量测试通过（53 用例 / 44,882+ 断言 / 0 失败）
- [ ] `test_kvdb_cache` 日志中 `[CACHE]` 显示真实槽位占用（0% → 100% → 0%），非桩输出
- [ ] 错误配置测试：`-DRDB_BLOOM_BITS=128` → 编译错误（#4）
- [ ] 错误配置测试：`-DRDB_MAX_SECTORS=300` → 编译错误（#7）
- [ ] 错误配置测试：MSVC 编译器 → CMake FATAL_ERROR（#6）
- [ ] TSDB 超大扇区测试：`sector_size=131072` → `RDB_ERR_PARAM`（#8）
- [ ] `test_kvdb_stress` 无 `ERR_FULL` 回归（#2）
- [ ] `test_fault_injection` 覆盖提交失败后 total_count 准确性（#3）
- [ ] GitHub Actions CI 全绿
