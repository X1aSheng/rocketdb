# RocketDB Simulator (C)

This directory contains a host-side C simulator for the v0.0.2 RocketDB implementation. It models W25Q128 NOR behavior (1->0 writes, sector erase) and generates reusable test vectors for embedded replay.

## Files
1. `sim_flash.h/.c` – Flash model with write-granularity enforcement and fault injection.
2. `sim_vectors.h/.c` – Vector generator producing binary test streams.
3. `sim_runner.c` – Minimal runner to exercise KVDB/TSDB and emit vectors.

## Build (example)
Compile with your host compiler and link RocketDB sources:

```bash
# From g:\c-module\rocketdb
make test   # Uses Makefile (or build.bat for Windows)
```

## Output
The runner writes vectors to:
- `test\out\kv_vectors.bin`
- `test\out\ts_vectors.bin`

The `test\out\` directory is created automatically by the build system and excluded via `.gitignore`.

## Notes
1. Write granularity is set to 1 byte in `sim_runner.c` (WRITE_GRAN=0).
2. The runner exercises:
   - KVDB basic set/get
   - KVDB GC stress (>=100 runs)
   - TSDB append/query
3. Extend `sim_runner.c` to map additional test cases from `RocketDB_test_plan.md`.
