#ifndef RDB_SIM_DIST_H
#define RDB_SIM_DIST_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum { SIM_DIST_UNIFORM = 0, SIM_DIST_GAUSSIAN = 1, SIM_DIST_POWERLAW = 2 } sim_dist_mode_t;

typedef struct {
    sim_dist_mode_t mode;
    uint32_t        seed;
    uint32_t        state;
    uint32_t        min;
    uint32_t        max;
    double          mean;
    double          stddev;
    double          alpha;
} sim_dist_t;

void sim_dist_init_uniform(sim_dist_t* dist, uint32_t seed, uint32_t min, uint32_t max);
void sim_dist_init_gaussian(sim_dist_t* dist, uint32_t seed, uint32_t min, uint32_t max, double mean, double stddev);
void sim_dist_init_powerlaw(sim_dist_t* dist, uint32_t seed, uint32_t min, uint32_t max, double alpha);

void     sim_dist_reset(sim_dist_t* dist);
uint32_t sim_dist_next(sim_dist_t* dist);

#ifdef __cplusplus
}
#endif

#endif
