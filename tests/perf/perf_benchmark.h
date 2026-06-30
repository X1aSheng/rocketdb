#ifndef PERF_BENCHMARK_H
#define PERF_BENCHMARK_H

#ifdef _WIN32
#include <windows.h>
#endif

#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/**
 * @file perf_benchmark.h
 * @brief RocketDB Performance Benchmark Framework
 *
 * Provides utilities for:
 * - High-precision timing (microsecond resolution)
 * - Statistical analysis (avg/min/max/p95)
 * - CSV export for analysis
 * - Multi-scenario benchmarking
 */

/* ========== Timing Utilities ========== */

typedef struct {
    uint64_t start_ns;
    uint64_t end_ns;
    uint64_t elapsed_ns;
} perf_timer_t;

/**
 * Get current time in nanoseconds
 * Note: Actual precision depends on system
 * - Windows: QueryPerformanceCounter (~100ns)
 * - Linux: clock_gettime MONOTONIC (~1ns)
 */
static inline uint64_t perf_get_ns(void) {
#ifdef _WIN32
    // Windows: Use QueryPerformanceCounter
    LARGE_INTEGER freq, count;
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&count);
    return (count.QuadPart * 1000000000) / freq.QuadPart;
#else
    // Linux/Mac: Use clock_gettime
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    uint64_t sec = (uint64_t)(unsigned long)ts.tv_sec;
    return sec * 1000000000u + (uint64_t)ts.tv_nsec;
#endif
}

/**
 * Start timing
 */
static inline void perf_start(perf_timer_t* timer) {
    memset(timer, 0, sizeof(*timer));
    timer->start_ns = perf_get_ns();
}

/**
 * Stop timing and calculate elapsed
 */
static inline void perf_stop(perf_timer_t* timer) {
    timer->end_ns     = perf_get_ns();
    timer->elapsed_ns = timer->end_ns - timer->start_ns;
}

/**
 * Get elapsed time in various units
 */
static inline uint64_t perf_elapsed_ns(perf_timer_t* timer) {
    return timer->elapsed_ns;
}

static inline double perf_elapsed_us(perf_timer_t* timer) {
    return (double)timer->elapsed_ns / 1000.0;
}

static inline double perf_elapsed_ms(perf_timer_t* timer) {
    return (double)timer->elapsed_ns / 1000000.0;
}

/* ========== Statistics Collection ========== */

typedef struct {
    char     scenario_id[16];
    char     operation[32];
    uint32_t count;

    uint64_t* samples;  // Individual samples in us
    uint32_t  capacity;
    uint32_t  sample_count;

    // Computed statistics
    double   avg_us;
    uint64_t min_us;
    uint64_t max_us;
    uint64_t p95_us;
    double   throughput;  // ops/sec
} perf_stats_t;

/**
 * Initialize statistics collector
 */
static inline perf_stats_t* perf_stats_create(const char* scenario_id, const char* operation, uint32_t max_samples) {
    perf_stats_t* stats = (perf_stats_t*)malloc(sizeof(*stats));
    if (!stats)
        return NULL;

    stats->samples = (uint64_t*)malloc(sizeof(uint64_t) * max_samples);
    if (!stats->samples) {
        free(stats);
        return NULL;
    }

    strncpy(stats->scenario_id, scenario_id, sizeof(stats->scenario_id) - 1);
    stats->scenario_id[sizeof(stats->scenario_id) - 1] = '\0';
    strncpy(stats->operation, operation, sizeof(stats->operation) - 1);
    stats->operation[sizeof(stats->operation) - 1] = '\0';
    stats->count                                   = 0;
    stats->capacity                                = max_samples;
    stats->sample_count                            = 0;

    return stats;
}

/**
 * Add timing sample (in microseconds)
 */
static inline void perf_stats_add_sample(perf_stats_t* stats, uint64_t sample_us) {
    if (stats->sample_count < stats->capacity) {
        stats->samples[stats->sample_count++] = sample_us;
        stats->count++;
    }
}

/**
 * Calculate statistics
 */
static inline void perf_stats_calculate(perf_stats_t* stats) {
    if (stats->sample_count == 0)
        return;

    // Min, Max, Sum
    uint64_t min = UINT64_MAX;
    uint64_t max = 0;
    uint64_t sum = 0;

    for (uint32_t i = 0; i < stats->sample_count; i++) {
        uint64_t sample = stats->samples[i];
        if (sample < min)
            min = sample;
        if (sample > max)
            max = sample;
        sum += sample;
    }

    stats->min_us = min;
    stats->max_us = max;
    stats->avg_us = (double)sum / stats->sample_count;

    // P95: 95th percentile
    // Simple implementation: sort and take 95th index
    // For production, use more efficient sorting
    uint32_t p95_idx = (stats->sample_count * 95) / 100;
    if (p95_idx >= stats->sample_count)
        p95_idx = stats->sample_count - 1;

    // Partial sort to find p95
    for (uint32_t i = 0; i <= p95_idx && i < stats->sample_count; i++) {
        for (uint32_t j = i + 1; j < stats->sample_count; j++) {
            if (stats->samples[j] < stats->samples[i]) {
                uint64_t tmp      = stats->samples[i];
                stats->samples[i] = stats->samples[j];
                stats->samples[j] = tmp;
            }
        }
    }
    stats->p95_us = stats->samples[p95_idx];

    // Throughput in ops/sec
    uint64_t total_ns = sum * 1000;  // Convert us to ns
    stats->throughput = (total_ns == 0) ? 0.0 : (double)(stats->count) * 1000000000.0 / (double)total_ns;
}

/**
 * Print statistics
 */
static inline void perf_stats_print(perf_stats_t* stats) {
    printf("%s,%s,%u,%.2f,%" PRIu64 ",%" PRIu64 ",%" PRIu64 ",%.0f\n", stats->scenario_id, stats->operation,
        stats->count, stats->avg_us, stats->min_us, stats->max_us, stats->p95_us, stats->throughput);
}

/**
 * Free statistics
 */
static inline void perf_stats_free(perf_stats_t* stats) {
    if (stats) {
        if (stats->samples)
            free(stats->samples);
        free(stats);
    }
}

/* ========== Report Generation ========== */

/**
 * Print CSV header
 */
static inline void perf_report_print_header(void) {
    printf("scenario_id,operation,count,avg_us,min_us,max_us,p95_us,throughput_ops_per_sec\n");
}

/**
 * Print results summary
 */
static inline void perf_report_summary(const char* title, perf_stats_t* stats_array, uint32_t count) {
    printf("\n");
    printf("========================================\n");
    printf(" %s\n", title);
    printf("========================================\n");
    printf("\n");

    perf_report_print_header();

    for (uint32_t i = 0; i < count; i++) {
        perf_stats_calculate(&stats_array[i]);
        perf_stats_print(&stats_array[i]);
    }
}

#endif  // PERF_BENCHMARK_H
