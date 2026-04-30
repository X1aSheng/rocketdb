# Implement（实现）

## 当前实现状态

### 源文件汇总

| 文件 | 大小 | 说明 | 状态 |
|------|------|------|------|
| `rocketdb.h` | ~1200 行 | 公共类型、常量、API 声明 | ✅ 完成 |
| `rocketdb_kvdb.c` | ~1500 行 | KVDB 引擎核心实现 | ✅ 完成 |
| `rocketdb_tsdb.c` | ~900 行 | TSDB 引擎核心实现 | ✅ 完成 |
| `tests/sim/` | 多个 | 模拟 Flash 驱动层、测试向量 | ✅ 部分完成 |

### 已实现功能

#### KVDB 核心功能
- [x] `rdb_kvdb_init()` — 初始化扫描、recovery、相位恢复
- [x] `rdb_kvdb_format()` — 全盘擦除
- [x] `rdb_kvdb_set()` — 键值写入，含自动 GC
- [x] `rdb_kvdb_get()` — 键值查询，CRC 校验
- [x] `rdb_kvdb_delete()` — 键标记删除
- [x] `rdb_kvdb_exists()` — 键存在性快速查询
- [x] `rdb_kvdb_gc()` — 手动垃圾回收触发
- [x] `rdb_kvdb_space_info()` — 空间统计
- [x] `rdb_kvdb_wear_info()` — 磨损信息
- [x] `rdb_kv_iter_init/next()` — 遍历所有有效记录
- [x] `rdb_kvdb_get_stats/reset_stats()` — 运行统计

#### KVDB 内部机制
- [x] 四阶段评分 GC（Phase 0~3：zero-live, low-live, wear, oldest）
- [x] 静态磨损均衡（Phase 4：时隔 GC_WEAR_THRESHOLD 的冷扇区主动擦除）
- [x] 二次写（WRITING → VALID → DEAD）
- [x] 掉电一致性 recovery（1~4 阶段恢复过期副本、安全水位）
- [x] 防死锁兜底（init 阶段 Phase 4）

#### TSDB 核心功能
- [x] `rdb_tsdb_init()` — 初始化，定位 head/tail
- [x] `rdb_tsdb_format()` — 全盘擦除
- [x] `rdb_tsdb_append()` — 追加时序数据，自动 rotation
- [x] `rdb_tsdb_query_by_time()` — 时间范围查询
- [x] `rdb_tsdb_get_range()` — 当前时间范围查询
- [x] `rdb_tsdb_get_stats/reset_stats()` — 运行统计

#### TSDB 内部机制
- [x] 环形缓冲 rotation（扇区满自动切换到下一扇区）
- [x] Epoch 机制（时间戳回绕自动重置时间基准）
- [x] 时间范围查询（跨扇区查询支持）
- [x] 掉电一致性 recovery（扇区状态恢复）

### 构建与测试设施

| 项目 | 状态 | 说明 |
|------|------|------|
| `build.bat` | ✅ 完成 | Windows 批处理编译脚本，输出到 `test\out\` |
| `Makefile` | ✅ 完成 | make 增量编译脚本，`.o` 文件统一到 `test/out/` |
| `test/sim/sim_runner.c` | ✅ 修复 | 路径修复（`test\out\`）、头文件修复（`rocketdb.h`）|
| `test/sim/rocketdb.h` | ✅ 修复 | 指向正确的 `../../rocketdb.h` |
| `test/sim/sim_flash.c` | ✅ 完成 | NOR Flash 模拟器（1→0 写入、故障注入）|
| `test/sim/sim_crypto.c` | ✅ 完成 | CRC16-MODBUS / Hash16 FNV 实现 |
| `test/sim/sim_vectors.c` | ✅ 完成 | 确定性测试向量生成器（LCG PRNG）|
| 测试日志 | ✅ 统一 | 全部输出到 `test/out/`，带时间戳 |

**构建验证状态（2026-02-25）**：`build.bat test` 已可完整编译并运行基础测试套件。

- ✅ **0 warning**，0 error（修复了 `migrate_one` 的 unused parameter warning）
- ✅ KVDB 基础 set/get 通过
- ✅ KVDB GC 压力：100 次 GC 循环，仅用 421 次写入循环
- ✅ TSDB 基础 append/query：200 条记录全部找回
- ✅ KV 测试向量生成：512KB（2000 条）
- ✅ TS 测试向量生成：578KB（2000 条）

**已编译源文件列表**：
```
rocketdb_kvdb.c           KVDB 引擎
rocketdb_tsdb.c           TSDB 引擎
test/sim/sim_flash.c      Flash 模拟器
test/sim/sim_vectors.c    向量生成器
test/sim/sim_crypto.c     CRC / Hash
test/sim/sim_runner.c     测试主程序
```

## 输出目录规划

基于 bitarray 项目经验，计划统一输出目录结构：

```
test/
└── out/                        # 统一输出目录
    ├── sim_test.exe            # 测试可执行文件
    ├── *.o                     # 编译目标文件
    ├── test_log_*.log          # 测试日志（带时间戳）
    ├── test_report_*.md        # 测试报告
    ├── kv_vectors.bin          # KVDB 测试向量
    └── ts_vectors.bin          # TSDB 测试向量
```

### 输出目录用途
- **可执行文件**：`sim_test.exe` - 模拟器测试程序
- **编译文件**：`*.o` - 目标文件，`clean` 时删除
- **测试日志**：`test_log_YYYYMMDD_HHMMSS.log` - 包含完整测试输出和执行时间
- **测试报告**：`test_report_YYYYMMDD_HHMMSS.md` - 结构化测试报告
- **测试向量**：`*.bin` - 可重放的测试向量文件

### 构建命令规划

```bash
# 编译
build.bat              # 或 make

# 测试（自动生成带时间戳的日志）
build.bat test         # 或 make test

# 清理（删除整个 test/out/ 目录）
build.bat clean        # 或 make clean

# 重新编译
build.bat rebuild      # 或 make rebuild
```

### 文件位置规范

| 文件类型 | 当前位置 | 规划位置 | 优势 |
|---------|---------|---------|------|
| 测试报告 | 根目录 | test/out/ | 统一管理、易于清理 |
| 测试向量 | tests/sim/out/ | test/out/ | 统一输出路径 |
| 编译产物 | 分散 | test/out/ | 便于版本控制（.gitignore） |
| 测试日志 | 分散 | test/out/ | CI/CD 友好 |

## 已知设计亮点（来自 design.md）

### KVDB 设计亮点

1. **防死锁兜底**：init 阶段 Phase 4 强制恢复 gc_reserve + 1 个擦除扇区，杜绝"掉电后无可用扇区"的永久死锁。
2. **双副本管理**：每个 key 更新时旧副本自动标记 DEAD，迭代中自动跳过陈旧副本。
3. **iter 代数管理**：迭代器含代数字段，若数据库中途修改返回 RDB_ERR_BUSY。
4. **精确磨损均衡**：四阶段 GC + Phase 4 静态磨损均衡，扇区寿命差异控制在 1000 次以内。
5. **数据安全优先**："先写新再删旧"策略，任何路径不以丢失已有数据为代价。
6. **将旧记录空间计入虚拟垃圾**：`ensure_space` 的 `will_free` 参数使 GC 能更准确评估空间。

### TSDB 设计亮点

1. **Epoch 防回绕**：时间戳单调性保证，回绕自动 epoch++，查询无截断风险。
2. **原生范围查询**：跨扇区查询支持，同时处理已封存和开放扇区。
3. **自动 rotation**：扇区满自动轮转，支持 2~N 扇区灵活配置。
4. **time_delta 使用 uint32_t**：单扇区时间跨度上限约 136 年（秒精度），从根本上消除非预期 rotation。

### 结构体对齐设计（来自 design.md 第三章）

全部 On-Flash 结构体的 32 位字段精确对齐到 4 字节边界：
- `rdb_kv_record_hdr_t.seq` 位于偏移 8（4 字节对齐）
- `rdb_ts_record_hdr_t.time_delta` 位于偏移 4（4 字节对齐）

在 Cortex-M0 等不支持非对齐访问的 MCU 上，可直接将 flash 读缓冲强制转换为结构体指针而不触发 HardFault。

### KVDB 初始化流程详解（Phase 1-4）

**Phase 1：全扇区扫描与分类**
- 检查 magic = 0xFFFFFFFF（三点擦除验证）→ ERASED
- magic = KV_MAGIC 且 hdr_crc 匹配 → 检查 record_seq
  - WRITING + CRC 匹配 → 修复为 VALID
  - WRITING + CRC 不匹配 → 标记为 DEAD
- 其他情况 → CORRUPT
- 恢复 `write_seq`：最大(create_seq, max_record_seq)

**Phase 2：过期副本清理**
- 对每个 VALID 记录查找 latest → 非最新则标记 DEAD
- 重算每个扇区的 garbage_bytes
- 全局重算 live_bytes

**Phase 3：活跃扇区选择**
1. 优先选 create_seq 最大且有余量的扇区（能写入新数据）
2. 次选 create_seq 最大的（满的扇区）
3. 非 active 的 ACTIVE 扇区降级为 SEALED

**Phase 4：安全水位恢复（防死锁兜底）**
```c
if (count_erased(db) < gc_reserve + 1)
    ensure_space(db, 0, 0, 0xFF)  /* 强制一次 GC */
```
- 确保 init 完成后 erased ≥ gc_reserve + 1
- 杜绝"掉电后无可用扇区"导致永久死锁
- GC 失败不阻塞 init（降级为受限模式）

### TSDB 初始化流程详解

**Phase 1：全扇区扫描与环形定位**
- 定位 head_sec（create_seq 最大）和 tail_sec（create_seq 最小）
- 分类：SEALED / ACTIVE / EMPTY / CORRUPT

**Phase 2：环形完整性检验**
- tail → head 的 seq 应严格递增（seqA < seqB 对下一个）
- EMPTY 在环内标记为 data_gaps（容忍，不触发 format）
- seq 非单调标记 data_gaps（容忍，尽最大努力恢复）

**Phase 3：加载 head 扇区状态**
- 如果 head 是 SEALED → head_off = sector_size（下次 append 触发 rotate）
- 如果 head 是 ACTIVE → 扫描修复 WRITING 记录（CRC 校验）
- 扫描获取 last_time

**Phase 4：总计数校准**
- 从 tail 到 head 累加每个扇区的记录数（使用 ts_sector_count 处理降级 ACTIVE）

**Phase 5：last_time 兜底扫描**
- 若 last_time == 0 且 total_count > 0 → 全部扇区扫描取最大时间

### 掉电恢复表（关键路径分析）

**KVDB 掉电恢复**：

| 掉电时刻 | Flash 残留 | init 行为 | 数据完整性 |
|----------|-----------|----------|----------|
| 写 record_hdr 中 | header 不完整 | magic 不匹配 → corrupt_skip | ✅ 无损 |
| 写 key/val 中 | header 完整, data 部分 | WRITING+CRC不匹配 → DEAD | ✅ 无损 |
| 写 state=VALID 中 | header+data 完整 | WRITING+CRC匹配 → VALID | ✅ 无损 |
| 作废旧记录中 | 新VALID, 旧部分作废 | fixup_stale → DEAD | ✅ 无损 |
| GC 迁移中(源未DEAD) | 目标VALID, 源VALID | fixup 保留 seq 大者 | ✅ 无损 |
| GC 擦除中 | 擦除不完整 | 三点检验 → CORRUPT → 恢复 | ✅ 无损 |
| GC fixup 中 | 部分旧副本未清理 | 下次 init fixup 完成 | ✅ 无损 |

**TSDB 掉电恢复**：

| 掉电时刻 | init 行为 | 影响 |
|----------|----------|------|
| 写 time_base 中 | 非法值 → head_count=0, 重写 | ✅ 无损 |
| seal: 写 count 后 | end_off=0xFFFF → ACTIVE（降级）| ✅ 记录保留，header 由 query 补全 |
| seal: 写 end_off 后 | hdr_crc=0xFFFF → ACTIVE（降级）| ✅ 同上 |
| seal: 写 crc 后 | SEALED 正常 | ✅ 无损 |
| rotate: erase 完 | 新扇区 EMPTY → data_gaps++ | ✅ 最多跳过一个 rotation 周期 |

### 扫描优化策略（来自 design.md 4.5 节）

**scan_sector 的 update_woff 策略**：
- 只有 init Phase 1 传 TRUE（需恢复 write_off）
- 其余所有调用点（find_latest、fixup、garbage 统计、GC 迁移）均传 FALSE
- 保证只读操作路径不产生副作用

**损坏记录跳过策略**：
- 遇到不合法 record header 时跳过 16 字节（一个完整记录头宽度）
- 避免逐字节爬行（穿越损坏区域需数百次无效读取）
- 降低虚假解析风险（步长过小可能对齐到记录中间）

---

| 文件 | 说明 | 当前位置 | 规划位置 |
|------|------|---------|----------|
| `design.md` | 完整的设计文档（1391 行） | 根目录 | 保持 |
| `test_plan.md` | 测试计划与覆盖分析 | 根目录 | 保持 |
| `test_request.txt` | 原始需求文本 | 根目录 | 保持 |
| 测试报告 | 多份历史报告 | 根目录（分散） | **→ test/out/** |

## 下一步实现目标

### 立即（项目结构优化）
1. **统一输出目录**：创建 `test/out/` 目录，迁移所有编译产物和测试输出。
2. **创建构建脚本**：编写 `build.bat` 和 `Makefile`，参考 bitarray 项目。
3. **统一测试日志格式**：所有测试输出到 `test/out/test_log_YYYYMMDD_HHMMSS.log`。
4. **更新测试文档**：在 tests/sim/README.md 中说明新的输出路径。

### 短期（v0.0.2 完善）
1. **补齐自动化测试**：构建 executable test suite，覆盖所有 Phase recovery 场景。
2. **写入粒度矩阵测试**：验证 1/2/4/8 字节粒度下的正确性。
3. **压力测试**：每个分区 ≥100 GC 循环，混合 key/value 长度。
4. **掉电注入测试**：在关键步骤（header、data、commit）模拟掉电，验证 recovery。

### 中期（可靠性验证）
1. **CRC 错误注入**：损坏数据后的隔离与恢复。
2. **随机序列长工作**：连续 run，监控 erase count 分布。
3. **epoch reset 验证**：TSDB 时间戳回绕的正确处理。
4. **iterator 并发验证**：迭代期间修改触发 RDB_ERR_BUSY。

### 长期（性能与扩展）
1. **性能基准建立**：读/写/GC 延迟分布。
2. **大分区支持**：验证 1MB+ 分区的表现。
3. **多分区并行**：多个 KVDB/TSDB 分并行操作的表现。

## 已知局限与风险

### KVDB
1. **无索引加速**：key 查找基于线性扫描，超大数据集（>10K key）性能下降；可考虑 B-tree 索引优化。
2. **GC 延迟变异**：Phase 0~2 通常快速，Phase 4（时隔 GC 均衡）可能需要全区扫描；建议在非关键路径触发。
3. **副本数受限**：single-key-multiple-version 设计中，同一 key 最多保留 1 个 VALID + N 个 DEAD；无版本历史保留。

### TSDB
1. **环形覆盖无告警**：新数据覆盖最旧数据时无明确通知，应用需自行管理保留周期。
2. **大记录磨损**：单条记录接近扇区容量时，垃圾率快速上升，可考虑跨扇区大记录支持。
3. **query 缓冲限制**：range query 结果缓冲由应用提供，缓冲过小时结果截断；无分页游标支持。

## 测试覆盖现状

| 场景 | 覆盖率 | 备注 |
|------|--------|------|
| 基本 set/get/delete | ✅ 80% | 缺随机长度混合 |
| 单分区 GC | ⚠️ 50% | 缺 Phase 4 验证 |
| Recovery | ⚠️ 40% | 缺掉电中断点全覆盖 |
| TSDB append/query | ✅ 70% | 缺 epoch 回绕验证 |
| 多分区协作 | ❌ 0% | 未测 |
| 磨损均衡验证 | ❌ 0% | 需专项测试 |
| CRC 错误注入 | ❌ 0% | 未测 |
| 线程安全 | ❌ 0% | 无并发测试 |

## 注释与可读性

- [x] 核心函数含详细流程注释
- [x] On-Flash 结构体字段加字节偏移标注
- [x] 错误路径注释清晰
- [ ] 某些复杂 GC 阶段逻辑可增强注释
- [ ] phase 4（磨损均衡）逻辑建议补充伪代码

