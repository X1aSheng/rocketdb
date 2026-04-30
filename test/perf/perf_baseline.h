/**
 * @file perf_baseline.h
 * @brief Performance baseline targets and configuration for RocketDB v1.0.0
 * 
 * This header defines the baseline performance targets that all optimizations
 * must meet or exceed. Used to validate performance improvements.
 */

#ifndef PERF_BASELINE_H
#define PERF_BASELINE_H

#include <stdint.h>

/* ========== Performance Targets (Microseconds) ========== */

/* KVDB Operations */
#define PERF_TARGET_KVDB_SET_SMALL_US    10000  /* < 10ms for small values */
#define PERF_TARGET_KVDB_GET_SMALL_US     5000  /* < 5ms for small values */
#define PERF_TARGET_KVDB_SET_LARGE_US    20000  /* < 20ms for 2KB values */
#define PERF_TARGET_KVDB_GC_CYCLE_US     50000  /* < 50ms per GC cycle */

/* TSDB Operations */
#define PERF_TARGET_TSDB_APPEND_US       10000  /* < 10ms per append */
#define PERF_TARGET_TSDB_QUERY_US         5000  /* < 5ms per query */
#define PERF_TARGET_TSDB_ROTATION_US     20000  /* < 20ms for rotation */

/* Mixed Workload */
#define PERF_TARGET_MIXED_AVG_US         15000  /* < 15ms average operation */

/* ========== Throughput Targets (Operations/Second) ========== */

#define PERF_TARGET_KVDB_SET_THRP       100000  /* >= 100k sets/sec */
#define PERF_TARGET_KVDB_GET_THRP       200000  /* >= 200k gets/sec */
#define PERF_TARGET_TSDB_APPEND_THRP    100000  /* >= 100k appends/sec */

/* ========== Acceptable Variance ========== */

/* Variance allowed from targets (percentage) */
#define PERF_VARIANCE_INITIAL_PERCENT    15   /* Initial measurements allow 15% variance */
#define PERF_VARIANCE_OPTIMIZED_PERCENT   5   /* After optimization: 5% variance */

/* ========== Percentile Targets ========== */

/* For P95 percentile (95th percentile latency) */
#define PERF_TARGET_P95_SET_SMALL_US     15000  /* P95 < 15ms for set */
#define PERF_TARGET_P95_GET_SMALL_US      8000  /* P95 < 8ms for get */
#define PERF_TARGET_P95_QUERY_US          7000  /* P95 < 7ms for query */

/* ========== Optimization Goals ========== */

/* Target improvements for v1.0.0 */
#define PERF_OPTIMIZATION_TARGET_PERCENT 30   /* Achieve 30% improvement */

struct perf_target_t {
    const char *operation;
    uint32_t target_us;
    uint32_t target_p95_us;
    uint32_t target_throughput;
};

/* Baseline configuration and validation utilities */

/**
 * Check if operation latency meets target
 * @param operation_name Name of operation (e.g., "P-100 set")
 * @param latency_us Measured latency in microseconds
 * @param target_us Target latency in microseconds
 * @return 1 if meets target, 0 if exceeds
 */
static inline int perf_check_target(
    const char *operation_name, 
    uint32_t latency_us, 
    uint32_t target_us)
{
    return latency_us <= target_us;
}

/**
 * Calculate acceptable variance range
 * @param target_us Baseline target in microseconds
 * @param variance_percent Maximum acceptable variance (percentage)
 * @param min_us Output: minimum acceptable value
 * @param max_us Output: maximum acceptable value
 */
static inline void perf_variance_range(
    uint32_t target_us,
    uint32_t variance_percent,
    uint32_t *min_us,
    uint32_t *max_us)
{
    uint32_t delta = (target_us * variance_percent) / 100;
    *min_us = target_us - delta;
    *max_us = target_us + delta;
}

/**
 * Calculate optimization improvement
 * @param baseline_us Original baseline latency
 * @param optimized_us Optimized latency
 * @return Improvement percentage (positive = improvement)
 */
static inline int32_t perf_improvement_percent(
    uint32_t baseline_us,
    uint32_t optimized_us)
{
    if (baseline_us == 0) return 0;
    return ((int32_t)baseline_us - (int32_t)optimized_us) * 100 / baseline_us;
}

/* ========== Performance Targets Summary ========== */

/*
 * P-100: Basic Set/Get
 *   - Target: set < 10ms, get < 5ms
 *   - P95: set < 15ms, get < 8ms
 *   - Throughput: set >= 100k/sec, get >= 200k/sec
 *
 * P-101: GC Stress
 *   - Target: GC cycle < 50ms
 *   - Rationale: GC is rare, can tolerate higher latency
 *
 * P-102: Large Value (2KB)
 *   - Target: set < 20ms
 *   - Rationale: Large values require more I/O
 *
 * P-103: Random Access
 *   - Target: get < 10ms (may be slower due to cache misses)
 *   - P95: < 15ms
 *
 * P-104: TSDB Append
 *   - Target: append < 10ms
 *   - Rationale: Stream performance critical for time-series
 *
 * P-105: TSDB Query
 *   - Target: query < 5ms
 *   - P95: query < 7ms
 *
 * P-106: Mixed Workload
 *   - Target: average < 15ms
 *   - Workload: 50% KV, 30% append, 20% query
 */

#endif /* PERF_BASELINE_H */
