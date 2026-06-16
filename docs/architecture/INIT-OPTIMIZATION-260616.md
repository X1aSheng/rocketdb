# rdb_kvdb_init 函数集群优化分析

**日期:** 2026-06-16  
**分析范围:** `rdb_kvdb_init` + 11 个内部静态函数 (13,120 bytes, 占 KVDB .text 的 48.9%)

---

## 1. 现状分析

### 1.1 集群成员

| 函数 | 行数 | 职责 | 调用者 |
|------|------|------|--------|
| `rdb_kvdb_init` | 228 | 4 Phase 初始化主控 | API |
| `scan_sector` | 56 | 通用扇区扫描器 (回调模式) | init, fixup, bloom, GC, recalc |
| `writing_cb` | 34 | Phase 1 回调: 恢复 WRITING + 跟踪 max_seq | scan_sector |
| `fixup_stale` | 35 | Phase 2: 去重标记 + 排序调度 | rdb_kvdb_init |
| `fixup_cb` | 39 | Phase 2 回调: 去重跟踪 + 标记 DEAD | scan_sector |
| `bloom_rebuild_all` | 8 | Phase 2b 调度 | rdb_kvdb_init |
| `bloom_rebuild_sec` | 5 | 单扇区 Bloom 重建 | bloom_rebuild_all |
| `bloom_build_cb` | 14 | Phase 2b 回调: 设置 Bloom bit | scan_sector |
| `dedup_init` | 4 | 去重表初始化 | fixup_stale, GC |
| `dedup_track` | 38 | 哈希去重跟踪 (线性探测) | fixup_cb, dedup_mark_cb |
| `dedup_mark_cb` | 29 | GC 去重回调 (与 fixup_cb 近重复) | scan_sector (GC) |
| `init_sector` | 33 | 擦除 + 写扇区头 + 清 Bloom | rotate, format |
| `rotate` | 26 | 选择 erased 扇区 + 初始化 | gc_ensure_space, rdb_kvdb_init |

### 1.2 Phase 流程与 Flash I/O

```
rdb_kvdb_init()
  │
  ├─ memset(db, 0)                          零初始化句柄
  ├─ memset(meta, 0) + 恢复 erase_cnt        元数据缓冲
  │
  ├─ Phase 1: for each sector:
  │    ├─ fl_read(16B)                       读扇区头
  │    ├─ is_erased() → 3 × fl_read(4B)      擦除检查 (仅 0xFF magic)
  │    ├─ fl_read(16B CRC验证)              版本 CRC 可能重复读取
  │    └─ scan_sector(writing_cb):
  │         for each record:
  │           ├─ fl_read(16B)                读记录头
  │           └─ [WRITING] fl_read(K+V) + fl_write(1B)  CRC验证+恢复
  │
  ├─ Phase 2: fixup_stale()
  │    ├─ 排序扇区 (insertion sort, O(N²))
  │    └─ scan_sector(fixup_cb):  ← 第二次全表扫描!
  │         for each record:
  │           ├─ fl_read(key)                读 key
  │           ├─ dedup_track()               哈希表查找
  │           └─ [duplicate] mark_dead        标记旧副本
  │
  ├─ Phase 2b: bloom_rebuild_all()
  │    └─ scan_sector(bloom_build_cb):  ← 第三次全表扫描!
  │         for each record:
  │           └─ fl_read(key) + hash + BLOOM_SET
  │
  ├─ recalc_garbage_all()                    ← 第四次全表扫描 (通过 scan_sector)
  ├─ reconcile_live()                        ← 第五次全表扫描 (通过 scan_sector)
  │
  ├─ Phase 3: 选择 active sector             纯 RAM 操作
  ├─ Phase 4: 恢复 CORRUPT + 安全水位 GC
  └─ kv_cache_flush + iter_gen
```

**问题: init 过程中每个扇区被 scan_sector 扫描了 5 次！**

---

## 2. 优化方案

### 优化 1 (高收益): 合并 Phase 1 + Phase 2 + Bloom → 单次扫描

**原理:**
- Phase 1 读取扇区头后已知 `create_seq`，可先收集所有扇区信息，排序后再扫描
- 在单次扫描回调中同时完成: WRITING 恢复 + 去重跟踪 + Bloom 设置 + garbage/recalc

**实现方案:**

```c
// 新增: 组合扫描上下文
typedef struct {
    uint32_t max_seq;           // Phase 1: 最大 seq
    rdb_dedup_set_t ds;         // Phase 2: 去重表
    uint8_t*     blooms;       // Phase 2b: Bloom 位图基址
    uint32_t     garbage;       // garbage 累加
    uint32_t     live;          // live bytes 累加
} init_scan_ctx_t;

// 新增: 组合回调 (替代 writing_cb + fixup_cb + bloom_build_cb + garbage_cb)
static int init_scan_cb(rdb_kvdb_t* db, uint8_t s,
    const kv_rec_info_t* ri, void* arg) {
    init_scan_ctx_t* ctx = (init_scan_ctx_t*)arg;
    
    // Phase 1: max_seq
    if (RDB_SEQ_GT(ri->seq, ctx->max_seq)) ctx->max_seq = ri->seq;
    
    // Phase 1: WRITING recovery
    if (ri->state == RDB_STATE_WRITING) {
        recover_writing(db, ri);   // 提取出的恢复逻辑
        if (ri->state != RDB_STATE_VALID) goto skip; // 未恢复成功则跳过后续
    }
    if (ri->state != RDB_STATE_VALID) goto skip;
    
    // Phase 2: dedup
    { /* 原 fixup_cb 逻辑 */ }
    
    // Phase 2b: bloom
    { /* 原 bloom_build_cb 逻辑 */ }
    
skip:
    // Phase garbage/live accounting
    { /* 原 garbage_cb + live_cb 逻辑 */ }
    return RDB_ITER_CONTINUE;
}
```

**节省:**
- Flash: 从 5 次全表扫描 → **1 次全表扫描** (80% 减少)
- ROM: 消除 fixup_stale (排序单独保留), bloom_rebuild_all, garbage/live callbacks → **~800 bytes**
- init 时间: 正比于 Flash 读取次数, 约 **3-5× 加速**

**风险:** 中等 — 逻辑合并影响多个子系统, 需要完整回归测试

---

### 优化 2 (中收益): 统一 fixup_cb 和 dedup_mark_cb

**现状:** 两个函数 95% 相同, 仅差异在:
- `fixup_cb`: 更新 `db->sectors[s].garbage_bytes` 和计数器
- `dedup_mark_cb`: 不更新 garbage (GC 中单独计算)

**方案:**

```c
// 之前: 两个函数 (39 + 29 = 68 行)
static int fixup_cb(...) { /* 39 行 */ }
static int dedup_mark_cb(...) { /* 29 行 */ }

// 之后: 一个函数 + flags (42 行)
static int dedup_scan_cb(rdb_kvdb_t* db, uint8_t s,
    const kv_rec_info_t* ri, void* arg) {
    dedup_ctx_t* dx = (dedup_ctx_t*)arg;
    // ... 统一逻辑 ...
    if (r == RDB_DEDUP_SEEN) {
        kv_mark_dead(db, ri->addr);
        kv_cache_invalidate(db, ...);
        if (dx->flags & DEDUP_UPDATE_GARBAGE)  // ← 关键差异
            db->sectors[s].garbage_bytes += ri->rsz;
    }
}
```

**节省:** ROM **~250 bytes**

---

### 优化 3 (低收益): Phase 3 双循环合并

**现状:** 两个几乎相同的循环 (L2333-2343 和 L2347-2356), 仅差一个条件

```c
// 之前: 两个循环
for (s...) {  // 第一遍: 优先有剩余空间的
    if (write_off < sector_size && (best == 0xFF || seq > best_cs))
        best = s;
}
if (best == 0xFF) {
    for (s...) {  // 第二遍: 只考虑已满的
        if (best == 0xFF || seq > best_cs)
            best = s;
    }
}

// 之后: 单循环
uint8_t best_full = 0xFF;
uint32_t best_cs_full = 0;
for (s...) {
    if (status != ACTIVE && status != SEALED) continue;
    if (write_off < sector_size && seq > best_cs)
        best = s, best_cs = seq;          // 有空闲的最优
    else if (seq > best_cs_full)
        best_full = s, best_cs_full = seq; // 已满的最优 (备选)
}
if (best == 0xFF) best = best_full;
```

**节省:** ROM **~60 bytes**

---

### 优化 4 (低收益): is_erased() 减少潜在重复读取

**现状:** `is_erased` 在 `rdb_kvdb_init` Phase 1 中调用, 此时已经读了扇区头 `sh` (16 bytes, 包含 magic 字段)。如果 `sh.magic == 0xFFFFFFFFu` 表示可能擦了, 需要调用 `is_erased` 确认。

`is_erased` 做 3 次 Flash 读取 (begin / mid / end)。但 Phase 1 已经读过了 begin (扇区头), 所以第 1 次探测是冗余的。

**方案:** 传入已读取的第一个 word (扇区头 magic), 跳过第一次探测:

```c
// 之前: 3 probes
static int is_erased(const rdb_kvdb_t* db, uint8_t s) {
    // probe 1: first 4 bytes
    // probe 2: midpoint
    // probe 3: last 4 bytes
}

// 之后: 2 probes (probe 1 已知为 0xFFFFFFFF)
static int is_erased_from_midpoint(const rdb_kvdb_t* db, uint8_t s) {
    // probe 2: midpoint (saves 1 flash read per erased sector)
    // probe 3: last 4 bytes
}
```

**节省:** ROM ~30 bytes, 每次擦除扇区检查省 1 次 Flash 读取

---

### 优化 5 (低收益): writing_cb 减少头重复读取

**现状:** `scan_sector` 已经读了 16 字节记录头 (`rh`), 填充到 `ri`。`writing_cb` 又读了一次记录头:

```c
static int writing_cb(rdb_kvdb_t* db, uint8_t s,
    const kv_rec_info_t* ri, void* arg) {
    // ...
    rdb_kv_record_hdr_t rh;
    if (fl_read(db, ri->addr, &rh, sizeof(rh)) != 0) {  // ← 重复!
```

**方案:** 在 `kv_rec_info_t` 中增加 `data_crc` 字段, 由 `scan_sector` 填充, `writing_cb` 直接使用:

```c
typedef struct {
    // ... existing fields ...
    uint16_t data_crc;   // ← 新增: 避免 writing_cb 重复读头
} kv_rec_info_t;
```

**节省:** ROM ~20 bytes, 每个 WRITING 记录省 1 次 Flash 读取 (16 bytes)

---

### 优化 6 (中收益): recalc_garbage_all + reconcile_live 合并

**现状:** 两个独立的全表扫描

| 函数 | 扫描 | 回调 | 逻辑 |
|------|------|------|------|
| `recalc_garbage_all` | scan_sector | `garbage_cb` | 统计非 VALID 记录的 garbage |
| `reconcile_live` | scan_sector | `live_cb` | 统计 VALID 记录的总 live bytes |

**方案:** 合并为一个回调, 同时统计 garbage 和 live。

**节省:** ROM **~100 bytes**, Flash 读取减半

---

## 3. 综合收益估算

| 优化 | 难度 | ROM 节省 | Flash I/O 节省 (init) | 风险 |
|------|------|---------|----------------------|------|
| **1. Phase 1+2+Bloom 合并** | 中 | ~800 B | **5×→1× (80%)** | 中 |
| **2. fixup_cb/dedup_mark_cb 统一** | 低 | ~250 B | 无 | 低 |
| **3. Phase 3 循环合并** | 低 | ~60 B | 无 | 低 |
| **4. is_erased 去冗余探测** | 低 | ~30 B | 每 erased 扇区 -1 读 | 低 |
| **5. writing_cb 去重复头读** | 低 | ~20 B | 每 WRITING 记录 -1 读 | 低 |
| **6. garbage+live 合并** | 低 | ~100 B | 2×→1× (50%) | 低 |

### 合计

| 指标 | 优化前 | 优化后 | 改善 |
|------|--------|--------|------|
| init 集群 ROM | 13,120 B | **~11,860 B** | **-9.6%** |
| KVDB .text | 26,818 B | **~25,558 B** | **-4.7%** |
| init 全表扫描 | 5 遍 | **1 遍** | **-80%** |
| 内部静态函数数 | 18 | **~14** | 减少 4 个 |
| callback 类型数 | 6 | **2** | 统一 |

### init 时间估算 (8 扇区, 每扇区 100 条记录, SPI 40MHz)

| 操作 | 优化前 | 优化后 |
|------|--------|--------|
| Phase 1 (扫描+恢复) | ~8ms | — |
| Phase 2 (去重) | ~8ms | — |
| Phase 2b (Bloom) | ~8ms | — |
| garbage/live | ~16ms (2遍) | — |
| **合并扫描** | — | **~10ms** |
| **总 init** | **~45ms** | **~15ms** (3× 快) |

---

## 4. 实施建议

### 第 1 步 (低风险, 立即见效): 优化 3+4+5+6
- Phase 3 循环合并
- is_erased 去冗余
- writing_cb 去重复头读
- garbage+live 合并
- **预估:** ROM -210 bytes, 低风险, 易验证

### 第 2 步 (低风险, 代码清理): 优化 2
- 统一 fixup_cb/dedup_mark_cb
- **预估:** ROM -250 bytes, 消除重复代码

### 第 3 步 (中风险, 最大收益): 优化 1
- 合并 Phase 1+2+Bloom 为单次扫描
- 需要仔细设计组合回调的数据结构
- **预估:** ROM -800 bytes, Flash I/O -80%
- **需要:** 完整回归测试 (55 用例 / 48K 断言)

---

## 5. 不推荐的优化

### ❌ sort 改用 qsort
- `fixup_stale` 用 insertion sort (O(N²), N≤255), 实际 N 通常 <16
- 内置 qsort 需要函数指针和比较函数, 对 tiny N 反而不如 insertion sort
- ROM 反而会增加 (~200 bytes for qsort)

### ❌ dedup 改用链式哈希
- 当前线性探测在 32 槽中小负载下已足够
- 链式哈希需要额外内存管理, 对嵌入式不适用

### ❌ 取消 dedup, 改用 find_latest 全量比较
- dedup 通过 hash+prefix 快速跳过已知 key, 比 find_latest 遍历所有扇区快得多
- 删除 dedup 会拖慢 init, 不能节省足够 ROM 来弥补性能损失
