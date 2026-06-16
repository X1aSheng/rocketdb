# RocketDB Comprehensive Review — 260613

## Baseline

- Repository: `G:\rocketdb`
- Branch: `main` at `5c1d8fe`
- All 11 CTest suites PASSED (100%) with zero compilation warnings
- Python `rdbdump` offline verification: 0 anomalies
- Previous reviews: `PROJECT-REVIEW-260517-160748`, `PROJECT-REVIEW-260525-230807`, `PROJECT-REVIEW-260603-142000`, `PROJECT-REVIEW-260604-000000`, `PROJECT-REVIEW-260604-001500`

---

## Findings

### F1 — Version string inconsistency

**Severity**: Medium — Confusing to downstream consumers / API users.

**Files**: `src/rocketdb_kvdb.c:275`, `src/rocketdb.h:977`, `README.md:3`

The `rdb_version()` function returns `0x010100` (v1.1.0), and all source
file headers say `@version 1.1.0`.  However, `README.md` claims
`version-1.2.0` in its badge.

**Fix**: Update `rdb_version()` to return `0x010200` and update all source
headers to `1.2.0`, OR downgrade the README badge to `1.1.0`.  Since the
README already advertises 1.2.0 and the project has had substantial fixes
since the 1.1.0 tag, bumping the code to 1.2.0 is the correct direction.

**Verification**: `ctest -V` still passes; `rdb_version()` returns `0x010200`.

---

### F2 — KVDB sector header CRC comment misleading

**Severity**: Low — Readability/maintainability.

**File**: `src/rocketdb.h:601-606`

The Doxygen comment for `rdb_kv_sector_hdr_t.hdr_crc` says:
> "CRC16 of all 16 header bytes"

But the actual CRC in `rocketdb_kvdb.c:389-390` deliberately EXCLUDES bytes
6-7 (the `hdr_crc` field itself) to avoid self-referential computation:
```c
uint16_t crc = rdb_crc16(&h, 6);                   /* bytes [0..5]  */
crc = rdb_crc16_cont(crc, ((const uint8_t*)&h) + 8, 8); /* bytes [8..15] */
```

The comment should say: "CRC16 of header bytes excluding the hdr_crc field
itself".

**Fix**: Update the comment to accurately describe the CRC coverage.

---

### F3 — `strkey_len` return-value handling inconsistent across APIs

**Severity**: Low — All paths ultimately return an error, but error codes differ.

**Files**: `src/rocketdb_kvdb.c:2506-2517` vs `src/rocketdb_kvdb.c:2748-2757`
and `src/rocketdb_kvdb.c:2862-2870`

In `rdb_kvdb_set()`:
```c
int sr = strkey_len(key, &kl);
if (sr == -1) return RDB_ERR_PARAM;     /* not null-terminated */
if (sr == 1)  return RDB_ERR_TOO_LARGE; /* empty or too long */
```

In `rdb_kvdb_get()` and `rdb_kvdb_delete()`:
```c
if (strkey_len(key, &kl) != 0)
    return RDB_ERR_PARAM;   /* lumps empty, too long, AND not null-terminated */
```

When a key is empty (length < 1), `set()` reports `RDB_ERR_TOO_LARGE` but
`get()` and `delete()` report `RDB_ERR_PARAM`.  Both are errors, but the
inconsistency may confuse debugging.

**Fix**: Standardise all callers to the `set()` pattern (three-way return
handling) for consistent error reporting.  Alternatively, since empty keys
are a programming mistake that should never happen, document why PARAM is
acceptable for get/delete.

---

### F4 — No Docker/K8s deployment support

**Severity**: Medium — Blocks cloud deployment workflow.

The project has zero Docker or Kubernetes files.  For cloud deployment
testing (Task 6 in the original request), a minimal Dockerfile and
docker-compose.yml are needed.  K8s manifests are a stretch goal.

**Fix**: Add a multi-stage Dockerfile that:
  1. Stage 1: Builds the library and test executables using GCC.
  2. Stage 2: A minimal Alpine image with the server binary (if a
     server exists) or the test/example executables for smoke testing.

Note: The core library is designed for embedded use (no `main()` in the
library targets).  A Docker-based deployment implies adding a server
layer or using the examples as integration smoke tests.

---

### F5 — CI workflow `actions/cache@v4` deprecation and build-perf gap

**Severity**: Low — CI stability.

**File**: `.github/workflows/ci.yml:27`

The workflow uses `actions/cache@v4`, which is deprecated.  Also, the
`BUILD_PERF=ON` CMake option is set but the cmake-build path cached key
uses `hashFiles('CMakeLists.txt', '**/*.cmake')`, which won't invalidate
when test sources change (test files are not .cmake files).  This means
CI may use stale builds when only test code changes.

**Fix**: 
1. Upgrade to `actions/cache@v5`.
2. Widen cache key globs to include source files that affect the build.
3. Consider removing the cache for `cmake-build` entirely — Ninja rebuilds
   are fast enough that caching adds complexity without much benefit.

---

### F6 — Interface template CRC/hash code duplicated across example files

**Severity**: Informational — Not a bug, but maintenance burden.

**Files**: `examples/example_kvdb_basic.c:22-67`, `examples/example_tsdb_sensor.c:31-76`

Both example files contain identical CRC-16/MODBUS and FNV-1a hash
implementations.  If the hash or CRC algorithm ever changes, both must
be updated in lockstep along with `tests/sim/sim_crypto.c` and
`interface/rocketdb_interface_template.c`.

**Fix**: Extract the shared CRC/hash into `interface/rocketdb_interface.c`
(or a single `rdb_portable.c`), and have the examples link against it.
However, the examples are intentionally self-contained (single-file for
copy-paste portability), so this may be intentional.  Document the
duplication in `CONTRIBUTING.md` as a known maintenance practice.

---

### F7 — Test output `.gitignore` excludes `tests/out/` but stale logs persist

**Severity**: Low — Developer ergonomics.

**File**: `.gitignore`

The `.gitignore` has `tests/out/` which correctly excludes the output
directory.  However, the `build/run_all_tests.bat` and `build/run_suite.bat`
clean targets use `del /q` with specific patterns, which may leave orphan
files on interrupted runs.

**Fix**: Enhance the clean target to use `rmdir /s /q tests\out` followed
by `mkdir tests\out` for a guaranteed clean state.  Already done manually
in this review session.

---

### F8 — CMakePresets.json `debug-win` uses bare `clang` with no PATH hint

**Severity**: Low — Affects first-time Windows setup.

**File**: `CMakePresets.json`

The `debug-win` preset sets `"CMAKE_C_COMPILER": "clang"` without a PATH
resolution hint.  On systems where `clang` is not in PATH (e.g. a fresh
LLVM install), this fails silently.

**Fix**: The CI already handles this via explicit `-DCMAKE_C_COMPILER=clang`.
Add a note in `CMakePresets.json` that the user must have LLVM in PATH
or override `CMAKE_C_COMPILER` with the full path.

---

### F9 — `rdb_kvdb_init` write_gran validation upper bound

**Severity**: Low — Defensive coding.

**File**: `src/rocketdb_kvdb.c:2189`

```c
if (part->write_gran > 3)
    return RDB_ERR_PARAM;
```

This is correct (`write_gran = 3` → 8-byte granularity), but there's no
explicit `#define` for the maximum.  A named constant (`RDB_WRITE_GRAN_MAX`)
would improve readability and catch future porting errors.

**Fix**: Add `#define RDB_WRITE_GRAN_MAX 3u` in `rocketdb.h` and use it.

---

### F10 — TSDB `rdb_tsdb_init` unconditionally zeroes `db` handle, losing pre-set fields

**Severity**: Low — Only matters if caller pre-populates fields before init.

**File**: `src/rocketdb_tsdb.c:742`

```c
memset(db, 0, sizeof(*db));
```

The `rdb_kvdb_init` also does this.  This is documented behavior ("caller
should pass a zeroed handle"), but if the caller has set `db->erase_cnts`
before calling init, it gets wiped.

**Fix**: Document in the API comment that the handle must be zeroed,
or save/restore the `erase_cnts` pointer across the memset.  The KVDB
side already has the [K-EC-PERSIST fix] for `saved_ec`; TSDB should
do the same for `erase_cnts`.

---

### Summary

| ID | Severity | Area | Requires code change? | Status |
|----|----------|------|----------------------|--------|
| F1 | Medium | Version | Yes (1 line) | ✅ Fixed in `8e15379` |
| F2 | Low | Comment | Yes (1 comment) | ✅ Fixed in `8e15379` |
| F3 | Low | Error handling | Optional (comment only) | ✅ Documented in `0000523` |
| F4 | Medium | Deployment | Yes (new files) | ✅ Added in `0000523` |
| F5 | Low | CI | Yes | ✅ Fixed in `635f8fb` |
| F6 | Info | Maintenance | No | Noted |
| F7 | Low | Build scripts | Yes (clean target) | ✅ Fixed in `635f8fb` |
| F8 | Low | CMake | Yes (comment + binaryDir) | ✅ Fixed in `0000523` |
| F9 | Low | Readability | Yes (named constant) | ✅ Fixed in `8e15379` |
| F10 | Low | Robustness | Not applicable | Reclassified — TSDB erase_cnts passed as param, not stored in handle |

---

## Test Results

All 11 CTest suites executed on Windows 11 with Clang 22.1.5
(before and after all fixes):

| Test | Status | Assertions |
|------|--------|-----------|
| test_kvdb_basic | PASS | 438 |
| test_kvdb_stress | PASS | 10,035 |
| test_kvdb_cache | PASS | 6,160 |
| test_tsdb_basic | PASS | 381 |
| test_tsdb_stress | PASS | 10,538 |
| test_integration | PASS | 52,739 |
| test_fault_injection | PASS | 225 |
| test_example | PASS | 12 |
| KVDB_Basic_Example | PASS | N/A |
| TSDB_Sensor_Example | PASS | N/A |
| rdbdump_offline_verify | PASS | 0 anomalies |

**Total: 11/11 PASSED, 0 warnings, 0 anomalies.**

---

## Remediation Completed

| ID | Fix | Commit | Verification |
|----|-----|--------|-------------|
| F1 | Bump rdb_version() to 0x010200, sync @version tags | `8e15379` | 11/11 CTest pass |
| F2 | Clarify KV sector header CRC comment | `8e15379` | Compile clean |
| F3 | Added doc note on strkey_len behavior differences | `0000523` | N/A (documentation) |
| F4 | Add Dockerfile + docker-compose.yml | `0000523` | `docker build -t rocketdb .` |
| F5 | Update cache action v4→v5, widen key globs | `635f8fb` | CI workflow |
| F7 | Stronger clean targets (rmdir/mkdir) | `635f8fb` | `build\run_all_tests.bat clean` |
| F8 | Fix binaryDir collision, add PATH hint | `0000523` | CMake configure |
| F9 | Add RDB_WRITE_GRAN_MAX constant | `8e15379` | 11/11 CTest pass |
