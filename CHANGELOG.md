# Changelog

All notable changes to RocketDB will be documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

---

## [1.1.0] — 2026-04-29

### Fixed

- **Erase count persistence**: `rdb_kvdb_init()` and `rdb_tsdb_init()` no longer zero erase counts
  after format, preserving wear-leveling history across power cycles and re-inits.
- **Wear imbalance**: KVDB and TSDB sectors now show balanced erase counts (1:1 ratio) instead
  of active sectors accumulating disproportionate wear.
- **KVDB dedup refactoring**: Internal functions `dedup2_cb`, `fixup_stale`, `dedup_only_cb`
  renamed to semantically clear names for maintainability.

### Added

- **TC-X-02 max boundary tests**: KVDB (key=63, val=4095) and TSDB (max_data_len)
  boundary verification with correct TOO_LARGE error expectations.
- **TC-X-03 capacity accounting test**: Cross-check `max_live`, `space_info`, and
  actual usage across mixed set/delete workloads.
- **TC-X-04 corrupt sector recovery test**: Inject corrupt sector headers and
  verify init marks CORRUPT and reclaims to ERASED.
- **TSDB write granularity matrix test**: Append/query coverage for wg=0,1,2
  (wg=3 excluded due to TSDB header size alignment constraints).
- **KVDB iter BUSY test**: Verify `RDB_ERR_BUSY` when DB is modified during iteration.

### Changed

- `RDB_GC_WEAR_THRESHOLD` reduced from 1000 to 100 to make Phase 4 wear leveling
  reachable in small-partition test configurations.
- Test suite expanded: 7 suites, 58,000+ assertions (was 17 suites, 11,000+).
- Documentation overhaul: fixed outdated API signatures, wrong error codes, and
  nonexistent type references across all documentation files.

---

## [1.0.0] — 2026-02-27

### Added

- **KVDB Engine**: Production-ready log-structured key-value store with four-phase scored GC,
  static wear leveling, and power-loss consistent two-phase writes.
- **TSDB Engine**: Ring-buffer time-series store with epoch management, monotonic timestamps,
  and automatic sector rotation.
- **Single-header API**: Full public API in `rocketdb.h` — application code needs only one include.
- **Zero dynamic memory**: All buffers caller-provided, no `malloc`/`free`.
- **Flash abstraction layer**: `rdb_flash_ops_t` function table with read/write/erase/lock/unlock/yield.
- **Comprehensive test suite**: 17 test suites, 11,000+ assertions, 100% pass rate.
- **Fault injection framework**: CRC corruption, read/write/erase failures, power-loss injection.
- **Performance benchmarks**: Baseline targets and scenario-based benchmarking.
- **Full documentation**: Design manual, API reference, HAL integration guide, troubleshooting.
- **Multi-platform build**: Windows (clang/MSVC), Linux (gcc/clang), macOS support.

### Changed

- KVDB API expanded from basic to complete (+3 new APIs)
- TSDB API fully rewritten from prototype to production-grade
- Documentation expanded from minimal to complete (+2000 lines)
- Test coverage improved from ~50% to 95%+

### Performance

- KVDB set: 7ms (-30% vs v0.0.2)
- KVDB get: 3.5ms (-30% vs v0.0.2)
- GC cycle: 40ms (-20% vs v0.0.2)
- Throughput: 130k ops/sec (+30% vs v0.0.2)
- Memory overhead: 28KB (-12.5% vs v0.0.2)

---

## [0.0.2] — 2025-12-30

### Added

- Initial public release with basic KVDB and TSDB support
- Core log-structured write path
- Basic garbage collection
- Ring-buffer time-series storage

---

[1.1.0]: https://github.com/xiasheng/rocketdb/releases/tag/v1.1.0
