# RocketDB 旧 CODE_REVIEW 整合核查报告

**日期**: 2026-05-17  
**范围**: `docs/CODE_REVIEW_*.md` 共 8 份历史审查文件  
**目标**: 去重整合历史缺陷，按当前源码/测试重新核查修复状态，并记录验证结果。

---

## 1. 输入文件

| 文件 | 主要内容 | 当前处理 |
|------|----------|----------|
| `CODE_REVIEW_251219_105822.md` | 早期架构级风险、GC/TSDB/锁/文档问题 | 已并入主题台账 |
| `CODE_REVIEW_260429_014849.md` | 第一轮完整缺陷清单，含 C/H/M 项与多阶段修复记录 | 已并入主题台账 |
| `CODE_REVIEW_260429_145501.md` | 版本、TSDB format、README/W25QXX/示例问题 | 已并入主题台账 |
| `CODE_REVIEW_260430_093332.md` | 版本标签、NULL erase_cnts、格式化校验等回归项 | 已并入主题台账 |
| `CODE_REVIEW_260430_110423.md` | 源码、测试、文档、构建系统二次审查 | 已并入主题台账 |
| `CODE_REVIEW_260430_115301.md` | 上一轮剩余缺陷复核 | 已并入主题台账 |
| `CODE_REVIEW_260430_142015.md` | 构建/测试基础设施、Makefile、日志、fault import | 已并入主题台账 |
| `CODE_REVIEW_260505_130821.md` | 最新旧报告，21 项缺陷与修复记录 | 已并入主题台账 |

旧报告保留为历史审计材料；本文件作为当前活跃汇总索引。

---

## 2. 核查结论

### 2.1 已验证修复

| 主题 | 覆盖的历史问题 | 当前证据 |
|------|----------------|----------|
| 位宽与大扇区 | KVDB iterator offset 截断、`write_off/head_off/head_seq` 位宽、`RDB_SEQ_INVALID` 冲突 | `rdb_kv_iter_t.offset`、KV/TS RAM 偏移为 `uint32_t`；全量测试通过 |
| NOR 安全提交失败处理 | `writing_cb`/`migrate_one`/`set` commit 失败复用地址、`mark_dead` 失败双副本 | 失败后推进写前沿或标记 DEAD；故障注入测试覆盖 |
| GC 原子性和迭代器失效 | victim 迁移前擦除、GC 后 `iter_gen` 未递增、zero-live erase 未失效迭代器 | `gc_execute` 迁移成功后擦除，擦除后递增 `iter_gen` |
| TSDB 查询无副作用 | `ts_scan` 查询路径修复 WRITING、跨 epoch 查询、降级 ACTIVE 扇区查询 | 查询路径只读，`query/latest/oldest/time_range` 处理降级 ACTIVE |
| TSDB seal/rotate 错误处理 | `ts_seal()` 返回值忽略、rotate 失败破坏 ring、seal CRC 降级 | seal 错误返回，失败路径保持可恢复状态 |
| 故障模型 | power-loss 无部分写、READ_FAIL/BIT_FLIP 未测试、fault tests 永真 | 字节级部分写、READ_FAIL/BIT_FLIP 测试、故障用例含断言 |
| 构建系统 | Makefile 平台语法、`-lm`、Windows batch build 动作、CMake/CTest 注册 | `run_all_tests.bat`、CMake/CTest、perf runner 均可执行 |
| W25QXX/HAL 文档 | 缺 W25QXX 指南、页编程边界、命令名、路径错误 | `W25QXX_GUIDE.md`/`HAL_REFERENCE.md` 已更新，HAL 示例按 256B 页拆分 |
| 大记录写入 | TSDB 大 data 单次 HAL 写入、W25QXX 页编程不友好 | `rdb_tsdb_append` 大 payload 按 `RDB_STACK_BUF_SIZE` 分块；新增回归测试 |
| 缓冲区不足语义 | TSDB `get_latest/get_oldest` 静默截断 | 当前返回 `RDB_ERR_TOO_LARGE`，仍通过 `out_len` 返回实际长度 |

### 2.2 本轮补修

| 项 | 历史来源 | 本轮修复 |
|----|----------|----------|
| `fault_import_rules()` 导入计数不完整 | H7 / BUG-P3-03 | 修复 `fault_add_rule() >= 0` 判断，避免只统计第 1 条规则；改用临时 int 避免 enum 强转别名风险 |
| fault rule 导入缺少回归 | H7 / BUG-P3-03 | 新增 `fault_rule_import_roundtrip` 测试 |
| integration PRNG 跨测试泄漏 | M4 | 每个相关测试入口重置固定 seed |
| wear_heatmap 永真断言 | M3 | 增加目标达成、erase sum、wear spread 上限断言 |
| GC 评分/cache 循环无 yield | M6 / H-13 | `gc_build_cache` 长循环中周期性调用 `fl_yield()` |
| TSDB latest/oldest 截断 | H-11 | 小缓冲返回 `RDB_ERR_TOO_LARGE`，新增测试 |

### 2.3 误报或设计接受项

| 项 | 状态 | 说明 |
|----|------|------|
| TSDB epoch 查询边界 C-8 | 误报 | 当前设计允许跨 epoch 查询返回时间范围内的新旧 epoch 记录，文档已说明 |
| `rdb_dedup_slot_t.prefix[8]` | 设计权衡 | 前缀冲突只影响性能，正确性由 fallback 验证 |
| KV sector header CRC 不覆盖全部字段 | 设计权衡 | 历史记录已沉淀为限制项，erase count 使用 RAM/Flash max 策略 |
| `is_erased()` 三点探测 | 已知限制 | 文档列为启发式检测，不作为完整擦除证明 |
| `test_make_log_path()` static buffer | 测试框架限制 | 测试 runner 单线程使用；已在文档/注释中说明 |
| `rdb_version_str()` / `rdb_err_str()` 缺失 | 非当前 API | 旧设计文档遗留接口，不作为缺陷处理 |

### 2.4 当前保留限制

| 项 | 状态 | 原因/建议 |
|----|------|-----------|
| 只读 API 未全部改为 `const rdb_*db_t*` | 保留 | 属于 ABI/API 破坏性变更；当前公共 API 维持兼容 |
| TSDB `get_latest/get_oldest` CRC 与复制仍双读 | 保留 | 性能优化项，非正确性缺陷 |
| `sim_flash` 允许单字节状态写绕过 `write_gran` | 保留并需移植注意 | 当前 on-flash 状态机依赖单字节 WRITING→VALID/DEAD 提交，适合 W25QXX byte program；严格 4/8B 内部 Flash 移植需重新审视提交写协议 |
| `RDB_MIN/RDB_MAX` 宏双重求值 | 保留 | 当前调用点均为无副作用表达式；后续可通过内联函数族替换 |
| `test_framework` 使用 `clock()` | 保留 | 仅影响测试耗时统计精度，不影响功能断言 |
| SimFlash trace before-image 512B 上限 | 保留 | 当前引擎写入已分块，默认配置下不会超过 512B；自定义超大 `RDB_STACK_BUF_SIZE` 时需同步调整 |

---

## 3. 验证结果

### 3.1 自动化测试

命令：

```bat
build\run_all_tests.bat test
```

结果：

| 测试套件 | 用例数 | 断言数 | 结果 |
|----------|--------|--------|------|
| `test_kvdb_basic` | 11 | 2,976 | PASS |
| `test_kvdb_stress` | 6 | 4,686 | PASS |
| `test_tsdb_basic` | 6 | 2,864 | PASS |
| `test_tsdb_stress` | 5 | 2,749 | PASS |
| `test_integration` | 6 | 25,535 | PASS |
| `test_fault_injection` | 8 | 74 | PASS |
| `test_example` | 2 | 27 | PASS |
| **总计** | **44** | **38,911** | **PASS** |

日志：

- `test/out/20260517_161928_SUMMARY.log`
- `test/out/20260517_161932_test_tsdb_basic.log`
- `test/out/20260517_161935_test_integration.log`
- `test/out/20260517_161937_test_fault_injection.log`

### 3.2 结论

历史 `CODE_REVIEW` 中的功能性、数据完整性、掉电恢复、构建阻塞类缺陷均已修复并通过测试验证。仍保留的项目均为 API 兼容性、性能优化、测试框架限制或移植约束，已在上表明确列出，不再视为当前发布阻塞项。

