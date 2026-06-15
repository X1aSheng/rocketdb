# RocketDB 完整代码审查报告

**日期:** 2026-06-16  
**审查范围:** `src/rocketdb.h` · `src/rocketdb_kvdb.c` · `src/rocketdb_tsdb.c` · `interface/` · `CMakeLists.txt` · `Makefile` · 测试框架  
**审查方法:** 4 代理并行扫描（Header / KVDB / TSDB / Cross-file）+ 交叉验证  
**测试状态:** 全量 53 用例 / 44,882 断言 / 0 失败  

---

## 前次审查遗留 — 全部已修复 ✅

| # | 问题 | 修复方式 |
|---|------|---------|
| 1 | `_Static_assert` MSVC 兼容 | `RDB_STATIC_ASSERT` 宏，含 C99 typedef fallback |
| 2 | `strkey_len()` uint8_t 死循环 | 改为 `size_t`，`RDB_MAX_KEY_LEN` 降至 32 |
| 3 | dedup hash 碰撞误删数据 | 完整 key memcmp（Flash 回读） |
| 4 | Cache 命中路径 buffer 溢出 | `uint8_t kb[RDB_MAX_KEY_LEN]`（32B） |
| 5 | KVDB 大记录非对齐写入 | 对齐分块 + padding |
| 6 | TSDB seal + wg≥2 不兼容 | init/format 拒绝 `write_gran > 1` |
| 7 | 跨 epoch 查询提前停止 | `ITER_CONTINUE` 替代 `ITER_STOP` |

---

## 新发现缺陷 — 按严重度排序

### 🔴 严重

**#1 RDB_KV_CACHE_SIZE ODR 违规 — 结构体布局不匹配**

[CMakeLists.txt:293](CMakeLists.txt#L293) · [rocketdb.h:806](src/rocketdb.h#L806)

`target_compile_definitions(test_kvdb_cache PRIVATE RDB_KV_CACHE_SIZE=64)` 仅在测试目标生效，库 `rocketdb` 以默认值 0 编译。`sizeof(rdb_kvdb_t)` 不一致：

| 目标 | `cache` 大小 | `stats` 偏移 | `blooms` 偏移 |
|------|-------------|-------------|---------------|
| 库 (SIZE=0) | 1 字节 | X+1 | X+37 |
| 测试 (SIZE=64) | 1025 字节 | X+1025 | X+1061 |

**后果:** 库函数内 `db->stats` 写入测试进程的 `cache.slots` 内存区，缓存测试实质上未测试真实逻辑，存在未定义行为。

**修复方案:**
```cmake
# 将宏定义移到 library 目标的 PUBLIC 编译定义
target_compile_definitions(rocketdb PUBLIC
    RDB_KV_CACHE_SIZE=64
    RDB_BLOOM_BITS=256
)
```

---

### 🟡 高

**#2 GC 旋转回退违反 K-1 不变式**

[rocketdb_kvdb.c:2023](src/rocketdb_kvdb.c#L2023)

`gc_ensure_space` 旋转回退检查 `count_erased(db) >= 1u`，但 K-1 不变式要求 `erased > gc_reserve`。当 `gc_reserve=2` 且 `erased=2` 时，旋转后 `erased=1 < gc_reserve`，后续写入永久 `ERR_FULL`。

**修复方案:**
```c
// 将 >= 1u 改为 > db->gc_reserve
if (count_erased(db) > db->gc_reserve) {
```

**#3 TSDB 提交失败记录的 total_count 漂移**

[rocketdb_tsdb.c:643](src/rocketdb_tsdb.c#L643) · [rocketdb_tsdb.c:1312](src/rocketdb_tsdb.c#L1312)

Phase B 提交失败时 `total_count` 未增加（正确），但后续 rotation 将该 WRITING 记录恢复为 VALID 后计入 `lost`，`total_count -= lost` 减去了从未计数的记录。累积后 `get_latest/oldest` 返回 `NOT_FOUND`。

**修复方案:**
```c
// Phase B 提交失败时记录 pending_abandoned++
// rotation lost 计数时，使用 max(0, lost - pending_abandoned)
```

**#4 Bloom filter 硬编码 0x1F 掩码**

[rocketdb.h:208](src/rocketdb.h#L208-L214)

`RDB_BLOOM_SET`/`RDB_BLOOM_MAYBE` 宏硬编码 `& 0x1Fu`（仅适配 256-bit）。若 `RDB_BLOOM_BITS` 为其他值，内存越界或容量减半。

**修复方案:**
```c
RDB_STATIC_ASSERT(RDB_BLOOM_BITS == 0 || RDB_BLOOM_BITS == 256,
    "RDB_BLOOM_BITS must be 0 (disabled) or 256");
```

---

### 🟡 中

**#5 CMake 最低版本过低**

[CMakeLists.txt:1](CMakeLists.txt#L1)

`cmake_minimum_required(VERSION 3.10)` 不支持 `$<C_COMPILER_ID:Clang>`（需 3.15+）。CMake 3.10-3.14 配置阶段失败。

**修复方案:** 改为 `VERSION 3.15`。

**#6 REGISTER_TEST 不兼容 MSVC**

[tests/sim/test_framework.h:246](tests/sim/test_framework.h#L246)

`__attribute__((constructor))` 是 GCC/Clang 扩展，MSVC 静默忽略 → 0 测试注册 → 假阴性通过。

**修复方案:**
```c
#ifdef _MSC_VER
  // MSVC: 在 main() 中显式调用 test_register_case()
#else
  __attribute__((constructor))
#endif
```

**#7 RDB_MAX_SECTORS 静默截断**

[rocketdb.h:798](src/rocketdb.h#L798)

用户 `-DRDB_MAX_SECTORS=300` → `uint8_t sector_cnt = 300 % 256 = 44`，元数据分配不足，init 越界。

**修复方案:**
```c
RDB_STATIC_ASSERT(RDB_MAX_SECTORS <= 255,
    "RDB_MAX_SECTORS must be <= 255 (fits uint8_t)");
```

---

### 🟢 低

**#8 TSDB seal 中 head_off/head_count 的 uint16_t 截断**

[rocketdb_tsdb.c:627](src/rocketdb_tsdb.c#L627) · [rocketdb.h:693](src/rocketdb.h#L693)

`ts_seal` 将 `uint32_t head_off` 强制转换为 `uint16_t`。>64KB 扇区上偏移超过 65535 的记录在重启后不可见。

**修复方案:** init 时添加 `sector_size <= 65535` 校验。

**#9 HAL 契约与 1 字节写入冲突**

[interface/rocketdb_interface.h:77](interface/rocketdb_interface.h#L77)

接口文档声明"调用方保证对齐"，但引擎内部 11 处 1 字节状态写入（`mark_dead`、commit、seal）。`sim_flash` 有绕过逻辑，但生产 HAL 可能拒绝。

**修复方案:** 接口文档补充"单字节状态提交不受 write_gran 约束"。

**#10 缺少 2 的幂静态断言**

[rocketdb.h:150](src/rocketdb.h#L150)

`RDB_MIN_SECTOR_SIZE`、`RDB_FLASH_PAGE_SIZE`、`RDB_BLOOM_BITS` 文档注明"必须为 2 的幂"，但无编译时验证。

**修复方案:** 为三者添加 `RDB_STATIC_ASSERT((x) && ((x) & ((x)-1)) == 0)`。

---

## 修复验证计划

| # | 修复后验证方式 |
|---|-------------|
| 1 | `test_kvdb_cache` 全部 9 用例应产生真实的缓存命中/未命中行为；日志中 `[CACHE]` 跟踪输出应反映实际缓存操作 |
| 2 | 构造 16 扇区 KVDB，填满至 `erased == gc_reserve`，验证写入不返回 `ERR_FULL` |
| 3 | 故障注入：随机 commit 失败 → 100+ rotation → 验证 `total_count` 与 `rdb_tsdb_count()` 一致 |
| 4 | `-DRDB_BLOOM_BITS=64` 应在编译期报错 |
| 5 | CMake 3.10 环境应给出清晰的版本要求错误 |
| 6 | MSVC 构建应正确注册并运行测试用例 |
| 7 | `-DRDB_MAX_SECTORS=300` 应在编译期报错 |
| 8 | `sector_size=131072` 应在 init 时返回 `RDB_ERR_PARAM` |
| 9 | 审查 HAL 实现文档更新 |
| 10 | 编译期断言通过 |

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
