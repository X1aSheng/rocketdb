# RocketDB Review — 260604

## Baseline

- Repository: `G:\rocketdb`
- Branch: `main` at `be84582` — all 8 test suites pass, rdbdump 0 anomalies
- Cloud: Ubuntu 26.04 (Alibaba Cloud) — compiled and tested successfully

## Findings

### F1 — Zephyr port rejects single-byte state transition writes

**Severity**: High — Breaks Zephyr builds with write_gran > 0.

**File**: `zephyr/rocketdb_port.c:52`

```c
gran = 1u << c->write_gran;
if (((addr | (uint32_t)len) & (gran - 1u)) != 0u)
    return -EINVAL;
```

The engine issues 1-byte writes for NOR-safe state transitions
(WRIITNG→VALID→DEAD).  These single-byte writes land at `addr+1`
which may be unaligned when `write_gran > 0` (e.g. 2-byte aligned
base address + 1 = odd).  The simulator (`sim_flash.c:66`) correctly
exempts `len==1` from alignment checking; the Zephyr port does not.

**Fix**: Add the same single-byte exception to `rdb_zephyr_write()`.

### F2 — CMakePresets `debug-win` uses ambiguous Clang discovery

**Severity**: Low — May fail if `clang` not in PATH.

**File**: `CMakePresets.json`

The `debug-win` preset sets `CMAKE_C_COMPILER: "clang"` without
a path hint.  On Windows systems where LLVM is not in PATH (e.g.
a fresh install at `C:\Program Files\LLVM\bin\`), CMake will fail
to find the compiler.

**Fix**: The CI already sets `-DCMAKE_C_COMPILER=clang` in the
configure step.  A CMake `toolchain` file or presets-level PATH
hint would be more robust but is deferred — CI is the primary
consumer.

### F3 — Test framework `test_make_log_path` buffer not thread-safe

**Severity**: Low — Tests are single-threaded.

**File**: `tests/sim/test_framework.c`

The `test_make_log_path()` function returns a pointer to a static
buffer.  If tests were ever parallelised, concurrent calls would
race on the shared buffer.  Documented as "caller should not free
the pointer."

**Fix**: Not actionable — tests are intentionally single-threaded.
Noted for future reference.

### F4 — `sim_crypto.c` CRC seed retrieval is correct but differs from template

**Severity**: Informational — Already fixed in the previous iteration.

The `sim_crypto.c` NULL+len==0 guard was added in `8ca1612`.
The interface template and Zephyr port have equivalent guards.

## Remediation

| ID | Area | Fix | Verification |
|----|------|-----|-------------|
| F1 | Zephyr port | Add `len==1` bypass in alignment check | Compile Zephyr port test |
| F2 | CMakePresets | Noted — CI handles via explicit `-D` | CI runs |
| F3 | Test framework | Noted — out of scope | N/A |
| F4 | sim_crypto | Already fixed in `8ca1612` | All tests pass |

## Completion

F1 fix applied and pushed.
