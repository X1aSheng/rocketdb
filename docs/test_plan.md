# RocketDB Test Plan
## 1. Purpose
This plan translates . It includes a coverage assessment, a missing-scenarios list, and a redesigned test suite that is repeatable and measurable.

## 2. Inputs And Version Context
1. Code under test: `rocketdb.h`, `rocketdb_kvdb.c`, `rocketdb_tsdb.c`.

## 3. Coverage Assessment vs. Requirements

### 3.1 Requirement Mapping
Requirement groupings inferred from `rocketdb_test.txt`:
1. Flash model and partitions: W25Q128, 4 partitions of 32KB each (KVDB1, KVDB2, TSDB1, TSDB2), no index.
2. Minimum GC pressure: each partition must trigger at least 100 GC/rotations and remain functional.
3. Reliability under mixed operations: write, update, delete, query, rotation, GC migration, wrap-around.
4. Power-loss consistency and recovery.
5. Time-series epoch reset and query correctness.
6. Write-granularity compliance across 1/2/4/8 bytes.
7. Structural RAM footprint and on-flash layout constraints.
8. Performance and jitter: avoid large blocking scans in hot paths.
9. Data corruption handling: CRC errors, partial writes, bad headers.
10. Large/small record handling, random key/value lengths.

### 3.2 Coverage Gaps (Missing Tests)
1. No automated GC stress across all four partitions with >=100 GC cycles each.
2. No multi-write-granularity test matrix (1/2/4/8 bytes) for KVDB and TSDB.
3. No epoch reset correctness tests (query must not truncate across epochs).
4. No append error path tests verifying head offset advancement or DEAD marking.
5. No sequence wrap-around tests for KVDB write sequence during init and recovery.
6. No iterator correctness under GC (KVDB iter_gen invalidation).
7. No power-loss injection tests across critical steps (header write, data write, commit write).
8. No CRC corruption injection tests (data CRC mismatch handling).
9. No randomized key/value length mixed workload tests.
10. No wear-leveling distribution verification (erase counts spread within threshold).
11. No boundary record size tests for `RDB_MAX_VAL_LEN` and `RDB_MAX_TS_DATA_LEN`.
12. No sector-degradation tests for TSDB ACTIVE sectors in ring body.
13. No query-range tests with `from > to`, `to = 0`, and boundary timestamps.

## 4. Redesign: Executable Test Suite

### 4.1 Test Infrastructure
1. Flash simulator implementing W25Q128 constraints:
   - 4KB sector erase
   - 256B page program behavior
   - Write granularity selectable per run (1/2/4/8)
   - 1->0 only writes unless erased
2. Fault injection hooks:
   - Fail on Nth write/erase/read
   - Corrupt byte ranges after write
   - Power-loss interruption at labeled steps
3. Deterministic random generator with seed logging.
4. Trace logging:
   - Sector status, write offsets, live/garbage bytes
   - GC victim selection details
   - erase counts per sector
   - TSDB head/tail positions, time_base, total_count

### 4.2 Global Test Matrix
Run each test suite for:
1. Write granularity: 1, 2, 4, 8 bytes
2. Partitions: KVDB1, KVDB2, TSDB1, TSDB2 (32KB each)
3. Payload size mix: small (<=16B), medium (32-256B), large (near sector capacity)
4. Key/value length mix: random, skewed, max-length edge

### 4.2.1 Data Distributions And Random Strategy
Use a deterministic PRNG with logged `seed` and per-test `stream_id`.

**Key Length Distribution**
1. 50%: 1-8 bytes (short keys)
2. 30%: 9-32 bytes (typical)
3. 15%: 33-63 bytes (upper normal)
4. 5%: `RDB_MAX_KEY_LEN` (max edge)

**KV Value Length Distribution**
1. 40%: 0-32 bytes
2. 40%: 33-256 bytes
3. 15%: 257-1024 bytes
4. 4%: 1025-`RDB_MAX_VAL_LEN - 1`
5. 1%: `RDB_MAX_VAL_LEN` (max edge)

**TS Data Length Distribution**
1. 40%: 1-32 bytes
2. 40%: 33-256 bytes
3. 15%: 257-1024 bytes
4. 4%: 1025-`max_data_len - 1`
5. 1%: `max_data_len` (max edge)

**Operation Mix (KVDB)**
1. 60%: set new key (insert)
2. 25%: set existing key (update)
3. 10%: get random key (read)
4. 5%: delete key

**Operation Mix (TSDB)**
1. 80%: append
2. 15%: query (random range)
3. 5%: get_latest/get_oldest

**Timestamp Generation (TSDB)**
1. 70%: strictly increasing (`last_time + delta`, delta 1-100)
2. 20%: equal or decreasing input to force monotonic correction
3. 10%: epoch-reset scenario every N appends (N in 100-500 range)

**Key/Value Content**
1. 70%: random bytes
2. 20%: low-entropy patterns (all 0x00, all 0xFF, 0xAA/0x55)
3. 10%: structured payloads with embedded sequence counters

### 4.2.2 Executable Test Case Table
Each entry below is a concrete, runnable test with explicit configuration. Use the global matrix for write_gran (1/2/4/8) and run each test across all four partitions unless stated otherwise.

1. **TC-KV-01 Empty Init/Format**
   Setup: fresh 0xFF flash, KVDB1 only
   Steps: init, verify active sector; format; verify erase count increments
   Pass: active_sec set, write_off at data_start, headers valid

2. **TC-KV-02 Basic Set/Get**
   Setup: 100 unique keys, random lengths per 4.2.1
   Steps: set all, get all
   Pass: all reads return correct data, no CRC errors

3. **TC-KV-03 Update/GC Accounting**
   Setup: 20 hot keys, fixed value length 32B
   Steps: update each key 200 times
   Pass: live_bytes bounded, garbage_bytes increases, no RDB_ERR_FULL

4. **TC-KV-04 Delete All Copies**
   Setup: 50 keys, multiple updates
   Steps: delete each key, then exists/get
   Pass: delete returns OK, exists/get return NOT_FOUND

5. **TC-KV-05 GC Stress 100+**
   Setup: 20 keys, 32B values, KVDB1
   Steps: loop writes until GC count >= 100
   Pass: gc_runs >= 100, no deadlock, no corruption

6. **TC-KV-06 Seq Wrap Recovery**
   Setup: prefill flash with seq near 0xFFFFFFFF
   Steps: init, write new records
   Pass: new seq > prior logical max, ordering correct

7. **TC-KV-07 Iterator Under GC**
   Setup: 100 keys, start iterator
   Steps: trigger GC mid-iteration, call iter_next
   Pass: iter_next returns RDB_ERR_BUSY

8. **TC-KV-08 Corrupt Header Skip**
   Setup: inject bad magic/key_len in middle of sector
   Steps: init and get/iter
   Pass: scan skips corrupt record, no crash, data after remains readable

9. **TC-KV-09 Power-Loss Recovery**
   Setup: fault injection at header/data/commit
   Steps: crash, re-init
   Pass: WRITING records repaired or marked DEAD, DB usable

10. **TC-TS-01 Append/Query Basic**
    Setup: append 1000 records, monotonic time
    Steps: query full range
    Pass: all records returned in order, CRC valid

11. **TC-TS-02 Epoch Query Integrity**
    Setup: append T increasing, reset epoch, append smaller times
    Steps: query range spanning both epochs
    Pass: no truncation, records from both epochs returned

12. **TC-TS-03 Append Failure Handling**
    Setup: inject write failure at header/data/commit
    Steps: append, then append again
    Pass: no AND-collision, head_off advanced or DEAD marked

13. **TC-TS-04 Rotation 100+**
    Setup: fixed record size, fill ring until 100 rotations
    Steps: continue appending
    Pass: tail advances, total_count consistent, no corruption

14. **TC-TS-05 Recount Jitter**
    Setup: measure append latency over multiple ring cycles
    Steps: collect max latency
    Pass: latency within defined budget or flagged

15. **TC-TS-06 CRC Corruption**
    Setup: corrupt data payload after write
    Steps: query affected range
    Pass: callback receives NULL data, crc_errors increments

16. **TC-TS-07 Degraded ACTIVE Sector**
    Setup: simulate power loss during seal, leaving ACTIVE in body
    Steps: init, query/get_oldest
    Pass: ts_active_info recovers time_base and data is accessible

17. **TC-X-01 RAM Footprint**
    Setup: static asserts on struct sizes and meta buffers
    Steps: compile-time checks
    Pass: sizes match expected, no malloc

18. **TC-X-02 Max Size Boundaries**
    Setup: KV max key/value, TS max data len
    Steps: write and read
    Pass: success or correct TOO_LARGE errors

19. **TC-X-03 Capacity Accounting**
    Setup: mix of sets and deletes
    Steps: compare space_info vs computed usage
    Pass: reported totals within expected tolerance

20. **TC-X-04 Corrupt Sector Recovery**
    Setup: corrupt sector header
    Steps: init
    Pass: marked CORRUPT or reclaimed to ERASED

### 4.3 KVDB Tests

**KVDB-INIT-01: Empty init and format**
1. Start with all 0xFF flash.
2. Init, verify one ACTIVE sector, write_off at data_start.
3. Format, verify erase counts increment and header written once per sector.

**KVDB-WRITE-02: Basic set/get**
1. Write 100 unique keys.
2. Read all, verify CRC and value integrity.

**KVDB-UPDATE-03: Overwrite and garbage accounting**
1. Update same keys repeatedly.
2. Validate garbage_bytes increases and live_bytes stays bounded.

**KVDB-DELETE-04: Delete all copies**
1. Write keys, update keys, delete keys.
2. Verify no VALID copies remain.

**KVDB-GC-05: GC stress >=100 cycles**
1. Use 20 keys with fixed-size values.
2. Force partition to rotate and trigger GC >=100 times.
3. Verify no RDB_ERR_FULL before GC target is met.

**KVDB-SEQ-06: write_seq wrap-around**
1. Initialize flash with records near 0xFFFFFFFF seq.
2. Re-init and confirm max_seq detection.

**KVDB-ITER-07: Iterator correctness under GC**
1. Start iterator.
2. Trigger GC or rotate, then call iter_next.
3. Expect RDB_ERR_BUSY and no stale outputs.

**KVDB-CORRUPT-08: Corrupt record header**
1. Inject invalid magic or key_len.
2. Ensure scan skips by corrupt_skip and continues safely.

**KVDB-PWL-09: Power-loss injection**
1. Interrupt after header write, after data write, before commit.
2. Re-init, ensure WRITING recovery logic correct.

### 4.4 TSDB Tests

**TSDB-APPEND-01: Basic append/query**
1. Append 1000 records with monotonic timestamps.
2. Query full range, ensure all records returned.

**TSDB-EPOCH-02: Reset epoch query correctness**
1. Append with time increasing to T.
2. Reset epoch, append with smaller timestamps.
3. Query range spanning both epochs, verify no truncation.

**TSDB-APPEND-03: Write failure handling**
1. Inject write failure during header, data, and commit steps.
2. Verify head_off advancement or DEAD marking to prevent AND-collision.

**TSDB-ROTATE-04: Full ring rotation >=100 cycles**
1. Fill head sectors to force rotation >=100 times.
2. Verify tail advancement, erase counts, and total_count consistency.

**TSDB-COUNT-05: Recount jitter control**
1. Measure append latency with recount enabled at ring cycles.
2. Ensure jitter stays within configured bounds or flagged.

**TSDB-CORRUPT-06: CRC mismatch**
1. Corrupt data bytes post-write.
2. Ensure query reports NULL data and increments crc_errors.

**TSDB-DEGRADED-07: ACTIVE in ring body**
1. Simulate power loss during seal.
2. Ensure ts_active_info recovers time_base and scan boundary.

### 4.5 Cross-Cutting Tests

**X-STRUCT-01: RAM footprint validation**
1. Assert rdb_kvdb_t, rdb_tsdb_t, and meta buffers sizes.
2. Confirm no dynamic allocation.

**X-LIMIT-02: Maximum size boundaries**
1. KVDB: key_len=RDB_MAX_KEY_LEN, val_len=RDB_MAX_VAL_LEN.
2. TSDB: data_len=max_data_len, including RDB_MAX_TS_DATA_LEN override.

**X-CAP-03: Capacity accounting**
1. Cross-check max_live and reported space_info against actual usage.

**X-RECOVER-04: Corrupt sector recovery**
1. Inject corrupt sector headers.
2. Verify init marks CORRUPT and reclaims when possible.

## 5. Pass/Fail Criteria
1. No data loss beyond defined GC and overwrite semantics.
2. No RDB_ERR_FULL before the required GC count threshold is reached.
3. No CRC mismatch unless intentionally injected.
4. Correct error return codes for all fault injections.
5. Stable operation across all write_gran values.

## 6. Reporting Output
Each test run records:
1. Seed and configuration.
2. GC count per partition.
3. Erase count distribution (min/max/avg).
4. Number of flash errors and CRC errors.
5. Maximum append and set latency for jitter evaluation.

## 7. Next Steps
1. Implement flash simulator and fault-injection harness.
2. Port tests into a repeatable runner (C test harness or host-side).
