# RocketDB SpecKit — 项目文档体系

## 概览

RocketDB 是针对资源受限嵌入式系统的**双模 Flash 存储引擎**，提供 KVDB（键值存储）和 TSDB（时序存储）两个核心引擎。

本 SpecKit 是 RocketDB 从早期版本演进至当前生产级基线的项目管理体系，包括规格定义、实现现状、分阶段计划、任务跟踪、问题澄清、功能分析。

> 当前构建、测试和离线分析状态以 `docs/architecture/ARCHITECTURE.md`、`docs/architecture/TEST_PLAN.md`、`tests/sim/README.md`、`tests/perf/README.md` 和 `docs/architecture/OFFLINE_ANALYSIS.md` 为准。本目录保留项目演进过程中的规格化材料，并已同步关键路径与当前工具链。

### 文档来源整合（2026-02-25 更新）

本 SpecKit 已整合以下核心文档内容：

1. **design.md**（1391 行完整设计手册）
   - ➡️ `specify.md`：API 规格、编译配置、错误码、On-Flash 结构
   - ➡️ `implement.md`：设计亮点、结构体对齐、扫描优化策略
   - ➡️ `clarify.md`：关键设计决策（write_seq 恢复、损坏记录跳过、数据安全原则等）

2. **test_plan.md**（完整测试计划）
   - ➡️ `analyze.md`：覆盖率评估、缺口列表、测试用例映射
   - ➡️ `plan.md`：测试框架建设策略、数据分布定义
   - ➡️ `tasks.md`：具体测试任务（TC-KV-01~09, TC-TS-01~07 等）

3. **test_request.txt**（原始测试需求）
   - ➡️ `constitution.md`：测试基准配置、GC 压力要求、数据分布要求
   - ➡️ `clarify.md`：测试要求澄清（混合长度、GC 次数、写入粒度）

所有核心设计理念、测试策略和实现细节已完整融入 SpecKit 体系，确保文档一致性。

---

## 文档导航

### 核心文档（必读）

| 文档 | 用途 | 读者 | 长度 |
|------|------|------|------|
| [Constitution（宪法）](constitution.md) | 定义不可变的核心目标、根本约束、设计边界 | 架构师、决策者 | ~150行 |
| [Specify（规格）](specify.md) | 公共 API、错误码、编译配置、Flash 抽象层 | 应用开发者、API 使用者 | ~300行 |
| [Implement（实现）](implement.md) | 当前 v0.0.2 的实现现状、已知亮点、下一步目标 | 代码维护者、测试工程师 | ~250行 |

### 规划文档（项目管理）

| 文档 | 用途 | 读者 | 长度 |
|------|------|------|------|
| [Plan（计划）](plan.md) | 从 v0.0.2 到 v1.0.0 的分阶段路线图（6 阶段, 6~7 周） | 项目经理、技术负责人 | ~400行 |
| [Tasks（任务）](tasks.md) | 具体可执行的任务清单、依赖关系、工作量估算 | 任务分配者、执行者 | ~300行 |

### 深入分析文档

| 文档 | 用途 | 读者 | 长度 |
|------|------|------|------|
| [Clarify（澄清）](clarify.md) | 设计中的开放性问题（11 个）及团队决策记录 | 架构师、高级开发者 | ~200行 |
| [Analyze（分析）](analyze.md) | 功能覆盖率、代码路径分析、与参考实现对比、改进建议 | 代码审查者、架构优化 | ~350行 |

---

## 快速导读

### 我是项目经理

👉 按以下顺序阅读：
1. [Constitution](constitution.md) — 5 分钟了解项目范围
2. [Plan](plan.md) — 了解 6 阶段路线图和关键里程碑
3. [Tasks](tasks.md) — 了解当前关键路径和优先级

### 我是应用开发者（RocketDB 用户）

👉 按以下顺序阅读：
1. [Specify](specify.md) — **必读**，API 参考
2. [Constitution](constitution.md) — 理解设计约束
3. [Architecture.md](../Architecture.md) — 完整技术细节（可选深读）

### 我是代码维护者 / 充分测试工程师

👉 按以下顺序阅读：
1. [Implement](implement.md) — 了解当前实现状态和已知风险
2. [Analyze](analyze.md) — 功能覆盖分析、缺口识别
3. [Tasks](tasks.md) — 了解测试任务优先级
4. [Clarify](clarify.md) — 了解设计决策背景

### 我是架构师 / 代码审查者

👉 按以下顺序阅读：
1. [Constitution](constitution.md) — 核心约束与设计目标
2. [Clarify](clarify.md) — 理解 11 个关键决策
3. [Analyze](analyze.md) — 功能完整性、风险评估
4. [Plan](plan.md) — 验证优化方向是否合理

---

## 关键信息速查

### 核心特性（From Constitution）
- ✅ **零动态内存**：全缓冲由调用者提供
- ✅ **掉电一致性**：二次写 (WRITING → VALID → DEAD)
- ✅ **四阶段 GC + Phase 4 磨损均衡**：KVDB 扇区寿命均衡 ≤1000 次
- ✅ **自动 epoch 管理**：TSDB 时间戳回绕防护
- ✅ **结构体自然对齐**：Cortex-M0 HardFault 防护

### 编译时配置（From Specify）
| 配置 | 默认值 | 说明 |
|------|--------|------|
| RDB_MAX_KEY_LEN | 32 | KVDB key 最大长度 |
| RDB_MAX_VAL_LEN | 4095 | KVDB value 最大长度 |
| RDB_GC_GARBAGE_PCT | 20 | 垃圾率触发 GC 阈值 |
| RDB_GC_WEAR_THRESHOLD | 100 | Phase 4 磨损均衡阈值 |
| RDB_MIN_SECTOR_SIZE | 4096 | 最小扇区大小 |

### 实现现状（From Implement）
| 模块 | 状态 | 覆盖 |
|------|------|------|
| KVDB 核心 API | ✅ 完成 | set/get/delete/gc/iter |
| KVDB 四阶段 GC | ✅ 完成 | Phase 0~4 逻辑完整 |
| KVDB Recovery | ✅ 完成 | Phase 1~4 恢复 |
| TSDB 核心 API | ✅ 完成 | append/query/rotation |
| TSDB Epoch | ✅ 完成 | 时间戳回绕防护 |
| 构建脚本 | ✅ 完成 | bat / Makefile / CMake 均可用 |
| 输出目录 | ✅ 完成 | 可控产物统一到 `tests/out/` |
| 自动化测试 | ✅ 完成 | 8 个基础套件 + CTest + rdbdump |
| 压力测试 | ✅ 完成 | GC/rotation ≥100 覆盖 |

### 已知缺口（From Analyze）
| 优先级 | 缺口 | 计划修补 |
|--------|------|---------|
| P1 | GC Phase 4 效果未验证 | v0.1.0（压力测试） |
| P1 | 跨 epoch 查询边界 | v0.1.0（完整测试） |
| P2 | 无查询索引加速 | v0.2.0（可选 B-tree） |
| P2 | 无分页游标支持 | v0.2.0（可选分页） |
| P2 | 诊断信息不足 | v0.1.0（增强 debug API） |

### 关键决策（From Clarify）
| 问题 | 决策 | 原因 |
|------|------|------|
| iter 期间是否允许修改 | **禁止**（严格 lock） | 单任务嵌入式环境最安全 |
| 是否跨扇区大记录 | **否**（单扇区限制） | 典型工况不需要，GC 简洁 |
| 时间戳回绕处理 | **自动 epoch++** | 防护优先 |
| 是否支持向后兼容 | **否** | v0.0.2 已充分改进，不值得支持迁移 |
| CRC 错误时隔离 | **仅报告** | 故障可见性优先 |

### 开发路线（From Plan）
| 阶段 | 工期 | 交付物 |
|------|------|--------|
| 1 | 1~2 周 | API 规格锁定 + 对齐报告 |
| 2 | 2~3 周 | 测试框架 + Flash 模拟器 |
| 3 | 3~4 周 | 核心覆盖测试（20+ 用例） |
| 4 | 4~5 周 | 压力/掉电/磨损测试 |
| 5 | 5~6 周 | 质量门禁 + 完整文档 |
| 6 | 6~7 周 | 性能基准 + 优化建议 |

---

## 关键概念

### KVDB 设计

**日志结构存储（Log-Structured）**
- 新记录追加到活跃扇区末尾
- 旧记录标记为 DEAD（不覆盖自身，遵循 1→0 原理）
- GC 周期性清理 DEAD 记录，回收空间

**四阶段评分 GC**
```
Phase 0: garbage_bytes == sector 容量（全删）→ 直接擦除
Phase 1: 垃圾率 > 20% → 迁移有效 + 擦除
Phase 2: 磨损均衡（erase_cnt 最小） → 迁移 + 擦除
Phase 3: 最老扇区（create_seq 最小） → 迁移 + 擦除
Phase 4: 冷扇区均衡（时隔 >= 1000 次 GC 的扇区主动擦除）
```

**掉电一致性（Two-Phase Write）**
```
Step 1: 写入记录为 WRITING 状态 (0xFF) + 数据
Step 2: CRC 校验通过
Step 3: 提交为 VALID 状态 (0xFE) ← 此处断电仍安全
Step 4: 旧版本标记 DEAD (0xFC) ← 可选
```

### TSDB 设计

**环形缓冲（Ring Buffer）**
- 追加新数据到 head (写位置)
- 最旧数据位置为 tail (读位置)
- 满后自动 rotation 到下一扇区
- 最旧扇区的数据通常被覆盖（自动淘汰）

**Epoch 管理（防时间戳回绕）**
```
时间戳单调性检查:
  if (ts_n < ts_n-1)  // 回绕
    epoch++           // 时间基准重置
```

---

## 文件结构

```
rocketdb/
├── src/
│   ├── rocketdb.h          — 公共类型、API 声明
│   ├── rocketdb_kvdb.c     — KVDB 引擎实现
│   └── rocketdb_tsdb.c     — TSDB 引擎实现
├── docs/
│   ├── Architecture.md     — 完整设计文档
│   ├── test_plan.md        — 测试计划
│   ├── offline_flash_analysis.md — 离线 Flash dump 分析
│   └── SpecKit/            — 项目规范体系 (本目录)
│   ├── SpecKit.md          — 本文档（导航）
│   ├── constitution.md     — 宪法（不可变约束）
│   ├── specify.md          — 规格（API 定义）
│   ├── implement.md        — 实现（当前状态）
│   ├── plan.md             — 计划（阶段路线）
│   ├── tasks.md            — 任务（执行清单）
│   ├── clarify.md          — 澄清（问题与决策）
│   └── analyze.md          — 分析（覆盖与缺口）
├── tests/
│   ├── out/                — 测试、构建、性能、rdbdump 输出
│   ├── sim/                — Flash 模拟器与测试
│   │   ├── sim_flash.h/.c
│   │   ├── test_*.c
│   │   └── ...
│   └── perf/               — 性能基准
├── tools/rdbdump/          — 离线 Flash dump 解析工具
├── build/                  — Windows 批处理入口
└── Makefile / CMakeLists.txt
```

---

## 版本与发布计划

| 版本 | 预计完成 | 主要目标 |
|------|----------|---------|
| v0.0.2 | 已完成 | 功能核心完整，API 定型 |
| v0.1.0 | 7 周 | 基线规格 + 覆盖测试通过 |
| v0.2.0 | 12 周 | 压力测试 + 质量门禁 (生产预备) |
| v1.0.0 | 14 周 | 性能基准 + 完整文档 (生产级) |

---

## 使用 SpecKit 的最佳实践

### 对于 Bug 报告
1. 检查 [Clarify](clarify.md) 是否为已知决策
2. 查阅 [Analyze](analyze.md) 的已知缺口清单
3. 补充材料：截图、日志、复现步骤
4. 关联至 [Tasks](tasks.md) 的适当任务

### 对于功能请求
1. 检查 [Specify](specify.md) 是否已包含
2. 查阅 [Clarify](clarify.md) 的非目标清单
3. 评估 [Plan](plan.md) 中是否计划包含
4. 若必要，创建相关 issue + 更新 tasks.md

### 对于代码审查
1. 参考 [Constitution](constitution.md) 检查设计一致性
2. 参考 [Implement](implement.md) 查看已知亮点与风险
3. 参考 [Analyze](analyze.md) 验证覆盖率
4. 用 [Tasks](tasks.md) 的检查清单验证完成度

### 对于性能优化
1. 基准化后参考 [Analyze](analyze.md) 的性能特征
2. 参考 [Plan](plan.md) 第 6 阶段的优化建议
3. 在 [Clarify](clarify.md) 中记录决策

---

## 反馈与更新

SpecKit 应与代码实现同步演进：

- **代码变更** → 更新 Implement / Specify
- **新发现的问题** → 记录在 Clarify 或 Analyze
- **任务完成** → 更新 Tasks 状态
- **里程碑达成** → 更新 Plan 版本标记

---

## 相关资源

- **参考实现**：
  - FlashDB: https://github.com/armink/FlashDB
  - LittleFS: https://github.com/littlefs-project/littlefs
- **标准**：
  - CRC-16-MODBUS: IEC 61158-2
  - W25Q128 Datasheet

---

**最后更新**：2026-02-25  
**维护者**：RocketDB 开发团队  
**版本**：SpecKit v1.0（RocketDB v0.0.2）

