# Plan（计划）

## 实现路线图

目标：从 v0.0.2 当前状态演进至完全验证的生产级驱动。

---

## 第零阶段：项目结构优化（基于 bitarray 经验，0.5~1 周）

**目标**：统一项目结构，建立标准化构建和测试流程。

### 任务
1. **统一输出目录**
   - 创建 `tests/out/` 目录
   - 迁移所有编译产物、可执行文件、测试日志到统一目录
   - 更新 `.gitignore` 排除 `tests/out/`

2. **创建构建脚本**
   - 编写 `build.bat`（Windows），参考 bitarray 项目
   - 编写 `Makefile`（Unix/Linux），参考 bitarray 项目
   - 支持命令：build, test, clean, rebuild
   - 测试日志自动生成时间戳：`test_log_YYYYMMDD_HHMMSS.log`

3. **更新测试文档**
   - 更新 `tests/sim/README.md` 说明新的输出路径
   - 统一测试报告格式和位置

4. **清理历史文件**
   - 迁移根目录的历史测试报告到 `tests/out/`
   - 清理分散的临时文件

**交付物**
- [x] 学习 bitarray 项目结构和测试方法（已完成，2026-02-25）
- [x] 更新 SpecKit 文档以反映输出目录规划（已完成，2026-02-25）
- [ ] 创建 tests/out/ 目录
- [ ] 编写 build.bat 和 Makefile
- [ ] 迁移历史测试报告
- [ ] 更新 tests/sim/README.md

**预期效果**
- ✅ 所有输出文件统一管理，便于版本控制
- ✅ 标准化的构建和测试流程
- ✅ CI/CD 友好的目录结构
- ✅ 简化的清理命令（删除整个 tests/out/）

---

## 第一阶段：基线规格固化（1~2 周）

**目标**：确保规格文档、API 签名、错误码与头文件一致。

### 任务
1. **API 规格锁定**
   - 审视 `specify.md` 中的所有 API 声明
   - 对照 `rocketdb.h` 逐行校验函数签名、参数、返回值
   - 确保参数列表、内存布局约束清晰记录

2. **错误码语义明确**
   - 每个错误码限制为一个清晰的语义（无重载）
   - 文档标注各 API 可返回的错误码集合
   - 验证无"错误码含义随函数变化"的歧义

3. **On-Flash 结构体布局校验**
   - 所有结构体大小通过编译时断言检查
   - 字段对齐手工验证（特别是 Cortex-M0 双字对齐）
   - 编写对齐检查工具生成对齐报告

4. **外部接口明确**
   - `crc16()`, `crc16_cont()`, `hash16()` 语义定义
   - Flash ops 各回调的入参、出参、副作用明确

**交付物**
- [ ] `specify.md` 锁定版
- [ ] API 及错误码对齐报告
- [ ] 结构体对齐验证报告

---

## 第二阶段：自动化测试框架建设（2~3 周）

**目标**：建立可重复、可度量的测试基础设施（基于 test_plan.md 第 4 节）。

### 任务
1. **Flash 模拟器完善**（test_plan.md 4.1）
   - 实现 W25Q128 模型（sector_size=4KB, page_size=256B）
   - 支持可配置的写入粒度（1/2/4/8 字节）
   - 支持可配置的故障注入（erase 失败、CRC 损坏、掉电中断）
   - 1→0 only writes unless erased

2. **确定性随机生成**（test_plan.md 4.2.1）
   - 使用可重现 PRNG（seed 日志）
   - Key 长度分布：50% 短(1-8B)，30% 中(9-24B)，15% 长(25-31B)，5% 最大(32B)
   - Value 长度分布：40% 小(0-32B)，40% 中(33-256B)，15% 大(257-1024B)，5% 最大
   - 操作混合：60% insert，25% update，10% get，5% delete
   - 时间戳生成：70% 递增，20% 倒序(测试纠正)，10% epoch-reset

3. **测试日志与对比**（test_plan.md 4.1）
   - 每次测试输出结构化日志（trace logging）
   - 日志包含：
     - 扇区状态、write offsets、live/garbage bytes
     - GC victim 选择详情
     - erase counts per sector
     - TSDB head/tail positions, time_base, total_count
   - 输出到 `tests/out/test_log_YYYYMMDD_HHMMSS.log`

4. **测试用例执行器**（test_plan.md 4.2.2）
   - 20 个具体测试用例（TC-KV-01~09, TC-TS-01~07, TC-X-01~04）
   - 全局测试矩阵：
     - Write granularity: 1, 2, 4, 8 bytes
     - Partitions: KVDB1, KVDB2, TSDB1, TSDB2 (32KB 各)
     - Payload size mix: small (≤16B), medium (32-256B), large (接近扇区容量)

5. **CI 集成预备**
   - 编写简易测试驱动脚本，可从 shell/PowerShell 调用
   - 定义测试通过/失败条件
   - 日志格式便于自动化解析

**交付物**
- [ ] `tests/sim/` 下完整的模拟 Flash 硬件驱动
- [ ] 可配置的随机序列生成工具
- [ ] 测试框架 skeleton（main 循环、参数传递、结果收集）

---

## 第三阶段：覆盖率导航测试（3~4 周）

**目标**：建立最小必要覆盖，覆盖所有主要代码路径和故障场景。

### KVDB 测试矩阵

#### 3.1 基础操作测试
```
set:
  - 新 key 插入
  - 已存 key 更新（产生 DEAD 记录）
  - 超大 value (MAX_VAL_LEN)
  - 分布式长度（1B~MAX, 10B 间隔采样）

get:
  - 存在 key
  - 不存在 key
  - value 缓冲过小（验证 out_len 仍返回实际长度）
  - CRC 损坏后的处理（错误拒绝）

delete:
  - 存在 key 删除
  - 不存在 key 删除
  - 删除后重新写入

exists:
  - 返回值中 RDB_TRUE/RDB_FALSE 一致
```

#### 3.2 GC 阶段覆盖
```
Phase 0（zero-live）:
  - 扇区所有记录都删除 → garbage_bytes == sector 容量
  
Phase 1（low-live）:
  - 垃圾率 > 20% 但 < 50%
  
Phase 2（wear）:
  - 多个扇区达到均衡磨损条件
  
Phase 3（oldest）:
  - 所有扇区都在磨损范围内，选最老扇区

Phase 4（安全水位）:
  - 初始化后 erased < gc_reserve + 1 的恢复
  - 时隔 >= GC_WEAR_THRESHOLD 的冷扇区擦除
```

#### 3.3 Recovery 与掉电场景
```
Recovery Phase 1（清理过期副本）:
  - 多版本 key 的最新版保留，旧版标 DEAD
  
Recovery Phase 2（垃圾统计）:
  - garbage_bytes 准确重算
  
Recovery Phase 3（活跃扇区选择）:
  - 多候选时选 create_seq 最大
  - 多满扇区时 rotate 分配新区
  
Recovery Phase 4（安全水位）:
  - erased == 0 时强制擦除一个扇区
```

#### 3.4 写入粒度测试
对每个粒度 write_gran ∈ {0, 1, 2, 3}（1B/2B/4B/8B）:
```
- 单条记录大小跨粒度边界
- 填充验证（0xFF 填充正确性）
- CRC 计算（原始数据，不含填充）
```

### TSDB 测试矩阵

#### 3.5 追加与查询
```
append:
  - 大小 1B~MAX_TS_DATA_LEN 的数据
  - 单调递增时间戳
  - 时间戳带回绕
  - 单扇区填满后自动 rotation

rdb_tsdb_query / rdb_tsdb_query_ex:
  - [from, to] 查询内的所有记录
  - 单扇区查询
  - 跨扇区查询（含已封存和开放扇区）
  - from > to（边界）
  - from == to（单点查询）
```

#### 3.6 Epoch 管理
```
- 初始化第一条记录后 time_base 写入
- 时间戳回绕（ts_n < ts_n-1）触发 epoch++
- 查询跨多个 epoch 的数据
```

#### 3.7 Recovery
```
- 扇区头部分写入的恢复（magic/erase_cnt vs. time_base）
- 某扇区损坏的隔离，其他扇区正常查询
```

**交付物**
- [ ] KVDB 测试套件（20+ 个测试用例）
- [ ] TSDB 测试套件（15+ 个测试用例）
- [ ] 测试报告：用例覆盖矩阵、通过率、代码行覆盖统计

---

## 第四阶段：压力与耐久性测试（4~5 周）

**目标**：验证在长期运行、边界条件下的可靠性。

### 任务

#### 4.1 GC 循环压力
```
每个分区（KVDB1, KVDB2, TSDB1, TSDB2）触发 >= 100 GC 循环：
- KVDB: 持续 set/delete，监控 erase_cnt 分布
- TSDB: 持续 append，监控 erase_cnt 分布
```

#### 4.2 混合工作负载
```
KVDB:
  - key 长度随机：1~32 字节，均匀分布
  - value 长度分布：
    * 40%: 0~32B
    * 40%: 33~256B
    * 15%: 257~1024B
    * 5%: 1025~RDB_MAX_VAL_LEN

TSDB:
  - 数据长度同上分布
  - 时间戳间隔：均匀或泊松分布，可配
```

#### 4.3 掉电注入
在关键路径上注入掉电：
```
KVDB set():
  - hdr 写入前中断
  - hdr 写入后、data 前
  - data 写入中
  - commit 写入中
  恢复后验证 key 要么未写入，要么完整不损坏

TSDB append():
  - 类似上述中断点
  - 特别验证 time_base、rotation 的一致性
```

#### 4.4 磨损均衡验证
```
- 运行完全测试后统计各扇区 erase_cnt
- 验证 max - min <= RDB_GC_WEAR_THRESHOLD
- 打印磨损热力图（扇区 vs. erase_cnt）
```

**交付物**
- [ ] 压力测试脚本与参数集
- [ ] 掉电注入工具与中断点清单
- [ ] 压力测试报告：100+ GC 循环验证、磨损分布统计

---

## 第五阶段：质量门禁与文档完善（5~6 周）

**目标**：确保代码质量、文档完整性、CI 可集成。

### 任务

#### 5.1 代码质量检查
- [ ] 静态分析（如 cppcheck）无重大告警
- [ ] 所有 error path 有日志或诊断信息
- [ ] Magic number 和约束尽数添加注释

#### 5.2 文档完善
- [ ] 更新 `clarify.md`（待确认问题及最终决策）
- [ ] 更新 `analyze.md`（功能覆盖、已知缺口）
- [ ] 编写 README.md（快速开始、完整示例）
- [ ] 编写 TROUBLESHOOTING.md（常见故障排查）

#### 5.3 CI 友好性
- [ ] Makefile/build.bat 输出标准化
- [ ] 日志格式便于自动化对比（支持 diff）
- [ ] 返回码：0=全通过，非 0=失败（便于 CI 脚本检测）

#### 5.4 示例与参考
- [ ] 编写完整示例程序（KVDB + TSDB 并用）
- [ ] 编写错误处理示例（各错误码的应对）
- [ ] 编写 Flash 硬件接口实现示例（基于 STM32）

**交付物**
- [ ] 完整的 SpecKit 文档集（8 个 .md 文件）
- [ ] 代码质量报告
- [ ] 集成示例与参考实现

---

## 第六阶段：性能基准与优化建议（6~7 周）

**目标**：建立性能基准，提出未来优化方向。

### 任务
1. **读/写/GC 延迟分布**
   - 无 GC 情况下单次 set/get 延迟
   - GC 触发时的延迟分布（p50/p95/p99）
   - yield() 调用频率

2. **扇区生命周期分析**
   - 从新建到擦除的演化过程
   - 垃圾率变化曲线
   - Phase 4（磨损均衡）触发频率

3. **优化建议列表**
   - 是否需要 B-tree 索引加速查询？
   - 跨扇区大记录支持的可行性
   - 增量 GC 与批处理 GC 的权衡

**交付物**
- [ ] 性能测试报告（图表、数据表）
- [ ] 优化建议白皮书

---

## 时间与资源计划

| 阶段 | 工期 | 关键路径 |
|------|------|---------|
| 第 1 阶段 | 1~2 周 | API 规格锁定 |
| 第 2 阶段 | 2~3 周 | 测试框架搭建 |
| 第 3 阶段 | 3~4 周 | 核心覆盖测试 |
| 第 4 阶段 | 4~5 周 | 耐久性验证 |
| 第 5 阶段 | 5~6 周 | 质量门禁、文档完成 |
| 第 6 阶段 | 6~7 周 | 性能分析、优化建议 |

**总计**：6~7 周完整验证；可根据资源状况调整。

## 版本标签

- **v0.0.2**：当前基线（API 定型、核心功能完成）
- **v0.1.0**：第 1~3 阶段完成（基线规格 + 覆盖测试通过）
- **v0.2.0**：第 4~5 阶段完成（压力测试 + 质量门禁）
- **v1.0.0**：第 6 阶段完成（生产级，性能基准建立）

