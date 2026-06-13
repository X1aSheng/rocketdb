# RocketDB Function Naming — Semantic Analysis

## 1. 命名规范总览

### 1.1 公共 API（`rocketdb.h`）

| 模式 | 示例 | 语义清晰度 |
|------|------|-----------|
| `rdb_{engine}_{verb}` | `rdb_kvdb_init`, `rdb_tsdb_append` | ⭐ 优秀 — 模块+操作一目了然 |
| `rdb_{engine}_{verb}_{noun}` | `rdb_kvdb_get_stats`, `rdb_kvdb_reset_stats` | ⭐ 优秀 — verb-object 分明 |
| `rdb_{engine}_query_ex` | `rdb_tsdb_query_ex` | ⭐ 良好 — `_ex` 为 "extended" 惯例 |

**结论**：公共 API 命名一致、可读性高，`rdb_` → `{engine}` → `{verb}[_{noun}]` 三层结构清晰。

### 1.2 引擎内部函数

#### KVDB（`rocketdb_kvdb.c`）

| 类别 | 函数 | 模式 |
|------|------|------|
| 几何辅助 | `wr_gran()`, `data_start()`, `data_cap()`, `sec_addr()`, `rec_size()`, `max_live()` | `{noun}_{verb}` |
| Flash 包装 | `fl_lock()`, `fl_read()`, `fl_write()`, `fl_erase()`, `fl_yield()` | `fl_{verb}` |
| 扫描回调 | `find_cb()`, `fixup_cb()`, `garbage_cb()`, `writing_cb()`, `live_cb()`, `dedup_mark_cb()`, `bloom_build_cb()` | `{context}_cb` |
| GC 子系统 | `gc_build_cache()`, `gc_execute()`, `gc_migrate_cb()`, `gc_avail()` | `gc_{verb}[_{noun}]` |
| 缓存 | `kv_cache_lookup()`, `kv_cache_insert()`, `kv_cache_invalidate()`, `kv_cache_flush()` | `kv_cache_{verb}` |
| 去重 | `rdb_dedup_init()`, `rdb_dedup_track()` | `rdb_dedup_{verb}` |
| Bloom | `bloom_rebuild_sec()`, `bloom_rebuild_all()` | `bloom_rebuild_{scope}` |

#### TSDB（`rocketdb_tsdb.c`）

| 类别 | 函数 | 模式 |
|------|------|------|
| 几何辅助 | `twr()`, `tds()`, `tdc()`, `trs()`, `tsa()` | `t{abbr}_{nothing}` — 极简缩写 |
| Flash 包装 | `tlock()`, `tunlock()`, `trd()`, `twr_f()`, `tera()`, `tyield()` | `t{verb}` |
| 引擎操作 | `ts_classify()`, `ts_rotate()`, `ts_seal()`, `ts_scan()`, `ts_active_info()` | `ts_{verb}[_{noun}]` |
| 查询 | `ts_qcb()`, `ts_old_cb()`, `ts_query_impl()`, `ts_find_last_valid()` | 混合风格 |

---

## 2. ⚠️ 问题分析

### I1 — 同名抽象层命名分叉（Flash 包装函数）

KVDB 和 TSDB 实现了相同的 Flash 抽象层，但命名完全不同：

| 操作 | KVDB | TSDB |
|------|------|------|
| 读 | `fl_read(db, ...)` | `trd(db, ...)` |
| 写 | `fl_write(db, ...)` | `twr_f(db, ...)` |
| 擦除 | `fl_erase(db, ...)` | `tera(db, ...)` |
| 锁 | `fl_lock(db)` | `tlock(db)` |
| 解锁 | `fl_unlock(db)` | `tunlock(db)` |
| 让步 | `fl_yield(db)` | `tyield(db)` |

- **KVDB**：`fl_` 前缀可读，`fl` ≈ "flash"
- **TSDB**：`t` 单字母前缀不可读，`twr_f` 连"写"都要猜（write flash?），`trd` 似 read 却又不像

**建议**：统一为 `fl_` 前缀（或 `rdb_flash_`），从 KVDB 中提取公共内联函数到 header。

### I2 — 几何辅助函数命名差

| 含义 | KVDB（可读） | TSDB（晦涩） |
|------|-------------|-------------|
| 写入粒度 | `wr_gran()` | `twr()` |
| 数据起始偏移 | `data_start()` | `tds()` |
| 数据容量 | `data_cap()` | `tdc()` |
| 记录大小 | `rec_size()` | `trs()` |
| 扇区地址 | `sec_addr()` | `tsa()` |

- `twr` → 猜不出是 "TS write granularity" 还是 "two"
- `tds` → 和 "Transaction Data Set" / TDS 协议撞名
- `tsa` → 和 "Time Stamping Authority" 撞名

**建议**：TSDB 改用完整命名：`ts_wr_gran()`, `ts_data_start()`, `ts_data_cap()`, `ts_rec_size()`, `ts_sec_addr()`。与 KVDB 保持一致命名模式。

### I3 — `mark_dead` vs `ts_mark_dead`

- KVDB: `mark_dead(db, addr)` — 简洁，但缺少命名空间
- TSDB: `ts_mark_dead(db, addr)` — 命名空间清晰

两者功能完全相同，命名不统一。

**建议**：KVDB 改为 `kv_mark_dead()` 或在两文件间提取公共内联函数。

### I4 — `rdb_dedup_*` 误用 `rdb_` 前缀

`rdb_dedup_init()` 和 `rdb_dedup_track()` 是 `static` 内部函数，但使用了 `rdb_` 公共 API 前缀：

```c
// rocketdb_kvdb.c
static void rdb_dedup_init(rdb_dedup_set_t* ds) { ... }
static int  rdb_dedup_track(rdb_dedup_set_t* ds, ...) { ... }
```

`rdb_` 前缀在 rocketdb.h 中定义为公共 API 保留。内部函数使用它会混淆阅读者：它是可导出的吗？不是（`static`）。

**建议**：改为 `dedup_init()` / `dedup_track()` 或 `kv_dedup_init()` / `kv_dedup_track()`。

### I5 — `strkey_len` 前缀不清

`strkey_len()` 用于安全提取以 null 结尾的 key 长度：

```c
static int strkey_len(const char* key, uint8_t* out_len);
```

`strkey_` 前缀令人困惑：是 "string key"？"structured key"？`key_strlen()` 或 `key_scan_len()` 更自描述。

**建议**：改为 `key_scan_len()`。

### I6 — `ensure_space` 含义不完整

```c
static rdb_err_t ensure_space(rdb_kvdb_t* db, uint32_t need, ...);
```

函数名下只说了"确保空间"，没有表达**怎么**确保——它会触发垃圾回收。`gc_ensure_space()` 或 `ensure_gc_headroom()` 更准确地反映其行为：不仅是检查，而是主动回收。

**建议**：改为 `gc_ensure_space()` 以明确其会触发 GC。

### I7 — `ts_qcb` 过度缩写

`ts_qcb` 是 "TS query callback" 的缩写。少于 6 个字符的缩写应避免：

```c
static int ts_qcb(rdb_tsdb_t* db, const ts_rec_t* r, void* arg);
```

对比 `ts_old_cb()`（oldest callback）—— 后者虽短但意思清楚。

**建议**：改为 `ts_query_cb()`。

### I8 — `int_sz_next` 上下文前缀不明

```c
// test_integration.c
static uint16_t int_sz_next(void);
```

`int_` 前缀指 "integration test" 而不是 "integer"（也不像 "interrupt"）。但单从调用点看，读者无法判断前缀含义。

**建议**：改为 `integ_sz_next()` 或 `mix_sz_next()`（该文件已有 `mix_` 系列函数）。

### I9 — `corrupt_skip` vs `ts_corrupt_skip`

| KVDB | TSDB |
|------|------|
| `corrupt_skip(db)` | `ts_corrupt_skip(db)` |

功能相同命名不同。KVDB 的函数名缺少模块前缀。

**建议**：KVDB 侧改为 `kv_corrupt_skip()` 以匹配 `ts_corrupt_skip()` 的风格。

### I10 — `lcg_next` 依赖读者了解算法缩写

```c
// test_kvdb_basic.c / test_kvdb_stress.c
static uint32_t lcg_next(uint32_t *state);
```

"LCG" 是 Linear Congruential Generator 的缩写。只在此测试文件内部使用，但对新读者不友好。

**建议**：改为 `prng_next()` 或 `rand_next()`，或加注释说明 LCG 缩写。

### I11 — TSDB 回调函数 `ts_old_cb` 命名模糊

`ts_old_cb` 用于 `get_oldest`，但只从名字看不出和 "oldest" 的关系：

```c
static int ts_old_cb(rdb_tsdb_t* db, const ts_rec_t* r, void* arg);
```

**建议**：改为 `ts_oldest_cb()`。

### I12 — 回调函数后缀不统一

| 当前名称 | 建议 |
|----------|------|
| `find_cb` | 可接受，上下文明确 |
| `fixup_cb` | ✅ 好 |
| `writing_cb` | ✅ 好 |
| `garbage_cb` | ✅ 好 |
| `live_cb` | ⚠️ `live_records_cb` 更准确 |
| `bloom_build_cb` | ✅ 好 |
| `dedup_mark_cb` | ✅ 好 |
| `gc_migrate_cb` | ✅ 好 |
| `ts_qcb` | ❌ → `ts_query_cb` |
| `ts_old_cb` | ❌ → `ts_oldest_cb` |
| `ts_count_cb` | ⚠️ 出现在 test 文件中，属于回调，命名 ok |

### I13 — C 测试宏命名缺乏项目前缀

`TEST_CASE` 和 `TEST_ASSERT` 等宏无项目前缀。虽然在测试文件中可接受（范围有限），但在大项目中与其他测试框架有碰撞风险。

```c
TEST_CASE(kv_set_get_basic, "KVDB", "Basic set/get operations")
TEST_ASSERT(rdb_kvdb_set(db, "key", "val", 3) == RDB_OK);
```

**建议**：当前状态可接受（仅在测试编译单元中定义），但如果扩展到更多测试套件，考虑 `RDB_TEST_CASE` / `RDB_TEST_ASSERT`。

### I14 — Python 工具函数名分析

| 函数 | 分析 |
|------|------|
| `align_up()` | ⭐ 好，几何辅助 |
| `crc16_modbus()` | ⭐ 好，算法名+参数 |
| `is_erased()` | ⭐ 好 |
| `detect_kind()` | ⭐ 好 |
| `parse_kvdb()` / `parse_tsdb()` | ⭐ 好 |
| `cmd_inspect()` / `cmd_verify()` / `cmd_export()` | ⭐ 好，`cmd_` 前缀清晰标识 CLI 命令 |
| `state_name()` | ⭐ 好 |

Python 侧命名整体质量高。

---

## 3. 语义一致性评分

### 3.1 各模块评分

| 模块 | 评分 | 说明 |
|------|------|------|
| 公共 API（rocketdb.h） | ⭐ 9/10 | 结构清晰，偶有 `_ex` vs `_impl` 需靠惯例 |
| KVDB 内部 | ⭐ 7/10 | 词义明确，但 `ensure_space` 名不副实、`rdb_dedup_*` 前缀误用 |
| TSDB 内部 | ⭐ 5/10 | 单字母 `t` 大幅降低可读性，几何辅助函数难以猜义 |
| 测试框架 | ⭐ 8/10 | 整体良好，`lcg_next` 和 `int_sz_next` 有提升空间 |
| 故障注入 | ⭐ 8/10 | 命名一致，`quick_` 系列稍显随意但功能清晰 |
| Flash 模拟 | ⭐ 8/10 | `sim_flash_*` 前缀统一，函数名自描述 |
| 接口模板 | ⭐ 9/10 | 命名规整，`rocketdb_interface_` 前缀一致 |
| Zephyr 移植 | ⭐ 9/10 | 优秀，`rdb_zephyr_*` + `rocketdb_partition_init` |
| Python 工具 | ⭐ 9/10 | 命名质量高，`cmd_` 前缀是亮点 |

### 3.2 总体评分

**总评：7.5/10** — 公共 API 优秀，KVDB 良好，TSDB 内部和测试辅助函数有改进空间。

---

## 4. 改进优先级

| 优先级 | 问题 | 影响范围 | 建议改动 |
|--------|------|---------|---------|
| P0 | I1: Flash 包装命名分叉 | 开发效率 | 统一 `fl_` 前缀，TSDB 侧改 `tlock`→`fl_lock` 等 |
| P0 | I2: TSDB 几何辅助晦涩 | 可维护性 | `twr`→`ts_wr_gran`, `tds`→`ts_data_start` 等 |
| P1 | I4: `rdb_dedup_*` 前缀误用 | API 混淆 | `rdb_dedup_init`→`dedup_init` |
| P1 | I5: `strkey_len` 前缀不清 | 可读性 | → `key_scan_len` |
| P1 | I6: `ensure_space` 名不副实 | 可读性 | → `gc_ensure_space` |
| P2 | I3: `mark_dead` vs `ts_mark_dead` | 一致性 | KVDB → `kv_mark_dead` |
| P2 | I9: `corrupt_skip` vs `ts_corrupt_skip` | 一致性 | → `kv_corrupt_skip` |
| P2 | I7: `ts_qcb` 缩写 | 可读性 | → `ts_query_cb` |
| P2 | I11: `ts_old_cb` 模糊 | 可读性 | → `ts_oldest_cb` |
| P3 | I8: `int_sz_next` 无用前缀 | 可读性 | → `integ_sz_next` / `mix_sz_next` |
| P3 | I10: `lcg_next` 晦涩 | 可读性 | 加注释或改名 `prng_next` |
| P3 | I12: 回调后缀统一 | 一致性 | `live_cb`→`live_records_cb`, `ts_old_cb`→`ts_oldest_cb` |
| P4 | I13: 测试宏前缀 | 防碰撞 | 暂不建议改动，风险较低 |

---

## 5. 优秀命名示例

以下函数命名值得作为全项目标准：

```c
// 公共 API — 结构完美
rdb_kvdb_init           // rdb + 模块 + 操作
rdb_kvdb_get_stats      // rdb + 模块 + verb + noun
rdb_tsdb_reset_epoch    // rdb + 模块 + verb + noun

// 内部函数 — 自描述
sim_flash_read           // 模块 + 操作
trace_kvdb_snapshot      // 模块 + 具体操作
fault_quick_write_fail   // 模块 + 快捷方式 + 故障类型
gc_execute               // 模块 + 操作
recalc_garbage           // verb + noun — 清楚做什么
bloom_rebuild_all        // 操作 + 范围 — 清楚做什么和范围
```
