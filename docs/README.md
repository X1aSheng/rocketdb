# RocketDB Documentation

## Directory Structure

```
docs/
├── README.md                    ← This index
├── architecture/                System design & technical reference
├── guides/                      How-to documentation
├── reports/                     Timestamped review & test records
├── decisions/                   Architecture Decision Records (ADR)
├── planning/                    Roadmaps & design prompts
├── reviews/                     Code review tracking
└── SpecKit/                     Spec-driven development methodology
```

## Architecture (`architecture/`)

System design, on-flash data structures, integration guides, and hardware reference.

| Document | Purpose |
|----------|---------|
| [ARCHITECTURE.md](architecture/ARCHITECTURE.md) | Full design manual: KVDB/TSDB engines, GC algorithm, power-loss recovery, API reference |
| [TEST_PLAN.md](architecture/TEST_PLAN.md) | Test strategy, matrix, build output policy, compiler notes |
| [Flash Chip Lifespan Evaluation Model.md](architecture/Flash%20Chip%20Lifespan%20Evaluation%20Model.md) | Flash lifetime estimation and write amplification analysis |
| [INIT-OPTIMIZATION-260616.md](architecture/INIT-OPTIMIZATION-260616.md) | Init scan merging and performance optimisation |
| [RESOURCE-ANALYSIS-260616.md](architecture/RESOURCE-ANALYSIS-260616.md) | ROM/RAM/efficiency analysis for v1.6.0 |
| [KVDB-REVIEW-260617.md](architecture/KVDB-REVIEW-260617.md) | KVDB module audit and design rule verification |
| [STORAGE-COMPATIBILITY-260617.md](architecture/STORAGE-COMPATIBILITY-260617.md) | Storage compatibility analysis |
| [W25QXX_GUIDE.md](architecture/W25QXX_GUIDE.md) | W25QXX-class SPI NOR integration guidance |
| [OFFLINE_ANALYSIS.md](architecture/OFFLINE_ANALYSIS.md) | PC-side raw Flash dump analysis with `tools/rdbdump` |
| [gen_calc.py](architecture/gen_calc.py) | Flash lifespan calculation script |
| [flash_lifespan_calc.xlsx](architecture/flash_lifespan_calc.xlsx) | Flash lifespan spreadsheet |

## Guides (`guides/`)

How-to documentation for using and deploying RocketDB.

| Document | Purpose |
|----------|---------|
| [DEPLOYMENT.md](guides/DEPLOYMENT.md) | Cloud deployment, CI/CD, and containerised build validation |

## Reports (`reports/`)

Timestamped snapshots of project reviews, code audits, and test results.

| Document | Date | Focus |
|----------|------|-------|
| [PROJECT-REVIEW-260630-232117.md](reports/PROJECT-REVIEW-260630-232117.md) | 2026-06-30 | **最新审查**: 版本同步、HAL 调试、CMake、构建脚本、文档 |
| [PROJECT-REVIEW-260616-merged.md](reports/PROJECT-REVIEW-260616-merged.md) | 2026-06-16 | **合并综合报告**: 9 次审查、40+ 发现、全部修复状态 |
| [RocketKV/](reports/RocketKV/) | 2025-12 – 2026-02 | Historical KVDB/TSDB test reports |

> 原始 9 份独立报告已合并为单一综合文档。详细代码审计见合并报告第 9 节。

## SpecKit (`SpecKit/`)

Spec-driven development methodology — project constitution, specification, planning, implementation, and analysis.

| Document | Purpose |
|----------|---------|
| [SpecKit.md](SpecKit/SpecKit.md) | Methodology overview |
| [constitution.md](SpecKit/constitution.md) | Project constitution and design rules |
| [specify.md](SpecKit/specify.md) | Specification phase guide |
| [plan.md](SpecKit/plan.md) | Planning phase guide |
| [implement.md](SpecKit/implement.md) | Implementation tracking |
| [analyze.md](SpecKit/analyze.md) | Analysis and verification |
| [clarify.md](SpecKit/clarify.md) | Requirements clarification |

## Quick Links

- [Project README](../README.md) — Overview and quick start
- [CHANGELOG](../CHANGELOG.md) — Version history
- [Contributing](../CONTRIBUTING.md) — Development setup and guidelines
- [Test Framework](../tests/sim/TEST_FRAMEWORK.md) — Writing and running tests
