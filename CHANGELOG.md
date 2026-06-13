# Changelog

All notable changes to RocketDB will be documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

---

## [1.5.0] — 2026-06-13

### Added

- **Docker support**: Multi-stage Dockerfile and docker-compose.yml for Linux
  build and test.  `docker build -t rocketdb . && docker run --rm rocketdb`
  runs the full CTest suite in an Ubuntu 24.04 container.
- **`RDB_WRITE_GRAN_MAX` constant**: Named constant (value 3) for the write
  granularity exponent upper bound, used by `rdb_kvdb_init()` validation.

### Fixed

- **Version consistency**: `rdb_version()` now returns `0x010200` (v1.2.0),
  matching the README badge and source `@version` tags.
- **KVDB sector header CRC documentation**: Comment now accurately states
  that `hdr_crc` covers bytes [0..5] and [8..15], excluding its own storage.
- **CMakePresets binary directory**: `debug` and `release` presets now use
  separate build directories (`cmake-build` vs `cmake-build-release`).
- **CI cache key**: `actions/cache` updated from v4 to v5; cache key widened
  to include `src/*.c`, `src/*.h`, `tests/sim/*.c`, `tests/sim/*.h`.
- **Windows batch clean targets**: `run_all_tests.bat clean` and
  `run_suite.bat clean` now use `rmdir/mkdir` for guaranteed clean state.

### Changed

- **CI**: `actions/cache@v4` → `actions/cache@v5`.
- **CMakePresets.json**: `debug-win` description now notes that `clang` must
  be in PATH or CMAKE_C_COMPILER must use a full path.

## [1.4.0] — 2026-06-03

### Fixed

- **sim_crypto.c CRC-16 NULL seed guard**: Added explicit `data==NULL && len==0`
  check matching the Zephyr and interface template implementations for portable
  seed retrieval (`rdb_crc16(NULL, 0)` returns `0xFFFF`).
- **Windows batch compiler discovery**: Batch scripts now search standard
  `C:\Program Files\LLVM\bin` and PATH before dev-specific `D:\Programs` paths,
  enabling CI runners to find `clang.exe` without environment-specific overrides.

### Added

- **CMakePresets.json**: Pre-configured build presets (debug, release, debug-win,
  minimal) with matching build and test presets.
- **CI build caching**: GitHub Actions `actions/cache@v4` step for the CMake
  build directory across matrix variants.

### Changed

- **`.gitignore`**: Added `*.orig`, `*.bak`, `*.save` editor backup patterns.

---

## [1.3.0] — 2026-05-25

### Added

- **Regression coverage for review fixes**: Added tests for KVDB large
  non-aligned values across 1/2/4/8-byte write granularities, max-key cache-hit
  verification at the 32-byte architecture limit, de-dup fingerprint collisions,
  TSDB unsupported write granularities, and cross-epoch query scanning.
- **KVDB key-to-address cache** (`RDB_KV_CACHE_SIZE`): Optional direct-mapped
  cache that maps key fingerprints (16-bit FNV-1a hash + key length + 8-byte
  prefix) to absolute Flash addresses. Eliminates full-table scans for repeated
  get()/set() calls on frequently-accessed keys. Controlled by compile-time
  macro `RDB_KV_CACHE_SIZE` (0 = disabled, recommended: 64 slots = 1024 bytes).
- **KVDB GC batch migration**: Consecutive small live records are packed into
  a single Flash write during GC migration, reducing write amplification by
  50-80% for small-record workloads.
- **`test_kvdb_cache` suite**: 8 new test cases (6,153 assertions) validating
  cache correctness, TSDB safety fixes, recount optimization, and GC migration
  consistency. All tests use deterministic varied-length data distribution
  (5 categories, 1–255 bytes) via `make_value()` helper.
- **Unified trace format**: All test files now use consistent per-operation
  trace output: `[KV-WRITE] key=%s vsz=%u`, `[KV-READ] key=%s`,
  `[KV-DEL] key=%s`, `[TS-APPEND] time=%u dlen=%u`.
- **Cache state tracing**: `trace_cache_stats()` reports cache occupancy
  (used/total slots, fill percentage) at key phases in KVDB cache tests.

### Fixed

- **Zephyr port interface sync**: Kconfig now enforces the 32-byte KVDB key
  limit, the adapter stores/enforces `write_gran`, checks `device_is_ready()`,
  uses the same FNV-1a folded hash as the simulator, and no longer references a
  missing shell source file unless it exists.
- **Bare-metal interface template sync**: `interface/` now exposes a
  RocketDB-compatible `rocketdb_interface_ops` table, bridges `rdb_crc16()`,
  `rdb_crc16_cont()`, and `rdb_hash16()`, uses FNV-1a folded hashing, and
  documents W25QXX 256-byte page-program splitting.
- **Examples sync**: Example partitions now explicitly set `flash_ctx`, use
  designated flash ops initializers, default to W25QXX-friendly
  `write_gran=0`, and size KVDB iterator key buffers from `RDB_MAX_KEY_LEN`.
- **KVDB large value writes**: Large values are now streamed as aligned chunks
  with `0xFF` tail padding, so HALs that enforce `write_gran` never receive a
  short final write.
- **KVDB cache long-key verification**: Cache-hit validation now reads keys
  into an `RDB_MAX_KEY_LEN` buffer instead of `RDB_STACK_BUF_SIZE`.
- **KVDB de-dup collisions**: Init/GC de-dup now verifies the full key when
  hash, length, and prefix collide, preventing accidental deletion of distinct
  keys.
- **KVDB max key length scan**: `strkey_len()` uses `size_t` loop state so the
  bounded key scan remains safe at the 32-byte architecture limit.
- **KVDB key-length architecture limit**: `RDB_MAX_KEY_LEN` now defaults to 32
  and compile-time validation rejects larger values. Embedded applications
  should map long paths or dynamic names to short keys before storage.
- **TSDB write granularity contract**: `rdb_tsdb_init()` and
  `rdb_tsdb_format()` reject `write_gran > 1` until the 2-byte seal protocol is
  redesigned for wider program units.
- **TSDB cross-epoch query**: Range query no longer stops globally at the first
  timestamp greater than `to`, preserving data after epoch/time resets.
- **Windows/C99 portability**: Compile-time assertions now use a portable macro
  and disabled KV cache builds avoid zero-length arrays.
- **TSDB `ts_mark_dead()`**: Added `ts_mark_dead()` and calls in all write-failure
  paths matching KVDB's `mark_dead()` pattern. Prevents NOR 1→0 violations on
  subsequent writes after partial record corruption.
- **TSDB `head_off` advancement on commit failure**: After a commit-byte write
  failure, `head_off` now advances past the aborted record, preventing NOR
  violations on the next append.

### Changed

- **TSDB recount optimization**: Removed periodic O(N) full recount in
  `rdb_tsdb_append()`. `total_count` is now maintained incrementally via
  append increments and rotation `lost` accounting.
- **Test data diversity**: All test suites now use varied-length data
  distributions instead of fixed-size payloads, improving boundary coverage.

## [1.2.0] — 2026-05-24

### Added

- **Zephyr OS port layer** (`zephyr/`): Provides `rocketdb_port.c` with flash
  ops backed by Zephyr's `flash_read/write/erase` API, CRC-16/MODBUS and FNV-1a
  hash implementations, `rocketdb_partition_init()` factory, Kconfig options
  for all compile-time settings, CMakeLists.txt for Zephyr module build, and
  `module.yml` for Zephyr module auto-discovery.
- **Per-callback context pointer** (`void *ctx`): Every callback in
  `rdb_flash_ops_t` now receives a `void *ctx` first argument. This enables a
  single ops table to be shared across multiple flash devices/partitions. The
  context is stored in `rdb_partition_t.flash_ctx`.

### Changed

- **`rdb_flash_ops_t` signature**: All six callbacks gain `void *ctx` as first
  parameter. Existing users must add a `void *ctx` parameter to their ops
  implementations and set `rdb_partition_t.flash_ctx = NULL` for backward
  compatibility.
- **`rdb_partition_t`**: New field `void *flash_ctx`. Uninitialised fields
  are zeroed by C99 designated-initialiser rules, so existing code that omits
  this field is automatically compatible (ctx = NULL).

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
- **KVDB `iter_next` truncation**: Removed leftover `(uint16_t)` cast on
  `it->offset` assignment after struct field had already been widened to
  `uint32_t`, preventing infinite loop on sectors >64 KB.
- **Test framework `log_output` va_copy ordering**: Moved `va_copy` before
  `vprintf` to avoid copying from consumed `va_list` (UB per C99 §7.15).
- **Makefile portability**: Replaced Windows cmd.exe syntax (`if exist`,
  `for %%t`, `rmdir /Q /S`, backslash paths) with POSIX shell equivalents
  (`mkdir -p`, `for...done`, `rm -rf`, forward slashes). Added conditional
  `-lm` for Linux/macOS link rules.
- **TSDB `ts_classify` comment**: Documented that `magic == 0xFFFFFFFFu`
  check serves as implicit start-of-sector erase probe.
- **`test_make_log_path`**: Added documentation noting static buffer is not
  thread-safe.
- **`fault_import_rules`**: Added TODO comment marking it as unimplemented stub.

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

- **TC-X-02 max boundary tests**: KVDB (key=32, val=4095) and TSDB (max_data_len)
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
