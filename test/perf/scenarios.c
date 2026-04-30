/**
 * @file scenarios.c
 * @brief RocketDB Performance Test Scenarios
 * 
 * Implements 7 performance benchmark scenarios:
 * P-100: Basic set/get
 * P-101: GC stress  
 * P-102: Large value
 * P-103: Random access
 * P-104: TSDB append
 * P-105: TSDB query
 * P-106: Mixed workload
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "perf_benchmark.h"
#include "../../src/rocketdb.h"
#include "../../test/sim/sim_flash.h"

/* ========== Global state for queries ========== */
static int query_count = 0;

/**
 * TSDB query callback (defined globally, not nested)
 */
static int tsdb_query_callback(uint32_t ts_val, const void *data, uint16_t len, void *arg) {
    query_count++;
    return 0;  /* Continue iteration */
}

/* ========== Scenario P-100: Basic set/get ========== */

void scenario_p100_basic_set_get(void) {
    printf("\n[P-100] Running basic set/get scenario...\n");
    
    rdb_kvdb_t db;
    rdb_kvdb_format(&db, NULL);
    
    perf_stats_t *stats_set = perf_stats_create("P-100", "set", 1000);
    perf_stats_t *stats_get = perf_stats_create("P-100", "get", 1000);
    
    perf_timer_t timer;
    
    // Perform 1000 set operations
    for (int i = 0; i < 1000; i++) {
        char key[16], val[32];
        snprintf(key, sizeof(key), "key_%05d", i);
        snprintf(val, sizeof(val), "value_%05d_%s", i, "test");
        
        perf_start(&timer);
        rdb_kvdb_set(&db, key, val, strlen(val));
        perf_stop(&timer);
        
        perf_stats_add_sample(stats_set, perf_elapsed_us(&timer));
    }
    
    // Perform 1000 get operations
    for (int i = 0; i < 1000; i++) {
        char key[16], buf[32];
        uint16_t len;
        snprintf(key, sizeof(key), "key_%05d", i);
        
        perf_start(&timer);
        rdb_kvdb_get(&db, key, buf, sizeof(buf), &len);
        perf_stop(&timer);
        
        perf_stats_add_sample(stats_get, perf_elapsed_us(&timer));
    }
    
    perf_stats_calculate(stats_set);
    perf_stats_calculate(stats_get);
    
    perf_stats_print(stats_set);
    perf_stats_print(stats_get);
    
    perf_stats_free(stats_set);
    perf_stats_free(stats_get);
    
    rdb_kvdb_format(&db);
}

/* ========== Scenario P-101: GC Stress ========== */

void scenario_p101_gc_stress(void) {
    printf("\n[P-101] Running GC stress scenario...\n");
    
    rdb_kv_t db;
    rdb_kvdb_init(&db, &sim_flash_ops);
    
    perf_stats_t *stats_gc = perf_stats_create("P-101", "gc_cycle", 200);
    perf_timer_t timer;
    
    // Trigger 200 GC cycles by setting/deleting keys repeatedly
    for (int gc_cycle = 0; gc_cycle < 200; gc_cycle++) {
        perf_start(&timer);
        
        // Set 10 keys (may trigger GC)
        for (int i = 0; i < 10; i++) {
            char key[16], val[64];
            snprintf(key, sizeof(key), "gc_key_%d_%d", gc_cycle, i);
            snprintf(val, sizeof(val), "gc_value_%d_%d_with_some_padding_for_size", gc_cycle, i);
            rdb_kvdb_set(&db, key, val, strlen(val));
        }
        
        perf_stop(&timer);
        perf_stats_add_sample(stats_gc, perf_elapsed_us(&timer));
    }
    
    perf_stats_calculate(stats_gc);
    perf_stats_print(stats_gc);
    
    perf_stats_free(stats_gc);
    rdb_kvdb_format(&db);
}

/* ========== Scenario P-102: Large value ========== */

void scenario_p102_large_value(void) {
    printf("\n[P-102] Running large value scenario...\n");
    
    rdb_kv_t db;
    rdb_kvdb_init(&db, &sim_flash_ops);
    
    perf_stats_t *stats_set = perf_stats_create("P-102", "set_2KB", 100);
    perf_timer_t timer;
    
    // Allocate 2KB buffer for large value
    uint8_t large_val[2048];
    memset(large_val, 'X', sizeof(large_val));
    
    // Set 100 large values
    for (int i = 0; i < 100; i++) {
        char key[16];
        snprintf(key, sizeof(key), "large_%04d", i);
        
        perf_start(&timer);
        rdb_kvdb_set(&db, key, large_val, sizeof(large_val));
        perf_stop(&timer);
        
        perf_stats_add_sample(stats_set, perf_elapsed_us(&timer));
    }
    
    perf_stats_calculate(stats_set);
    perf_stats_print(stats_set);
    
    perf_stats_free(stats_set);
    rdb_kvdb_format(&db);
}

/* ========== Scenario P-103: Random access ========== */

void scenario_p103_random_access(void) {
    printf("\n[P-103] Running random access scenario...\n");
    
    rdb_kv_t db;
    rdb_kvdb_init(&db, &sim_flash_ops);
    
    perf_stats_t *stats_get = perf_stats_create("P-103", "random_get", 1000);
    perf_timer_t timer;
    
    // First, populate 100 keys
    for (int i = 0; i < 100; i++) {
        char key[16], val[32];
        snprintf(key, sizeof(key), "rand_%03d", i);
        snprintf(val, sizeof(val), "value_%03d", i);
        rdb_kvdb_set(&db, key, val, strlen(val));
    }
    
    // Random access to 200 keys (may miss some)
    srand(time(NULL));
    for (int i = 0; i < 1000; i++) {
        char key[16], buf[32];
        uint16_t len;
        int idx = rand() % 100;
        snprintf(key, sizeof(key), "rand_%03d", idx);
        
        perf_start(&timer);
        rdb_kvdb_get(&db, key, buf, sizeof(buf), &len);
        perf_stop(&timer);
        
        perf_stats_add_sample(stats_get, perf_elapsed_us(&timer));
    }
    
    perf_stats_calculate(stats_get);
    perf_stats_print(stats_get);
    
    perf_stats_free(stats_get);
    rdb_kvdb_format(&db);
}

/* ========== Scenario P-104: TSDB append ========== */

void scenario_p104_tsdb_append(void) {
    printf("\n[P-104] Running TSDB append scenario...\n");
    
    rdb_tsdb_t db;
    rdb_tsdb_init(&db, &sim_flash_ops);
    
    perf_stats_t *stats_append = perf_stats_create("P-104", "append", 1000);
    perf_timer_t timer;
    
    uint32_t ts = 1000000;  // Start timestamp
    float value = 25.5;     // Temperature value
    
    // Append 1000 records
    for (int i = 0; i < 1000; i++) {
        perf_start(&timer);
        rdb_tsdb_append(&db, ts + i, &value, sizeof(value));
        perf_stop(&timer);
        
        perf_stats_add_sample(stats_append, perf_elapsed_us(&timer));
    }
    
    perf_stats_calculate(stats_append);
    perf_stats_print(stats_append);
    
    perf_stats_free(stats_append);
    rdb_tsdb_format(&db);
}

/* ========== Scenario P-105: TSDB query ========== */

void scenario_p105_tsdb_query(void) {
    printf("\n[P-105] Running TSDB query scenario...\n");
    
    rdb_tsdb_t db;
    rdb_tsdb_init(&db, &sim_flash_ops);
    
    perf_stats_t *stats_query = perf_stats_create("P-105", "query", 100);
    perf_timer_t timer;
    
    // Append 1000 records first
    uint32_t ts = 1000000;
    float value = 25.5;
    for (int i = 0; i < 1000; i++) {
        rdb_tsdb_append(&db, ts + i, &value, sizeof(value));
    }
    
    // Query counter (simple)
    static int query_count;
    int query_callback(uint32_t ts_val, const void *data, uint16_t len, void *arg) {
        query_count++;
        return 0;
    }
    
    // Query 100 times (different ranges)
    for (int i = 0; i < 100; i++) {
        uint32_t start = ts + (i * 5);
        uint32_t end = start + 100;
        query_count = 0;
        
        perf_start(&timer);
        rdb_tsdb_query(&db, start, end, query_callback, NULL);
        perf_stop(&timer);
        
        perf_stats_add_sample(stats_query, perf_elapsed_us(&timer));
    }
    
    perf_stats_calculate(stats_query);
    perf_stats_print(stats_query);
    
    perf_stats_free(stats_query);
    rdb_tsdb_format(&db);
}

/* ========== Scenario P-106: Mixed workload ========== */

void scenario_p106_mixed_workload(void) {
    printf("\n[P-106] Running mixed workload scenario...\n");
    
    rdb_kv_t kvdb;
    rdb_tsdb_t tsdb;
    rdb_kvdb_init(&kvdb, &sim_flash_ops);
    rdb_tsdb_init(&tsdb, &sim_flash_ops);
    
    perf_stats_t *stats_mixed = perf_stats_create("P-106", "mixed_op", 500);
    perf_timer_t timer;
    
    // Mixed operations: 50% KV, 30% TSDB append, 20% TSDB query
    for (int i = 0; i < 500; i++) {
        int op_type = rand() % 100;
        
        perf_start(&timer);
        
        if (op_type < 50) {
            // KV operation
            char key[16], val[32];
            snprintf(key, sizeof(key), "mixed_%04d", i);
            snprintf(val, sizeof(val), "val_%04d", i);
            rdb_kvdb_set(&kvdb, key, val, strlen(val));
        } else if (op_type < 80) {
            // TSDB append
            uint32_t ts = 2000000 + i;
            float temp = 20.0 + (i % 10);
            rdb_tsdb_append(&tsdb, ts, &temp, sizeof(temp));
        } else {
            // TSDB query (simple range)
            uint32_t start = 2000000 + (i / 5);
            uint32_t end = start + 50;
            int qc = 0;
            int qcb(uint32_t ts, const void *d, uint16_t l, void *a) { qc++; return 0; }
            rdb_tsdb_query(&tsdb, start, end, qcb, NULL);
        }
        
        perf_stop(&timer);
        perf_stats_add_sample(stats_mixed, perf_elapsed_us(&timer));
    }
    
    perf_stats_calculate(stats_mixed);
    perf_stats_print(stats_mixed);
    
    perf_stats_free(stats_mixed);
    rdb_kvdb_format(&kvdb);
    rdb_tsdb_format(&tsdb);
}

/* ========== Main entry point ========== */

int main(int argc, char *argv[]) {
    printf("========================================\n");
    printf(" RocketDB v0.0.2 Performance Benchmark\n");
    printf("========================================\n");
    
    perf_report_print_header();
    
    // Run all scenarios
    scenario_p100_basic_set_get();
    scenario_p101_gc_stress();
    scenario_p102_large_value();
    scenario_p103_random_access();
    scenario_p104_tsdb_append();
    scenario_p105_tsdb_query();
    scenario_p106_mixed_workload();
    
    printf("\n");
    printf("========================================\n");
    printf(" Benchmark Complete\n");
    printf("========================================\n");
    
    return 0;
}
