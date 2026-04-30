# RocketDB Performance Benchmark Suite

## Overview

This directory contains comprehensive performance benchmarking scenarios for RocketDB v0.0.2. The benchmark suite measures key operations across both KVDB (Key-Value Database) and TSDB (Time-Series Database) modules.

## Scenarios

### P-100: Basic Set/Get
- **Purpose**: Measure fundamental KVDB operations
- **Workload**: 1000 set operations + 1000 get operations
- **Metrics**: Operation latency (microseconds), throughput (ops/sec)
- **Target**: set < 10ms, get < 5ms

### P-101: GC Stress
- **Purpose**: Measure garbage collection performance under rapid updates
- **Workload**: 200 GC cycles with 10 set operations per cycle
- **Metrics**: GC cycle latency, average time per cycle
- **Target**: GC cycle < 50ms

### P-102: Large Value
- **Purpose**: Measure throughput with large values (2KB each)
- **Workload**: 100 set operations with 2KB values
- **Metrics**: Set latency for large values
- **Target**: 2KB set < 20ms

### P-103: Random Access
- **Purpose**: Measure cache/performance characteristics with random access patterns
- **Workload**: 100 keys populated, 1000 random get operations
- **Metrics**: Random access latency, hit rate impact
- **Target**: random get < 10ms

### P-104: TSDB Append
- **Purpose**: Measure time-series data insertion performance
- **Workload**: 1000 append operations with sequential timestamps
- **Metrics**: Append latency per record
- **Target**: append < 10ms

### P-105: TSDB Query
- **Purpose**: Measure time-series query performance
- **Workload**: 1000 append operations, then 100 range queries
- **Metrics**: Query latency, records traversed per query
- **Target**: query < 5ms

### P-106: Mixed Workload
- **Purpose**: Measure performance under realistic mixed operations
- **Workload**: 500 operations (50% KV set, 30% TSDB append, 20% TSDB query)
- **Metrics**: Operation latency distribution, throughput
- **Target**: Average < 15ms

## Building the Benchmark

### Windows (MinGW/GCC)

```bash
# Navigate to this directory
cd test\perf

# Build and run
run_benchmark.bat
```

### Linux/macOS

```bash
# Navigate to this directory
cd test/perf

# Build
make clean
make

# Run
make run
```

Or manually:

```bash
gcc -O2 -Wall -Wextra -std=c99 -I../../ -I../sim \
    -o benchmark scenarios.c ../sim/sim_flash.c

./benchmark
```

## Output Format

The benchmark produces output in two formats:

### Console Output
```
[P-100] Running basic set/get scenario...
P-100 set (1000 samples)
  Min: 0.123 us
  Max: 5.432 us
  Avg: 0.856 us
  P95: 2.145 us
  Throughput: 1,167,883 ops/sec
  CSV: results_20260225_140531.csv
```

### CSV Output
Each scenario generates CSV entries with:
- Scenario ID (P-100, P-101, etc.)
- Operation type (set, get, append, etc.)
- Sample count
- Min/Max/Avg/P95 metrics
- Throughput data

File naming: `results_YYYYMMDD_HHMMSS.csv`

## Analyzing Results

### Quick Analysis

```bash
# View latest results
cat results_*.csv | head -20

# Sort by operation type
sort -t, -k2 results_*.csv
```

### Detailed Performance Report

After running the benchmark, check:

1. **Operation Latencies**: Compare against targets (P-100: set<10ms, get<5ms)
2. **GC Performance**: P-101 should show GC cycle < 50ms
3. **Large Value Handling**: P-102 should handle 2KB efficiently
4. **Random Access**: P-103 patterns indicate cache effectiveness
5. **TSDB Operations**: P-104/P-105 validate time-series performance
6. **Mixed Workload**: P-106 simulates real-world usage patterns

### Regression Detection

Compare across runs:

```bash
# Baseline (v0.0.2 initial)
results_baseline.csv

# After optimization
results_optimized.csv

# Calculate improvement
awk -F, 'NR>1 {print $2, $4 / prev[$2]; prev[$2]=$4}' \
    results_baseline.csv results_optimized.csv
```

## Performance Targets for v1.0.0

| Operation | Current (v0.0.2) | v1.0.0 Target | Notes |
|-----------|------------------|---------------|-------|
| KVDB set (small) | TBD | < 10ms | -50% from baseline |
| KVDB get (small) | TBD | < 5ms | Cache-friendly |
| KVDB GC cycle | TBD | < 50ms | Rare, background |
| TSDB append | TBD | < 10ms | Stream optimized |
| TSDB query (range) | TBD | < 5ms | Index accelerated |

## Optimization Strategy

### Phase 6 Optimizations (Priority Order)

1. **GC Pre-erase** (Estimated impact: -10ms)
   - Pre-allocate erase buffer
   - Reduce on-demand erase delay
   
2. **Hardware CRC** (Estimated impact: -40% for large values)
   - Utilize CPU CRC32 instruction
   - Fallback to software CRC
   
3. **Batch Operations** (Estimated impact: -15%)
   - Combine related operations
   - Reduce function call overhead
   
4. **Memory Pool** (Estimated impact: -20%)
   - Pre-allocate buffers
   - Reduce malloc/free overhead

5. **Parallel Queries** (TSDB, Estimated impact: -30%)
   - Multi-threaded range queries
   - Partition index traversal

## Troubleshooting

### Build Issues

**"gcc: command not found"**
- Install MinGW/Cygwin on Windows
- On Linux: `apt-get install build-essential`
- On macOS: `xcode-select --install`

**"Undefined reference to sim_flash_*"**
- Ensure `../sim/sim_flash.c` exists
- Check include paths with `-I../sim`

### Runtime Issues

**All operations timeout/hang**
- Check sim_flash.c initialization
- Verify flash simulation parameters
- Review error logs for assertion failures

**Metrics look unrealistic**
- Check timer resolution (`perf_benchmark.h`)
- Windows: Ensure QueryPerformanceCounter available
- Linux: Verify clock_gettime MONOTONIC support

**Memory exhaustion**
- Reduce scenario sample counts
- Check for memory leaks in sim_flash
- Verify scenario cleanup (rdb_kvdb_format/rdb_tsdb_format)

## Future Enhancements

- [ ] Multi-threaded stress testing (P-200 series)
- [ ] Endurance testing (24-hour scenarios)
- [ ] Power consumption profiling
- [ ] Memory usage tracking
- [ ] Competitive benchmarking suite
- [ ] Automated regression detection
- [ ] Web dashboard for results visualization

## References

- Main: ../../rocketdb.h
- Tests: ../test_*.c
- Simulator: ../sim/sim_flash.c
- Framework: perf_benchmark.h
