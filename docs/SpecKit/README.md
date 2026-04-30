# RocketDB SpecKit 项目规范体系

## 概述

RocketDB SpecKit 是一套完整的项目规范与管理文档体系，用于指导 RocketDB v0.0.2 从当前状态演进至 v1.0.0 生产级版本。

基于 bitarray 项目的 SpecKit 方法论，系统化地定义了核心约束、API 规格、实现现状、分阶段计划、任务清单、关键决策、功能分析，确保项目的透明性、可追溯性和可执行性。

### 📅 最新更新（2026-02-25）

**完成文档整合**：将 `design.md`（1391 行）、`test_plan.md`、`test_request.txt` 的核心内容融合到 SpecKit 各文档中。

**主要变更**：
- ✅ **constitution.md**：添加测试基准配置、数据分布要求、可靠性测试要求
- ✅ **specify.md**：完善编译配置说明、添加 RDB_GC_RESERVE 计算、编译时断言
- ✅ **clarify.md**：添加 13 个关键设计决策（write_seq 恢复、损坏记录跳过、数据安全原则等）
- ✅ **tasks.md**：更新测试框架任务（基于 test_plan.md）、细化测试用例映射
- ✅ **analyze.md**：添加覆盖率状态总结、13 项缺口清单、测试用例映射表
- ✅ **implement.md**：添加设计亮点、结构体对齐设计、扫描优化策略
- ✅ **plan.md**：完善测试框架建设策略、添加数据分布和测试矩阵
- ✅ **SpecKit.md**：添加文档来源整合说明

---

## 文档清单

### 核心文档（7 个必读文档）

| # | 文档 | 用途 | 长度 | 读者 |
|---|------|------|------|------|
| 1 | [SpecKit.md](SpecKit.md) | **导航与总览** | ~250行 | 所有人 |
| 2 | [constitution.md](constitution.md) | **宪法（核心约束）** | ~150行 | 架构师、决策者 |
| 3 | [specify.md](specify.md) | **规格（API定义）** | ~280行 | API 用户、开发者 |
| 4 | [implement.md](implement.md) | **实现（当前状态）** | ~200行 | 代码维护者 |
| 5 | [plan.md](plan.md) | **计划（阶段路线）** | ~380行 | 项目经理 |
| 6 | [tasks.md](tasks.md) | **任务（执行清单）** | ~320行 | 任务执行者 |
| 7 | [clarify.md](clarify.md) | **澄清（决策记录）** | ~280行 | 架构师、高级开发者 |
| 8 | [analyze.md](analyze.md) | **分析（缺口评估）** | ~400行 | 代码审查者 |

**总计约 2200 行文档，完整覆盖项目全生命周期。**

### 辅助文档

- [SUMMARY.md](SUMMARY.md) — 工作总结与核心指标

---

## 快速导航

### 🚀 我需要快速了解项目
👉 阅读：[SpecKit.md](SpecKit.md) （5 分钟）

### 📖 我是 API 使用者
👉 按顺序阅读：
1. [specify.md](specify.md) — API 参考（必读）
2. [constitution.md](constitution.md) — 设计约束

### 👨‍💻 我是代码维护者
👉 按顺序阅读：
1. [implement.md](implement.md) — 当前实现状态
2. [analyze.md](analyze.md) — 功能分析与缺口
3. [tasks.md](tasks.md) — 未来任务清单

### 🎯 我是项目经理
👉 按顺序阅读：
1. [constitution.md](constitution.md) — 项目范围
2. [plan.md](plan.md) — 分阶段计划
3. [tasks.md](tasks.md) — 具体任务与优先级

### 🏛️ 我是架构师
👉 按顺序阅读：
1. [constitution.md](constitution.md) — 核心约束
2. [clarify.md](clarify.md) — 关键决策
3. [analyze.md](analyze.md) — 完整性评估

---

## SpecKit 体系说明

### Constitution（宪法）
定义 RocketDB 的**不可变目标与根本约束**：
- 核心目标（KVDB/TSDB 各自的使命）
- 根本约束（零动态内存、掉电一致性、磨损均衡）
- 设计边界（不追求什么）
- 是否可修改：❌ 不可（修改需重新评审）

### Specify（规格）
定义公共 API、错误码、编译配置、Flash 抽象：
- API 函数签名、参数、返回值
- 错误码完整语义
- 编译时配置与约束
- Flash 操作抽象层
- 是否可修改：⚠️ 需向后兼容（breaking changes 需审议）

### Implement（实现）
当前 v0.0.2 的实现现状、已知亮点、风险与下一步目标：
- 源文件与功能完成度
- 已知设计亮点（Phase 4、iter 代数等）
- 已知风险与局限
- 是否可修改：✅ 可（反映实现真实状态）

### Plan（计划）
从 v0.0.2 到 v1.0.0 的分阶段可执行计划：
- 6 阶段、6~7 周的完整路线
- 每阶段的目标、任务、交付物
- 里程碑定义
- 是否可修改：⚠️ 需协议（影响时间表）

### Tasks（任务）
具体的可执行任务清单，包含优先级、工作量、依赖关系：
- P0/P1/P2 优先级划分
- 关键路径识别
- 每个任务的输入/输出
- 是否可修改：✅ 可（反映当下进度）

### Clarify（澄清）
设计中的开放性问题与团队决策记录：
- 11 个关键设计决策
- 每个决策的备选方案与原因
- 已确认/待决策清单
- 是否可修改：⚠️ 需重新评审（影响设计走向）

### Analyze（分析）
功能完整性、代码路径覆盖、缺口识别与改进建议：
- KVDB/TSDB 各模块功能覆盖率评估
- 已知缺口清单（优先级分类）
- 与参考实现对比（FlashDB/LittleFS）
- 性能特征与优化建议
- 是否可修改：✅ 可（反映分析结果）

---

## 关键指标一览表

### 功能完整性
| 模块 | 完成度 | 测试覆盖 | 备注 |
|------|--------|---------|------|
| KVDB 核心 API | ✅ 100% | ⚠️ 70% | 缺 GC Phase 4 完整验证 |
| KVDB 四阶段 GC | ✅ 100% | ⚠️ 60% | Phase 4 效果待压力测试 |
| KVDB Recovery | ✅ 100% | ⚠️ 60% | 缺掉电中断点全覆盖 |
| TSDB 核心 API | ✅ 100% | ⚠️ 75% | 缺 epoch 边界完整验证 |
| TSDB Rotation | ✅ 100% | ✅ 80% | 基本完整 |

### 文档完整性
| 项 | 现状 |
|-----|------|
| API 签名定义 | ✅ 完整 |
| 错误码语义 | ✅ 完整 |
| 编译时配置 | ✅ 完整 |
| On-Flash 结构体 | ✅ 对齐详细说明 |
| 设计决策记录 | ✅ 11 个完整记录 |
| 任务分解 | ✅ 35+ 个任务 |
| 缺口识别 | ✅ 5~10 个优先级缺口 |

### 可执行性
| 指标 | 评分 |
|------|------|
| 计划的可细化度 | ✅ 高（6 阶段明确） |
| 任务的可追踪度 | ✅ 高（35+ 个具体任务） |
| 决策的可追溯度 | ✅ 完整（11 个决策记录） |
| 风险的可识别度 | ✅ 中~高（已列 5+ 高风险） |

---

## 使用流程

### 新功能开发流程
```
1. 在 specify.md 中查找 API 是否存在
   ├─ YES → 直接使用，参考 clarify.md 了解设计背景
   └─ NO  → 提出 feature request，更新 clarify.md
   
2. 检查 constitution.md，确保不违反核心约束
   
3. 参考 tasks.md 的相关优先级与工作量评估
   
4. 完成后更新 implement.md 的实现状态
```

### Bug 修复流程
```
1. 检查 clarify.md 的已知问题清单
   ├─ YES, 已知 → 参考决策记录进行修复
   └─ NO        → 在 clarify.md 中记录新问题
   
2. 参考 analyze.md 的缺口清单，确定优先级
   
3. 从 analyze.md 找到对应的测试场景
   
4. 修复后添加测试用例（tasks.md）
   
5. 更新 implement.md 和相关测试报告
```

### 性能优化流程
```
1. 参考 analyze.md 的性能特征分析
   
2. 确认 plan.md 第 6 阶段（性能基准）的进度
   
3. 基准化指标后提出优化建议
   
4. 更新 analyze.md 的优化实施结果
```

---

## 核心概念解释

### 四阶段 GC 评分机制
RocketDB KVDB 的核心创新：
```
Phase 0 (Zero-Live)      → 垃圾率 100%  → 直接擦除
Phase 1 (Low-Live)       → 垃圾率 >20%  → 迁移有效 + 擦除
Phase 2 (Wear Balance)   → erase_cnt↓  → 选最小擦除次数扇区
Phase 3 (Oldest)         → create_seq↓ → 选最老扇区
Phase 4 (Cold Sector)    → 时隔 >=1000 → 主动激活冷扇区
```

**目标**：自动维持全盘磨损均衡 (max_erase_cnt - min_erase_cnt ≤ 1000)

### 掉电一致性（二次写）
```
Step 1: 写入新记录 + 数据 (state=0xFF WRITING)
Step 2: 完整性校验 (CRC OK)
Step 3: 提交状态 (state=0xFE VALID) ← 断电安全点
Step 4: 标记旧版本 (state=0xFC DEAD)  ← 可选
```

**保证**：任意时刻断电，下次 init 都能恢复一致状态（Phase 1~4）

### Epoch 防时间戳回绕
```
检测：ts_n < ts_n-1 （时间戳倒序）
动作：epoch++ （时间基准重置）
查询：支持跨 epoch 范围查询 [from_ts, to_ts]
```

**保证**：时钟回拨或系统时间重置不破坏查询逻辑

---

## 与 bitarray 对标

RocketDB SpecKit 基于 bitarray 框架设计，但覆盖更深、体量更大：

| 方面 | bitarray | rocketdb |
|------|----------|----------|
| 文档数 | 7 | 8 |
| 总行数 | 600+ | 2200+ |
| 设计决策 | 4 | 11 |
| 任务数 | 10 | 35+ |
| 覆盖深度 | 概念验证 | 生产级规范 |
| 计划周期 | 3 阶段 | 6 阶段 |

---

## 文件组织

```
rocketdb/
├── rocketdb.h                   — 公共 API 声明
├── rocketdb_kvdb.c              — KVDB 实现
├── rocketdb_tsdb.c              — TSDB 实现
├── design.md                    — 完整设计手册 (1391 行)
├── test_plan.md                 — 测试计划
├── test_request.txt             — 原始需求
│
├── SpecKit/                     ← 项目规范体系
│   ├── README.md                ← 本文件
│   ├── SpecKit.md               — 总体导航
│   ├── constitution.md          — 宪法（核心约束）
│   ├── specify.md               — 规格（API定义）
│   ├── implement.md             — 实现状态
│   ├── plan.md                  — 阶段计划
│   ├── tasks.md                 — 任务清单
│   ├── clarify.md               — 决策记录
│   ├── analyze.md               — 功能分析
│   └── SUMMARY.md               — 工作总结
│
├── tests/                       — 测试代码
│   └── sim/                     — Flash 模拟器
│
└── build.bat / Makefile         — 构建脚本
```

---

## 关键数据

### v0.0.2 当前代码量
- `rocketdb.h`: ~1200 行（API 类型定义）
- `rocketdb_kvdb.c`: ~1500 行（KVDB 核心）
- `rocketdb_tsdb.c`: ~900 行（TSDB 核心）
- **合计**: ~3600 行核心代码

### SpecKit 文档量
- **8 个文档**，**~2200 行**，覆盖规格、计划、决策、分析

### 开发计划
- **总周期**: 6~7 周（从 v0.0.2 → v1.0.0）
- **阶段数**: 6 个（规格 → 框架 → 覆盖 → 压力 → 质量 → 性能）
- **任务数**: 35+ 个（具体可执行）
- **投入**: 估计 200+ 人工小时

---

## 快速检查清单

在修改代码或计划时：

### 代码审查
- [ ] 是否遵守 constitution.md 中的核心约束？
- [ ] API 签名是否与 specify.md 一致？
- [ ] 错误返回是否符合错误码定义？
- [ ] 是否更新了 implement.md？

### 计划调整
- [ ] 是否影响了 plan.md 的时间表？
- [ ] 是否影响了 tasks.md 中的关键路径？
- [ ] 是否需要在 clarify.md 中记录新决策？

### 功能新增
- [ ] 是否需要在 specify.md 中添加 API？
- [ ] 是否需要在 constitution.md 中调整约束？
- [ ] 是否需要在 tasks.md 中添加新任务？

---

## 维护说明

### 文档更新频率
- **constitution.md**: 极少更新（仅在根本性设计变化时）
- **specify.md**: 低频（API 变化时）
- **implement.md**: 中频（每阶段/里程碑更新）
- **plan.md**: 低频（调整时间表时）
- **tasks.md**: 高频（每天更新进度）
- **clarify.md**: 中频（新决策时）
- **analyze.md**: 中频（缺口识别/改进时）

### 推荐流程
1. 代码 commit 或 PR 时，检查是否需要同步 SpecKit
2. 每周专项 review，检查文档与代码一致性
3. 每个里程碑完成后，更新 implement.md 与 plan.md

---

## 反馈与改进

发现 SpecKit 中的问题或改进空间？

1. **缺陷**：文件 issue，分类为 `docs-speckit`
2. **改进建议**：创建 feature request，链接相关文档
3. **决策变更**：更新 clarify.md，通知相关项目干系人

---

## 许可与归属

RocketDB SpecKit 遵循 RocketDB 主项目的开源协议。  
框架方法论参考 bitarray 项目的 SpecKit 体系。

---

**最后更新**：2026-02-25  
**版本**：SpecKit v1.0（RocketDB v0.0.2）  
**维护者**：RocketDB 开发团队

