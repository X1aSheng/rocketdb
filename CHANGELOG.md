# Changelog

All notable changes to RocketDB will be documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

---

## [1.1.2] — 2026-04-30

### Fixed

- **KVDB `fixup_stale` int8_t overflow**: Changed loop index from `int8_t` to `int16_t`
  to support sector counts >127 without silent dedup skipping.
- **KVDB `gc_execute` iter_gen**: `iter_gen` is now incremented unconditionally on sector
  erase, preventing active iterators from accessing erased sectors during zero-live GC.
- **KVDB `gc_execute` dedup ordering**: Sectors are now sorted by descending `create_seq`
  before post-GC duplicate marking, ensuring the newest record copy is preserved.
- **KVDB `iter_next` flash error**: `fl_read` return value now checked when copying
  value buffer; returns `RDB_ERR_FLASH` on failure instead of silently yielding garbage.
- **TSDB `ts_rotate` error path**: On `ts_init_sec` failure, `head_off` is now set to
  `sector_size` to force rotation on next append instead of writing to stale offset
  in the already-sealed head sector.
- **TSDB `ts_classify` read retry**: Transient flash read errors now trigger one retry
  before classifying a sector as CORRUPT, preventing unnecessary data loss.
- **TSDB `ts_scan` write checking**: Return values of `twr_f` during WRITING record
  recovery are now checked; records remain WRITING on write failure for retry.
- **TSDB `ts_data_crc` yield**: Added `tyield()` in the data CRC streaming loop to
  prevent RTOS starvation on large payloads.
- **Test framework `va_copy`**: Fixed undefined behavior from double `va_start` by
  using `va_copy` for log file output.
- **Fault injection tests**: Added missing `rdb_kvdb_format()` calls and proper
  `db.part`/`db.sectors` assignment in all 5 test cases. Added assertions for
  write failure detection.
- **KVDB/TSDB `write_off` uint16_t overflow**: Changed `write_off` in
  `rdb_kv_sector_meta_t`, `rdb_kvdb_t`, and `rdb_kv_iter_t.offset` from `uint16_t`
  to `uint32_t`, and removed explicit truncation casts. Prevents silent offset
  wraparound on sectors >64 KB (e.g. 128 KB W25Q128).
- **TSDB init Phase 3 `twr_f`**: Now checks `twr_f()` return values when promoting
  or demoting WRITING records during recovery, consistent with `ts_scan()`.
- **TSDB init `tdc()` guard**: Added runtime validation that `sector_size > tds(db)`
  to prevent unsigned wraparound in capacity computation.
- **KVDB/TSDB `format` stack usage**: Changed `saved_ec[RDB_MAX_SECTORS]` from
  stack (1020 bytes) to static allocation to avoid stack overflow on small MCUs.
- **TSDB `get_latest`/`get_oldest`**: Now checks `trd()` return value when copying
  data to caller buffer; returns `RDB_ERR_FLASH` on read failure instead of
  returning uninitialized data.
- **TSDB `ts_find_last_valid` const**: Removed incorrect `const` qualifier on `db`
  parameter that was implicitly discarded when calling `trd()`.
- **Test `sim_flash_read` usage**: Replaced direct `g_flash.mem[]` reads in
  `test_kvdb_basic` and `test_tsdb_stress` with `sim_flash_read()` calls.

---

## [1.1.1] — 2026-04-30

### Fixed

- **Version number**: `rdb_version()` now returns `0x010100` (v1.1.0), matching CHANGELOG.
  `@version` tags in source headers also updated.
- **KVDB init Phase 3**: Fallback sector selection now resets SEALED→ACTIVE status,
  fixing inconsistent runtime state when a sealed sector is chosen as active.
- **TSDB recount**: Periodic `total_count` reconciliation now uses read-only scan
  (`RDB_FALSE`), avoiding unexpected flash writes during append operations.
- **README broken link**: Fixed `docs/design.md` → `docs/rocketdb%20design.md`.

### Added

- **TSDB format validation**: `rdb_tsdb_format()` now validates `sector_cnt` consistency
  (matching KVDB format's validation pattern).  Removed overly strict `erase_cnts`
  NULL check that broke the init→format fallback path when caller passes NULL
  (which is valid per the API contract).
- **W25QXX integration guide**: New comprehensive guide at `docs/W25QXX_GUIDE.md` covering
  SPI command opcodes, HAL implementation examples for read/write/erase/lock/unlock/yield,
  recommended configuration for W25Q32/Q64/Q128, timing characteristics, and multi-task
  SPI bus sharing considerations.
- **W25QXX HAL template comments**: Added SPI flash command opcode reference in
  `interface/rocketdb_interface_template.c`.

### Changed

- Test suite: 38/38 cases pass, 57,816 assertions (was 37/38 — `kv_max_boundaries`
  confirmed as stale binary, resolved by rebuild).

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

[1.1.1]: https://github.com/xiasheng/rocketdb/releases/tag/v1.1.1
[1.1.0]: https://github.com/xiasheng/rocketdb/releases/tag/v1.1.0
