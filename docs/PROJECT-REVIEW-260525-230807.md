# RocketDB Review and Remediation Plan

Generated: 2026-05-25 23:08:07 Asia/Shanghai

## Scope

- Repository: `G:\rocketdb`
- Out of scope: `G:\shark-socket`, `G:\shark-mqtt`
- Cloud, Docker, and Kubernetes deployment: skipped by user decision for this local remediation round.

## Baseline

- `build\run_all_tests.bat test`: passed 8/8 before changes.

## Findings

1. KVDB large value writes can issue a final unaligned write when `write_gran` is 1, 2, or 3.
2. KVDB cache-hit key verification uses `RDB_STACK_BUF_SIZE` for the key read buffer even when `RDB_MAX_KEY_LEN` is larger.
3. KVDB stale-record de-duplication uses only hash, key length, and an 8-byte prefix, so rare fingerprint collisions can hide distinct keys during init or GC marking.
4. `strkey_len()` uses 8-bit loop state and can wrap when `RDB_MAX_KEY_LEN` is configured near the documented 254-byte maximum.
5. TSDB accepts `write_gran > 1` even though its current two-byte seal protocol is only safe for byte and half-word flash write granularities.
6. TSDB range query stops globally when it sees a timestamp above the upper bound, which is not valid across epoch changes or timestamp resets.
7. Public-header compile-time assertions and zero-slot cache layout are not portable enough for strict MSVC/C99 builds.
8. CI should exercise the same CMake, CTest, batch, cache, and perf build paths used locally.

## Remediation Log

The sections below are updated as fixes and validation complete.

| Area | Reproduction | Fix | Verification |
| --- | --- | --- | --- |
| KVDB aligned large value writes | Added large non-aligned value coverage in `kv_write_gran_matrix` for `write_gran=0..3`. | Large value streaming now writes aligned chunks and merges final `0xFF` padding. | `build\build_kvdb_all.bat test`, `build\build.bat all test`, CTest all passed. |
| KVDB cache long key verification | Added `kv_cache_long_key_hit` with `RDB_KV_CACHE_SIZE=64` and `RDB_MAX_KEY_LEN=254`. | Cache-hit key verification uses `RDB_MAX_KEY_LEN` storage. | Batch and CTest cache suite passed. |
| KVDB de-dup collision handling | Added same-hash/same-length/same-prefix key pair and re-init readback test. | De-dup table reads the tracked record's full key before treating a fingerprint match as duplicate. | `test_kvdb_basic` passed in batch and CTest. |
| KVDB max key length handling | Cache suite now compiles with `RDB_MAX_KEY_LEN=254`. | `strkey_len()` uses `size_t` scan state and bounded `uint8_t` assignment. | Batch and CTest passed with max-key configuration. |
| TSDB unsupported write granularity | Extended TSDB write-gran test to expect `RDB_ERR_PARAM` for `write_gran=2/3`. | `rdb_tsdb_init()` and `rdb_tsdb_format()` reject `write_gran > 1`. | `test_tsdb_basic` passed in batch and CTest. |
| TSDB epoch-aware query | Added high-time old epoch plus low-time new epoch range query test. | Query skips out-of-range records instead of stopping globally above `to`. | `test_tsdb_basic` passed in batch and CTest. |
| Portability and CI | CMake Debug build failed locally until compiler/toolchain and max-key ABI config were made explicit. | Added portable static assert macro, non-zero disabled cache layout, Windows clang configure path, and perf CI step. | `cmake --build` and `ctest` passed locally. |
| Documentation | Documentation was missing the new explicit TSDB write-gran limit and review validation matrix. | Updated README, CHANGELOG, test plan, architecture, W25QXX guide, and this report. | Markdown changes reviewed locally. |

## Final Validation

- `build\run_all_tests.bat test`: passed 8/8. Latest clean run summary: `test\out\260525-231706-SUMMARY.log`.
- `build\build.bat all test`: passed 8/8.
- `build\build_kvdb_all.bat test`: passed 3/3.
- `build\build_tsdb_all.bat test`: passed 2/2.
- CMake/CTest Debug with LLVM clang: configure, build, and 10/10 tests passed in `cmake-build-codex`.
- `build\build_perf.bat run`: passed, result `test\perf\results_20260525_231750.csv`.
- `test\perf\run_benchmark.bat`: passed, result `test\perf\results_20260525_231802.csv`.

Cloud-server, Docker, Kubernetes, and local-client-to-cloud data interaction
validation were intentionally not executed in this round per user scope. No
claim is made for real remote deployment verification.
