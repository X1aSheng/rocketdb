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
| [FLASH_DESIGN.md](architecture/FLASH_DESIGN.md) | Flash lifetime estimation and write amplification analysis |
| [W25QXX_GUIDE.md](architecture/W25QXX_GUIDE.md) | W25QXX-class SPI NOR integration guidance |
| [OFFLINE_ANALYSIS.md](architecture/OFFLINE_ANALYSIS.md) | PC-side raw Flash dump analysis with `tools/rdbdump` |
| [gen_calc.py](architecture/gen_calc.py) | Flash lifespan calculation script |
| [flash_lifespan_calc.xlsx](architecture/flash_lifespan_calc.xlsx) | Flash lifespan spreadsheet |

## Guides (`guides/`)

How-to documentation for using and deploying RocketDB.

| Document | Purpose |
|----------|---------|
| [DEPLOYMENT.md](guides/DEPLOYMENT.md) | Docker, Kubernetes, and cloud deployment |
| [DEVELOPMENT.md](../CONTRIBUTING.md) | Project structure, build system, contributing guide |
| [TESTING.md](../tests/sim/TEST_FRAMEWORK.md) | Test framework usage and conventions |
| [API.md](../src/rocketdb.h) | Public API header (Doxygen-commented) |
| [PERFORMANCE.md](../tests/perf/README.md) | Benchmark methodology and interpretation |

## Reports (`reports/`)

Timestamped snapshots of project reviews, code audits, and test results.

| Document | Date | Focus |
|----------|------|-------|
| [CODE-REVIEW-260616.md](reports/CODE-REVIEW-260616.md) | 2026-06-16 | Full codebase audit: 10 findings ranked by severity |
| [PROJECT-REVIEW-260613-161500.md](reports/PROJECT-REVIEW-260613-161500.md) | 2026-06-13 | v1.2.0 review: CI, Docker, batch scripts |
| [PROJECT-REVIEW-260613-103231.md](reports/PROJECT-REVIEW-260613-103231.md) | 2026-06-13 | v1.2.0 review: version sync, code quality |
| [FUNCTION-NAMING-REVIEW-260613.md](reports/FUNCTION-NAMING-REVIEW-260613.md) | 2026-06-13 | Function naming semantic analysis |
| [PROJECT-REVIEW-260604-001500.md](reports/PROJECT-REVIEW-260604-001500.md) | 2026-06-04 | Zephyr Kconfig fixes |
| [PROJECT-REVIEW-260604-000000.md](reports/PROJECT-REVIEW-260604-000000.md) | 2026-06-04 | Zephyr port single-byte alignment |
| [PROJECT-REVIEW-260603-142000.md](reports/PROJECT-REVIEW-260603-142000.md) | 2026-06-03 | Review snapshot |
| [PROJECT-REVIEW-260525-230807.md](reports/PROJECT-REVIEW-260525-230807.md) | 2026-05-25 | v1.3.0 feature review |
| [PROJECT-REVIEW-260517-160748.md](reports/PROJECT-REVIEW-260517-160748.md) | 2026-05-17 | Early review record |
| [RocketKV/](reports/RocketKV/) | 2025-12 – 2026-02 | Historical KVDB/TSDB test reports |

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
