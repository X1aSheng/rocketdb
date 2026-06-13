# RocketDB Documentation Index

This directory contains both current project documentation and historical planning/review records.

## Current Documents

| Document | Purpose |
|----------|---------|
| [Architecture.md](Architecture.md) | Current architecture, on-flash layout, integration guide, testing/toolchain requirements |
| [test_plan.md](test_plan.md) | Current test strategy, test matrix, build output policy, compiler/toolchain notes |
| [offline_flash_analysis.md](offline_flash_analysis.md) | PC/server-side raw Flash dump analysis with `tools/rdbdump` |
| [W25QXX_GUIDE.md](W25QXX_GUIDE.md) | W25QXX-class SPI NOR integration notes |
| [flash_lifespan.md](flash_lifespan.md) | Flash lifetime and write amplification notes |

## Test And Tool Documents

| Document | Purpose |
|----------|---------|
| [../tests/sim/README.md](../tests/sim/README.md) | Simulation test methodology |
| [../tests/sim/TEST_FRAMEWORK.md](../tests/sim/TEST_FRAMEWORK.md) | Test framework usage |
| [../tests/sim/FAULT_INJECTION.md](../tests/sim/FAULT_INJECTION.md) | Flash fault injection guide |
| [../tests/sim/vector_format.md](../tests/sim/vector_format.md) | Replay vector format, distinct from raw Flash dumps |
| [../tests/perf/README.md](../tests/perf/README.md) | Performance benchmark methodology |
| [../tools/rdbdump/README.md](../tools/rdbdump/README.md) | `rdbdump` CLI usage and export layout |

## Review Records

| Document | Focus |
|----------|-------|
| [PROJECT-REVIEW-260613-103231.md](PROJECT-REVIEW-260613-103231.md) | v1.2.0 review: version sync, Docker/CI, batch scripts, code quality |
| [PROJECT-REVIEW-260604-001500.md](PROJECT-REVIEW-260604-001500.md) | Previous review with Zephyr Kconfig fixes |
| [PROJECT-REVIEW-260604-000000.md](PROJECT-REVIEW-260604-000000.md) | Zephyr port single-byte write alignment |

`PROJECT-REVIEW-*`, `RocketKV/`, and `SpecKit/` preserve design notes, review records, and planning material from earlier project phases. They may mention older paths or historical validation results; current build/test behavior is defined by the documents listed above.
