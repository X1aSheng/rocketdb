# RocketDB Test Methodology Reference

## 1. Overview

The RocketDB test system verifies correctness, reliability, and durability of a dual-mode embedded Flash storage engine under conditions that closely mirror real NOR Flash hardware behavior.

**Version**: v1.3.0 | **Language**: C99 | **Flash Model**: W25Q128-compatible NOR

---

## 2. Test Infrastructure

### 2.1 Component Architecture

```
┌─────────────────────────────────────────────────────────────┐
│                    Test Executables (8)                      │
│  kvdb_basic  tsdb_basic  kvdb_stress  tsdb_stress           │
│  integration  fault_injection  example  kvdb_cache           │
├─────────────────────────────────────────────────────────────┤
│                 Test Framework                               │
│  test_framework.c/h — assertion engine, reporting, runner    │
├─────────────────────────────────────────────────────────────┤
│                 Simulation Layer                              │
│  sim_flash.c/h    — NOR Flash behavioral model               │
│  sim_fault.c/h    — Fault injection engine                   │
│  sim_trace.c/h    — Operation + parameter tracing            │
│  sim_dist.c/h     — Deterministic random distributions       │
│  sim_crypto.c     — CRC16 / Hash16 implementations           │
├─────────────────────────────────────────────────────────────┤
│                 Engine Under Test                             │
│  rocketdb_kvdb.c  — KVDB log-structured engine               │
│  rocketdb_tsdb.c  — TSDB ring-buffer engine                  │
│  rocketdb.h       — Public API + on-flash data structures    │
└─────────────────────────────────────────────────────────────┘
```

### 2.2 Build Model

Each test executable is a standalone host-side binary linking against:
- The test framework (assertion engine, runner)
- The simulation layer (flash model, fault injection, tracing, distributions, crypto)
- The RocketDB engine source (KVDB + TSDB, compiled unchanged from embedded target)

No hardware dependencies — all tests run on the development host.

---

## 3. NOR Flash Behavioral Model (`sim_flash`)

The simulator enforces all physical constraints of W25Q-series NOR Flash. The RocketDB engine source code (`rocketdb_kvdb.c`, `rocketdb_tsdb.c`) is compiled directly into the test executable without modification — it calls `rdb_flash_ops_t` callbacks that are implemented by `sim_flash`.

### 3.1 Geometry Configuration

| Parameter | Configurable | Default | Constraint |
|-----------|-------------|---------|------------|
| Flash size | Yes | 128 KB | Power of 2 × sector_size |
| Sector size | Yes | 4096 B | Power of 2, ≥ 4096 |
| Page size | Yes | 256 B | Stored but not enforced per-write |
| Write granularity | Yes, 0–3 | 0 (1 B) | 0=1B, 1=2B, 2=4B, 3=8B |

### 3.2 Initial State

All flash bytes are initialized to `0xFF` (erased state). This matches factory-fresh NOR Flash — every bit reads as 1 until programmed to 0.

### 3.3 Write Constraints (NOR Bit-Flip Physics)

```
┌──────────────────────────────────────────────────────────────┐
│ NOR Flash Rule: bits can only transition 1 → 0 by writing.  │
│                 To restore 0 → 1, the entire sector must     │
│                 be erased (set all bytes to 0xFF).           │
└──────────────────────────────────────────────────────────────┘
```

The simulator enforces this **on every byte**:

```c
// sim_flash_write — per-byte enforcement
uint8_t old = f->mem[addr + i];
uint8_t nw  = buf[i];
if ((~old) & nw) {
    // Attempting to raise a bit from 0→1 without prior erase
    return -1;
}
f->mem[addr + i] &= nw;  // Only 1→0 transitions committed
```

**Write alignment**: If `len > 1`, both `addr` and `len` must be multiples of the write granularity (1, 2, 4, or 8 bytes).

**Write scope**: A single write call may span multiple bytes within one page. The simulator does not enforce page boundaries — RocketDB engine code already guarantees single-write calls do not cross page boundaries.

### 3.4 Partial Write and Power Loss

Writes are committed **byte-by-byte**. If a power-loss fault triggers at byte N:
- Bytes 0 through N-1 are already committed to the flash array
- The write returns failure
- This models real NOR behavior: the SPI bus may be interrupted mid-transfer, leaving partially written data on the physical die

This is critical for RocketDB's recovery design — the engine must handle `WRITING` (0xFF) state records with partial data.

### 3.5 Sector Erase

```
┌──────────────────────────────────────────────────────────────┐
│ Erase: sets all bytes in a 4KB sector back to 0xFF.          │
│        The engine calls this before writing a new sector     │
│        header, and during GC to reclaim garbage space.       │
└──────────────────────────────────────────────────────────────┘
```

- Address must be sector-aligned (multiple of `sector_size`)
- An erase failure (injected via fault system) leaves the sector in an indeterminate state — the engine's "three-point erase verification" (checking bytes at start, middle, end of sector for 0xFF) handles this
- Each sector accumulates `erase_cnt` — this tracks the total number of P/E cycles, critical for wear-leveling verification

### 3.6 Write Granularity Matrix

RocketDB supports four write granularity values, matching different NOR Flash command sets:

| wg | Alignment | Flash Byte | Typical NOR Command |
|----|-----------|-----------|---------------------|
| 0 | 1 B | Any address | Page Program (standard) |
| 1 | 2 B | 2-byte aligned | 16-bit parallel NOR |
| 2 | 4 B | 4-byte aligned | 32-bit parallel NOR |
| 3 | 8 B | 8-byte aligned | 64-bit burst mode |

All KVDB tests run across wg ∈ {0, 1, 2, 3}. TSDB tests run across wg ∈ {0, 1} (wg ≥ 2 is structurally incompatible with TSDB's 20-byte header layout — see [test_tsdb_basic.c](test_tsdb_basic.c) `ts_write_gran_matrix` for details).

---

## 4. Sector Geometry and Capacity

The actual writable data capacity of each sector depends on sector header size, write granularity alignment, and record header overhead. These values are **computed at runtime** by the test framework and logged at the start of boundary and stress tests.

### 4.1 KVDB Sector Geometry

```
sector_size = 4096 B

┌──────────────┬──────────────────────────────────────────────┐
│ sector_hdr   │ records...                                    │
│ 16 B         │◄── data_cap = 4080 B (wg=0) ───────────────►│
└──────────────┴──────────────────────────────────────────────┘

record = 16 B (record_hdr) + ALIGN_UP(key_len, gran) + ALIGN_UP(val_len, gran)

max_val(key_len) = data_cap - 16 - ALIGN_UP(key_len, gran)
                 = 4080 - 16 - ALIGN_UP(key_len, gran)    [wg=0]
```

**Example (wg=0):**

| Key Length | Key Aligned | Max Value | Total Record |
|-----------|------------|-----------|-------------|
| 1 B | 1 B | 4063 B | 4080 B |
| 2 B ("MK") | 2 B | 4062 B | 4080 B |
| 32 B (max) | 32 B | 4032 B | 4080 B |

**Key insight**: `RDB_MAX_VAL_LEN = 4095` is a compile-time API limit, not the actual writable maximum. The actual max is always less due to:
- Sector header: 16 B
- Record header: 16 B
- Key bytes (aligned): 1 – 32 B
- Value alignment waste: 0 – `(gran - 1)` B

### 4.2 TSDB Sector Geometry

```
sector_size = 4096 B

┌──────────────┬──────────────────────────────────────────────┐
│ sector_hdr   │ records...                                    │
│ 20 B         │◄── data_cap = 4076 B (wg=0) ───────────────►│
└──────────────┴──────────────────────────────────────────────┘

record = 12 B (record_hdr) + ALIGN_UP(data_len, gran)

max_data_len (wg=0): 4064 B  →  record_sz = 12 + 4064 = 4076 B (exactly fills data_cap)
```

TSDB's `max_data_len` is computed in `rdb_tsdb_init()` by iterating downward from `data_cap - 12` until `ALIGN_UP(n, gran) ≤ data_cap - 12`.

### 4.3 Geometry Tracing

`trace_kvdb_geometry()` and `trace_tsdb_geometry()` (in [sim_trace.c](sim_trace.c)) emit a formatted geometry table at the start of boundary and mixed-length stress tests. This ensures every test run documents the exact per-configuration capacity limits.

---

## 5. Fault Injection Engine (`sim_fault`)

The fault injection system models real-world Flash failure modes with programmable triggers.

### 5.1 Fault Types

| Fault | Models | Effect |
|-------|--------|--------|
| `READ_FAIL` | Degraded cell, bus error | `sim_flash_read` returns -1 |
| `WRITE_FAIL` | Write disturb, worn cell | `sim_flash_write` returns -1, partial write committed |
| `ERASE_FAIL` | Worn sector, Vpp issue | `sim_flash_erase` returns -1, sector indeterminate |
| `POWER_LOSS` | Supply dropout, brownout | `sim_flash_write` aborts at byte N (bytes 0..N-1 committed) |
| `DATA_CORRUPT` | Alpha particle, read disturb | Post-write byte corruption at target address |
| `BIT_FLIP` | NAND-like bit error | Single bit inversion (0→1) in flash array |

### 5.2 Trigger Modes

| Mode | Use Case |
|------|----------|
| `COUNT` | "Inject fault on the Nth write/erase/read" — precise timing |
| `PROBABILITY` | "Inject fault with P% chance per operation" — random stress |
| `ADDRESS` | "Inject fault for operations in address range [A, B]" — sector targeting |
| `PATTERN` | "Inject fault when data matches pattern" — mode-specific testing |

### 5.3 Convenience API

```c
// Precise: fail on the Nth write
fault_quick_write_fail(&fctx, nth);

// Probabilistic: P% chance per write
fault_quick_write_fail_probability(&fctx, pct);

// Power loss: abort at byte M of the Nth write
fault_quick_power_loss(&fctx, nth_write, byte_offset);

// Regional: P% fail rate in address range
fault_quick_region_fail(&fctx, addr_start, addr_end, pct);

// Data corruption after write
fault_quick_corrupt_data(&fctx, addr, len, pattern);
```

### 5.4 Power Loss Precision

Power loss is the most timing-sensitive fault. The simulator models it at **per-byte granularity** within a write:

1. Before write: header is on flash (magic + key_len + val_len)
2. During write: data bytes 0..N-1 committed, byte N triggers power loss
3. After power loss: flash contains a `WRITING` (0xFF state) record with partial data

RocketDB's recovery logic handles this: on next init, `WRITING` records are CRC-checked — if CRC matches, promoted to `VALID`; if CRC fails, marked `DEAD`.

---

## 6. Trace System (`sim_trace`)

The trace system provides configurable verbosity for post-mortem analysis.

### 6.1 Verbosity Levels

| Level | Name | Content |
|-------|------|---------|
| 0 | Off | Test names + pass/fail only |
| 1 | Events | + Format, init, GC events, rotation events, geometry tables |
| 2 | Operations | + All flash reads/writes/erases, per-sector metadata |
| 3 | Hex Dump | + Full hex dumps of flash data before/after each operation |

### 6.2 Key Trace Functions

| Function | Output |
|----------|--------|
| `trace_kvdb_snapshot()` | Full KVDB state: active_sec, write_seq, live_bytes, write_off, stats |
| `trace_tsdb_snapshot()` | Full TSDB state: head_sec, tail_sec, head_off, max_data_len, stats |
| `trace_kvdb_gc_event()` | Per-sector GC table: status, used%, garb%, freeB, erases, seq |
| `trace_tsdb_rot_event()` | Per-sector rotation table: role (HEAD/TAIL/DATA/FREE), erases, freeB |
| `trace_kvdb_geometry()` | KVDB sector geometry: data_start, data_cap, max_val by key length |
| `trace_tsdb_geometry()` | TSDB sector geometry: data_start, data_cap, max_data_len, record_sz |
| `trace_kvdb_sector_summary()` | Post-test per-sector summary (called on every test completion) |
| `trace_tsdb_sector_summary()` | Post-test per-sector summary |

---

## 7. Deterministic Random Distributions (`sim_dist`)

All randomized test workloads use deterministic PRNGs with logged seeds. This ensures:
- **Reproducibility**: any test failure can be replayed with the same seed
- **Coverage diversity**: each seed explores different state space paths
- **No host entropy dependency**: tests produce identical results on any platform

Supported distributions:
- **Uniform**: flat distribution over [min, max]
- **Gaussian**: normal distribution (for key length Monte Carlo)
- **Power-law**: skewed distribution (for value length diversity)

---

## 8. Test Suite Catalog

### 8.1 Test Suite: `test_kvdb_basic` (11 cases, 2,976 assertions)

| Test | Section | Hardware Focus |
|------|---------|----------------|
| `kv_set_get_basic` | T-300 | Write + read-back with CRC verification |
| `kv_set_update` | T-300 | Log-structured overwrite: new record + old marked DEAD |
| `kv_delete_exists` | T-300 | Multi-copy invalidation (all VALID copies → DEAD) |
| `kv_get_too_large` | T-300 | Buffer-too-small detection (out_len returns actual size) |
| `kv_write_gran_matrix` | T-306 | **All 4 write granularities**: 1/2/4/8 byte alignment |
| `kv_seq_wrap_recovery` | T-302 | 32-bit sequence wrap-around at 0xFFFFFFFF → 0 |
| `kv_mixed_lengths` | T-307 | Gaussian key + power-law value distributions, 8 rounds |
| `kv_corrupt_header_skip` | T-304 | Corrupt record header: scan skips by 16B step, rest survives |
| `kv_init_format_verify` | TC-KV-01 | Erase count monotonicity, header write-once, seq initialization |
| `kv_max_boundaries` | TC-X-02 | **Geometry-computed max val**: precise boundary at max_val |
| `kv_capacity_accounting` | TC-X-03 | Cross-check: live_bytes == space_info.used after set/delete mix |

### 8.2 Test Suite: `test_tsdb_basic` (5 cases, 2,851 assertions)

| Test | Section | Hardware Focus |
|------|---------|----------------|
| `ts_basic_append_query` | T-310 | Append 200 records, query full range, get_latest/get_oldest |
| `ts_epoch_query_integrity` | T-311 | Epoch reset: cross-epoch query returns both epochs' data |
| `ts_recount_jitter` | T-314 | Total count calibration every full ring cycle |
| `ts_write_gran_matrix` | — | **WG 0 and 1**: append/query at each granularity |
| `ts_max_boundaries` | TC-X-02 | Write at computed max_data_len, TOO_LARGE at max+1 |

### 8.3 Test Suite: `test_kvdb_stress` (6 cases, 4,686 assertions)

| Test | Section | Hardware Focus |
|------|---------|----------------|
| `kv_gc_stress_100` | T-301 | **>=100 GC cycles**: per-event sector tables, GC statistics |
| `kv_iter_after_gc` | T-303 | Iterator returns latest values after multiple GC rounds |
| `kv_iter_busy_on_modify` | T-303 | ERR_BUSY when DB modified during iteration |
| `kv_power_loss_recovery` | T-305 | **Power loss during write**: re-init recovers K0 and K1, PL is absent |
| `kv_corrupt_sector_recovery` | TC-X-04 | Corrupt sector header → init detects and reclaims |
| `kv_mixed_value_stress` | TC-X-05 | **1..4095B random values**: 20 keys, >=13 GC, distribution histogram |

### 8.4 Test Suite: `test_tsdb_stress` (5 cases, 2,749 assertions)

| Test | Section | Hardware Focus |
|------|---------|----------------|
| `ts_rotation_stress` | T-313 | **>=100 rotations**: per-event rotation tables |
| `ts_append_fail_once` | T-312 | **Write failure during append**: DB usable, next append succeeds |
| `ts_crc_corruption` | T-315 | **Data CRC corruption**: get_latest → ERR_CRC, query → data=NULL |
| `ts_degraded_active_recovery` | T-316 | **Seal power-loss**: corrupt sealed header → init recovers data |
| `ts_mixed_payload_stress` | TC-X-05 | **1..max_data_len random**: piecewise distribution, stamp verification |

### 8.5 Test Suite: `test_integration` (6 cases, 25,508 assertions)

| Test | Section | Hardware Focus |
|------|---------|----------------|
| `kv_gc_cycles_stress` | T-400 | KVDB side: >=100 GC across full partition |
| `ts_rotation_cycles_stress` | T-400 | TSDB side: >=100 rotation across full partition |
| `mixed_workload` | T-401 | **KV+TS simultaneous**: 10,000 ops, 55% set / 20% get / 10% del / 15% TS append |
| `kv_power_loss_stress` | T-402 | **50 power-loss iterations**: set → crash → init → verify |
| `ts_power_loss_stress` | T-403 | **50 power-loss iterations**: append → crash → init → verify |
| `wear_heatmap` | T-404 | **>=100 GC + >=100 rotation**: per-sector erase count distribution, min/max/avg |

### 8.6 Test Suite: `test_fault_injection` (7 cases, 66 assertions)

Validates that every fault type is correctly detected and propagated through the flash abstraction layer, and that the engine's error handling paths (corrupt sectors, flash errors, CRC errors) function correctly.

### 8.7 Test Suite: `test_example` (2 cases, 27 assertions)

Minimal smoke test demonstrating test framework usage. Verifies the framework's basic assertion macros and reporting.

---

### 8.8 Test Suite: `test_kvdb_cache` (8 cases, 6,153 assertions)

New-feature validation suite covering KVDB key-to-address cache, TSDB safety fixes, and recount optimization. All tests exercise varied-length data via `make_value()` with 5 deterministic categories (tiny 1-4B / small 8-32B / medium 64-128B / large 200-255B / max 250-255B).

| Test | Section | Focus |
|------|---------|-------|
| `kv_cache_write_read_delete` | KVDB-Cache | 200 keys x 5 length categories, write/read/delete with cache hit verification |
| `kv_cache_hot_key_update` | KVDB-Cache | Hot-key 500 updates with cycling value sizes, cache occupancy trace |
| `kv_cache_gc_stress` | KVDB-GC | 120 keys x 5 GC rounds, varied-length stress with cache state tracing |
| `ts_append_query_ring` | TSDB-Recount | 2000 appends across 32-sector ring (1-32B payloads) |
| `init_format_large_sectors` | Init | Large-sector init/format verification (KV 64 + TS 32 sectors) |
| `ts_safety_recover_faults` | TSDB-Safety | Write-fault recovery: mark_dead + head_off advancement |
| `kv_cache_gc_migration` | KVDB-GC | 100 keys GC migration with cache consistency after migration |
| `kv_cache_collision_stress` | KVDB-Cache | 100-key collision stress (conditional: `RDB_KV_CACHE_SIZE > 0`) |

**Trace output format** is unified across all test files:
- `[KV-WRITE] key=%s vsz=%u` / `[KV-READ] key=%s` / `[KV-DEL] key=%s`
- `[TS-APPEND] time=%u dlen=%u`
- `[CACHE]` -- cache occupancy reporting at key test phases

---

## 9. How Tests Exercise Hardware Constraints

### 9.1 Erase

| Aspect | How Tested |
|--------|-----------|
| Sector must be erased before write | sim_flash_write rejects 0→1 bits; every test indirectly verifies |
| Erase count tracking | `kv_init_format_verify` checks monotonicity after multi-format |
| Erase failure recovery | `kv_corrupt_sector_recovery`: erased sector→header corrupt→reclaim |
| Wear distribution | `wear_heatmap`: per-sector erase count distribution after ≥80 GC+rotation |
| Three-point erase verification | Engine handles partial erase (power loss mid-erase) |

### 9.2 Sequential (Log-Structured) Write

| Aspect | How Tested |
|--------|-----------|
| Append-only within sector | Engine never overwrites; every KVDB write produces new record |
| Old record invalidation | `kv_delete_exists`: all copies marked DEAD; exists→NOT_FOUND |
| write_off monotonic advancement | `kv_init_format_verify`: write_off at data_start after init |
| Sector fill -> GC trigger | `kv_gc_stress_100`: 20 keys loop until GC count >= 100 |

### 9.3 Sector (4KB) Write

| Aspect | How Tested |
|--------|-----------|
| Sector header write-once per lifecycle | `kv_init_format_verify`: create_seq non-zero for ACTIVE/SEALED |
| Sector seal protocol | `ts_degraded_active_recovery`: count→end_off→hdr_crc three-step |
| Rotation across sectors | `ts_rotation_stress`: ring buffer advances, count stays consistent |
| Cross-sector GC migration | `kv_mixed_value_stress`: records migrate between sectors during GC |
| Per-sector geometry limits | `kv_max_boundaries`, `ts_max_boundaries`: exact boundary testing |

### 9.4 Power Loss at Critical Points

| Drop Point | Test | Verified Behavior |
|-----------|------|-------------------|
| During record data write | `kv_power_loss_recovery` | WRITING+CRC mismatch → DEAD, existing data survives |
| During GC migration | `kv_power_loss_stress` (50 iterations) | Re-init recovers, fixup_stale cleans duplicates |
| During append | `ts_power_loss_stress` (50 iterations) | Re-init, new append succeeds, data readable |
| During seal (CRC not written) | `ts_degraded_active_recovery` | Sector degraded to ACTIVE, data recovered via scan |
| After format, no records | `kv_seq_wrap_recovery` | write_seq correctly restored from sector headers |

### 9.5 Data Corruption

| Corruption Type | Test | Engine Response |
|----------------|------|-----------------|
| Record header (magic trashed) | `kv_corrupt_header_skip` | Skip 16 bytes (corrupt_skip), rest readable |
| Sector header magic corrupted | `kv_corrupt_sector_recovery` | CORRUPT → erase → ERASED, data survives |
| Data payload corruption | `ts_crc_corruption` | get_latest → ERR_CRC, query → data=NULL in callback |
| Sealed sector header CRC bad | `ts_degraded_active_recovery` | Degrade to ACTIVE, scan for data |

### 9.6 Write Granularity Alignment

| Aspect | How Tested |
|--------|-----------|
| All 4 granularities (KVDB) | `kv_write_gran_matrix`: set/update/get at wg=0,1,2,3 |
| All supported granularities (TSDB) | `ts_write_gran_matrix`: append/query at wg=0,1 |
| Alignment padding (0xFF) | Implicit: engine writes 0xFF to alignment gaps |
| Address alignment enforcement | sim_flash_write rejects misaligned addresses when len>1 |

---

## 10. Running Tests

### 10.1 Build & Run All Suites

```bash
# Compile all object files
cd g:/rocketdb

clang -Wall -Wextra -std=c99 -O2 -g -D_CRT_SECURE_NO_WARNINGS \
    -Isrc -Itests/sim -c src/rocketdb_kvdb.c -o tests/out/rocketdb_kvdb.o
clang -Wall -Wextra -std=c99 -O2 -g -D_CRT_SECURE_NO_WARNINGS \
    -Isrc -Itests/sim -c src/rocketdb_tsdb.c -o tests/out/rocketdb_tsdb.o
clang -Wall -Wextra -std=c99 -O2 -g -D_CRT_SECURE_NO_WARNINGS \
    -Isrc -Itests/sim -c tests/sim/sim_flash.c -o tests/out/sim_flash.o
# ... (compile all sim_*.c and test_*.c similarly)

# Link and run each suite (from project root for correct log paths)
clang tests/out/test_kvdb_basic.o tests/out/sim_flash.o tests/out/sim_fault.o \
    tests/out/test_framework.o tests/out/sim_trace.o tests/out/sim_dist.o \
    tests/out/sim_crypto.o tests/out/rocketdb_kvdb.o tests/out/rocketdb_tsdb.o \
    -o tests/out/test_kvdb_basic.exe

./tests/out/test_kvdb_basic.exe   # Trace log -> tests/out/<timestamp>-kvdb_basic.log
```

### 10.2 Log Files

Each test suite produces a timestamped trace log in `tests/out/<YYMMDD-HHMMSS>-<name>.log`. Log path is relative to the working directory — run executables from the project root.

The full batch runner also generates deterministic KVDB/TSDB Flash dump fixtures, verifies them with `tools/rdbdump`, and exports timestamped offline-analysis datasets under `tests/out/rdbdump_export/<YYMMDD-HHMMSS>/`.

### 10.3 Selecting Verbosity

```c
// In test main():
test_config_t config = {
    .log_file = fopen(test_make_log_path("kvdb_basic"), "w"),
    .verbose = 3,    // 0=off, 1=events, 2=ops, 3=hex
    .stop_on_fail = 0,
    .filter = NULL   // or "KVDB" to run only KVDB tests
};
```

### 10.4 Running Individual Tests

```bash
# Run a specific test case
./tests/out/test_kvdb_basic.exe --filter "kv_max_boundaries"

# Stop on first failure
# (set .stop_on_fail = 1 in config)
```

---

## 11. Interpreting Results

### 11.1 Pass/Fail Report

Every test suite produces:
```
========================================
Test Report
========================================

Test Cases:
  Total:   11       Passed:  11
  Failed:  0        Skipped: 0

Assertions:
  Total:   2976     Passed:  2976
  Failed:  0

Time:
  Elapsed: 45 ms    ✓ ALL TESTS PASSED
========================================
```

### 11.2 Key Metrics in Trace Logs

| Metric | Location | Significance |
|--------|----------|--------------|
| **GC runs** | `trace_kvdb_stats` → `gc_runs` | Number of GC cycles; validates GC liveness |
| **Sector rotations** | `trace_tsdb_stats` → `sector_rotations` | Number of ring cycles; validates ring progression |
| **Erase counts** | `wear_heatmap` output, per-sector tables | min/max/avg distribution; validates wear leveling |
| **Flash errors** | `flash_errors` in stats | Non-zero only when faults injected |
| **CRC errors** | `crc_errors` in stats | Non-zero only when corruption injected |
| **Records lost** | `records_lost` in TSDB stats | Records overwritten by ring advance |
| **Data gaps** | `data_gaps` in TSDB stats | Non-zero → power-loss recovery detected |
| **Live bytes** | `live_bytes` vs `space_info.used` | Must always match; capacity accounting invariant |

### 11.3 Geometry Tables

Each boundary and stress test emits geometry tables that document the exact per-configuration capacity. Compare these against expected values for the given sector_size and write_gran.

---

## 12. Adding New Tests

### 12.1 Test Case Template

```c
TEST_CASE(my_new_test, "KVDB", "Brief description of what is tested")
{
    (void)ctx;

    // 1. Initialize hardware simulation
    TEST_ASSERT_RDB_OK(kv_reset(DEFAULT_WG));

    // 2. Output geometry (for boundary/stress tests)
    trace_kvdb_geometry(&g_trace, &g_db);

    // 3. Exercise the engine
    TEST_ASSERT_RDB_OK(rdb_kvdb_set(&g_db, "key", "val", 3));

    // 4. Verify results
    uint8_t out[8]; uint16_t out_len = 0;
    TEST_ASSERT_RDB_OK(rdb_kvdb_get(&g_db, "key", out, sizeof(out), &out_len));
    TEST_ASSERT_EQ(out_len, 3u);

    return 0;
}
```

### 12.2 Checklist

- [ ] Use `kv_reset()` / `ts_reset()` to get a clean flash state
- [ ] Call `trace_kvdb_geometry()` / `trace_tsdb_geometry()` for boundary/stress tests
- [ ] Use fault injection for error-path testing — call `fault_clear_rules()` after
- [ ] Use deterministic PRNG for random workloads — log the seed
- [ ] Verify both success and error return codes
- [ ] Clean up allocated memory (if any)
- [ ] Register the test case in `main()` via `test_register_case(s, &test_case_my_new_test)`
