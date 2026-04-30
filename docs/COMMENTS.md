# COMMENTS（代码注释指南）

本文档说明 RocketDB 源代码中的注释约定和如何快速定位关键代码段。

---

## 注释风格

### 文件头注释

每个源文件（`.c` / `.h`）以统一格式开始：

```c
/**
 * @file rocketdb_kvdb.c
 * @brief 键值数据库实现（KVDB）
 * 
 * 包含四阶段 GC、迭代器、CRC 校验等核心功能。
 * 
 * @note 所有操作都是原子的；掉电时不会发生数据损坏。
 * @date 2026-02-20
 * @version 0.0.2
 */
```

### 函数文档

所有公开 API（`rdb_*`）都有详细注释：

```c
/**
 * 初始化键值数据库
 * 
 * 执行扫描已有数据、恢复崩溃状态、准备垃圾回收等操作。
 * 第一次调用时 Flash 可能全为 0xFF；此时初始化为空数据库。
 * 
 * @param db[out]       指向未初始化的 KVDB 结构
 * @param part[in]      Flash 分区配置（base_addr, sector_size 等）
 * @param sectors[out]  扇区元数据数组（init 过程中填充）
 * 
 * @return RDB_OK 初始化成功
 * @return RDB_ERR_FULL Flash 容量不足（分配给 KVDB 的空间太小）
 * @return RDB_ERR_INVALID 参数无效
 * 
 * @note 此函数应在其他操作前调用
 * @warning 不支持多线程调用；必须使用 mutex 保护
 */
int rdb_kvdb_init(rdb_kvdb_t *db, const rdb_partition_t *part, 
                  rdb_kv_sector_meta_t *sectors);
```

### 内联注释

**关键决策或非显而易见的逻辑**需要注释：

```c
// ✓ 好：解释为何要做这件事
if (garbage_bytes > threshold) {
    // 垃圾过多，需要回收。先找最多碎片的扇区
    victim_sec = find_most_fragmented_sector();
}

// ✗ 差：冗余或无用
if (garbage_bytes > threshold) {
    victim_sec = find_most_fragmented_sector();  // 找最多碎片的扇区
}

// ✗ 差：笑话或过度
if (garbage_bytes > threshold) {
    victim_sec = find_most_fragmented_sector();  // Houston, we have garbage 🚀
}
```

### 阶段标记

代码中用标记突出关键路径：

```c
// ============================================================================
// Phase 0: 扫描并计算垃圾字节数
// ============================================================================
// 此阶段遍历所有活跃记录和已删除记录，计算每个扇区的浪费空间

for (uint32_t sid = 0; sid < db->sector_cnt; sid++) {
    // 枚举此扇区的所有记录
    // ...
}

// ============================================================================
// Phase 1: 选择最多垃圾的扇区并重敲
// ============================================================================
// 从未密封的扇区中选择垃圾百分比最高的一个
```

---

## 快速导航

### 按功能查找代码

| 功能 | 文件 | 函数/宏 | 行号范围 |
|------|------|-------------|----------|
| KVDB 初始化 | `rocketdb_kvdb.c` | `rdb_kvdb_init()` | ~150-250 |
| KV Set（插入/更新） | `rocketdb_kvdb.c` | `rdb_kvdb_set()` | ~300-400 |
| KV Get（查询） | `rocketdb_kvdb.c` | `rdb_kvdb_get()` | ~430-480 |
| KV Delete | `rocketdb_kvdb.c` | `rdb_kvdb_delete()` | ~490-540 |
| KV 迭代器 | `rocketdb_kvdb.c` | `rdb_kv_iter_init()`, `rdb_kv_iter_next()` | ~550-700 |
| 四阶段 GC | `rocketdb_kvdb.c` | `rdb_kvdb_gc()` | ~800-1200 |
| GC Phase 0-3 | `rocketdb_kvdb.c` | `gc_phase_*()` 静态函数 | ~810-1100 |
| GC Phase 4 (wear balance) | `rocketdb_kvdb.c` | 在 `gc_phase_4()` 中 | ~1050-1100 |
| CRC16 计算 | `rocketdb_kvdb.c` | `rdb_crc16()` | ~50-100 |
| TSDB 初始化 | `rocketdb_tsdb.c` | `rdb_tsdb_init()` | ~100-200 |
| TSDB 追加数据 | `rocketdb_tsdb.c` | `rdb_tsdb_append()` | ~250-350 |
| TSDB 查询 | `rocketdb_tsdb.c` | `rdb_tsdb_query()` | ~400-550 |
| TSDB Rotation（轮转） | `rocketdb_tsdb.c` | `ts_rotation()` 静态函数 | ~600-700 |

### 常见问题代码位置

| 问题 | 查看位置 |
|------|----------|
| "为什么我的 GC 这么慢？" | `rocketdb_kvdb.c` 中的 GC Phase 0-3 和 `yield()` 调用 |
| "掉电数据会丢失吗？" | `rocketdb_kvdb.c` 中 `set()` 函数的 **write_seq** 和 **CRC** 校验逻辑 |
| "迭代器为什么返回 BUSY？" | `rocketdb_kvdb.c` 中的 `rdb_kv_iter_next()`，检查 `iter_gen != db->iter_gen` |
| "TSDB 如何处理旧数据？" | `rocketdb_tsdb.c` 中的 `ts_rotation()` 函数（自动删除超过 epoch 范围的记录） |
| "磨损是否均衡？" | `rocketdb_kvdb.c` 中的 GC Phase 4，检查 `gc_reserve` 和 `gc_candidate_idx` 逻辑 |

---

## 注释密度与风格

### KVDB 核心路径示例

下例展示了典型的注释风格：

```c
// ============================================================================
// 设置键值对（插入或更新）
// ============================================================================

int rdb_kvdb_set(rdb_kvdb_t *db, const char *key, const uint8_t *val, 
                 size_t val_len) {
    // 1. 参数检查
    if (!db || !key || !val || val_len == 0) {
        return RDB_ERR_INVALID;
    }

    // 2. 计算记录大小：magic(1) + klen(2) + key + vlen(2) + val + crc(2)
    size_t key_len = strlen(key);
    size_t record_size = 1 + 2 + key_len + 2 + val_len + 2;

    // 3. 找存活扇区
    // 存活扇区定义：unsealed && !erased && used_bytes < sector_size
    rdb_kv_sector_meta_t *live_sec = find_live_sector(db);
    if (!live_sec) {
        // 没有存活扇区，可能需要 GC
        return RDB_ERR_FULL;
    }

    // 4. 如果此键已存在，标记旧记录为删除状态
    if (key_exists(db, key)) {
        garbage_bytes += old_record_size;  // 垃圾字节累加
    }

    // 5. 在存活扇区末尾写入新记录
    uint32_t write_addr = live_sec->base_addr + live_sec->used_bytes;
    write_record_header(db, write_addr, key, val, val_len);
    
    // 6. 递增序列号以标记这次修改
    db->write_seq++;
    
    // 7. 修改数据库时，递增迭代器 gen（让 RDB_ERR_BUSY 生效）
    db->iter_gen++;

    return RDB_OK;
}
```

### TSDB 查询路径示例

```c
// ============================================================================
// 时间序列数据库：按时间范围查询（回调模式）
// ============================================================================

// 查询回调 — 每匹配一条记录被调用一次
static int query_cb(uint32_t time, const void *data, uint16_t len, void *arg) {
    int *count = (int *)arg;
    (*count)++;
    // 在此处理记录：data 为记录负载，len 为字节长度，time 为时间戳
    // 返回 RDB_ITER_STOP 可提前终止查询
    return RDB_ITER_CONTINUE;
}

int rdb_tsdb_query(rdb_tsdb_t *db, uint32_t from, uint32_t to,
                   rdb_ts_cb_t cb, void *arg) {
    // 1. 从 tail（最老）向 head（最新）遍历
    // 2. 对每条 VALID 记录检查 time ∈ [from, to]
    // 3. 匹配时调用 cb(time, data, len, arg)
    // 4. 若 from > to 则自动交换
    // 5. from=0 表示从最早开始，to=0 表示到最新结束

    return RDB_OK;
}
```

---

## 设计决策注释

某些特殊注释标记关键设计决策：

```c
// DESIGN: 为什么使用 write_seq？
// write_seq 是一个递增计数器，每次修改都递增。它用于：
// 1. 掉电恢复：识别哪些记录是完整写入的
// 2. CRC 校验：协助检测数据损坏
// 参见 clarify.md 问题 2：write_seq 与掉电一致性
```

``` c
// DESIGN: GC Phase 4 为什么存在？
// 即使 GC Phases 0-3 完成，如果所有扇区都被触发过 GC，
// 某个扇区可能长期未被 compact。Phase 4 确保轮转所有扇区。
// 这是磨损均衡的关键。参见 design.md#gc-wear-balance
```

---

## 测试与注释

测试代码中的注释说明测试目的和预期：

```c
// Test: 基本 set/get 操作
// 预期：插入后能正确读取，CRC 校验通过
void test_kv_basic_set_get(void) {
    RDB_ASSERT_EQ(rdb_kvdb_set(&db, "key1", val, len), RDB_OK);
    
    uint16_t read_len = 0;
    RDB_ASSERT_EQ(rdb_kvdb_get(&db, "key1", buf, sizeof(buf), &read_len), RDB_OK);
    RDB_ASSERT_EQ(read_len, len);
    RDB_ASSERT_MEMEQ(buf, val, len);  // ← CRC 已在 get() 内部校验
}

// Test: 掉电恢复
// 预期：在第 N 次 write 时注入掉电，重启后数据一致（无损坏，无丢失）
void test_power_loss_recovery(void) {
    fault_quick_power_loss(&g_kv_fault, 1u, 0u);  // 第 1 次写时中断
    
    // 验证恢复
    rdb_kvdb_init(&db, part, sectors);
    // 此时数据库应处于一致状态
}
```

---

## 注释约定总结

| 类别 | 风格 | 示例 |
|------|------|------|
| 文件头 | JavaDoc 风格 | `/** @file ... @brief ... */` |
| 函数文档 | 详细 + 参数 + 返回值 | `@param`, `@return` |
| 设计决策 | `// DESIGN: ...` | 链接到 clarify.md 或 design.md |
| 关键段落 | 分隔线 + 标题 | `// ===== Phase 0: ... =====` |
| 非显而易见逻辑 | 简洁解释 | `// 计算记录总大小（包括 CRC）` |
| TODO / 未来工作 | `// TODO: ...` 或 `@todo` | 性能优化留给 v1.0 |

---

## 阅读路径建议

### 初学者

1. 从 `rocketdb.h` 开始，理解数据结构
2. 查看 `README.md` 中的快速示例
3. 阅读 `rocketdb_kvdb.c` 中的 `rdb_kvdb_init()` 函数
4. 逐步查看 `set()` 和 `get()` 实现

### 进阶开发者

1. 查看 `rocketdb_kvdb.c` 中的 GC 四阶段注释
2. 研究 `write_seq` 和 CRC 校验逻辑
3. 阅读 `test/` 中的压力测试代码
4. 检查 `TROUBLESHOOTING.md` 中对磨损不均衡的诊断

### 贡献者

1. 理解整个 GC 流程（Phase 0-4）
2. 查看所有 `// DESIGN:` 前缀的注释
3. 参考 `clarify.md` 和 `design.md`
4. 运行完整回归测试套件（`.\build.bat all test`）

---

## 代码质量标准

所有代码遵循以下原则：

- ✅ 函数长度 < 100 行（复杂函数 < 300 行）
- ✅ 所有 `rdb_*` API 都有文档化注释
- ✅ 关键设计决策有 `// DESIGN:` 注释
- ✅ 测试代码包含预期说明
- ✅ 错误路径清晰标记（`return RDB_ERR_*`）

---

## 快速查找命令

在源代码中快速搜索：

```bash
# 查找所有 DESIGN 注释
grep -n "DESIGN:" rocketdb_kvdb.c

# 查找 Phase 标记
grep -n "Phase [0-9]:" rocketdb_kvdb.c

# 查找所有公开 API（rdb_* 前缀）
grep -n "^int rdb_" rocketdb_kvdb.c

# 查找测试注释
grep -n "// Test:" test/sim/test_*.c
```

---

**最后更新**：2026年4月29日  
**版本**：RocketDB v1.1.0
