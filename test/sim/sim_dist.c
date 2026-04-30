#include "sim_dist.h"
#include <math.h>

#define SIM_DIST_DEFAULT_SEED 0x12345678u

static uint32_t lcg_next(uint32_t *s)
{
    *s = (*s * 1664525u) + 1013904223u;
    return *s;
}

static double uniform01(uint32_t *s)
{
    const double denom = 4294967296.0; /* 2^32 */
    return (lcg_next(s) + 1.0) / (denom + 1.0);
}

static uint32_t clamp_u32(uint32_t v, uint32_t lo, uint32_t hi)
{
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}

void sim_dist_reset(sim_dist_t *dist)
{
    if (!dist) return;
    dist->state = dist->seed ? dist->seed : SIM_DIST_DEFAULT_SEED;
}

void sim_dist_init_uniform(sim_dist_t *dist, uint32_t seed,
                           uint32_t min, uint32_t max)
{
    if (!dist) return;
    dist->mode = SIM_DIST_UNIFORM;
    dist->seed = seed;
    dist->min = min;
    dist->max = max;
    dist->mean = 0.0;
    dist->stddev = 1.0;
    dist->alpha = 2.0;
    sim_dist_reset(dist);
}

void sim_dist_init_gaussian(sim_dist_t *dist, uint32_t seed,
                            uint32_t min, uint32_t max,
                            double mean, double stddev)
{
    if (!dist) return;
    dist->mode = SIM_DIST_GAUSSIAN;
    dist->seed = seed;
    dist->min = min;
    dist->max = max;
    dist->mean = mean;
    dist->stddev = (stddev > 0.0) ? stddev : 1.0;
    dist->alpha = 2.0;
    sim_dist_reset(dist);
}

void sim_dist_init_powerlaw(sim_dist_t *dist, uint32_t seed,
                            uint32_t min, uint32_t max,
                            double alpha)
{
    if (!dist) return;
    dist->mode = SIM_DIST_POWERLAW;
    dist->seed = seed;
    dist->min = min;
    dist->max = max;
    dist->mean = 0.0;
    dist->stddev = 1.0;
    dist->alpha = (alpha > 1.0) ? alpha : 1.1;
    sim_dist_reset(dist);
}

uint32_t sim_dist_next(sim_dist_t *dist)
{
    if (!dist) return 0;

    if (dist->max <= dist->min) {
        return dist->min;
    }

    switch (dist->mode) {
        case SIM_DIST_UNIFORM: {
            uint32_t span = dist->max - dist->min + 1u;
            uint32_t v = lcg_next(&dist->state);
            return dist->min + (v % span);
        }
        case SIM_DIST_GAUSSIAN: {
            const double two_pi = 6.283185307179586;
            double u1 = uniform01(&dist->state);
            double u2 = uniform01(&dist->state);
            double z0 = sqrt(-2.0 * log(u1)) * cos(two_pi * u2);
            double val = dist->mean + z0 * dist->stddev;
            if (val < 0.0) val = 0.0;
            return clamp_u32((uint32_t)llround(val), dist->min, dist->max);
        }
        case SIM_DIST_POWERLAW: {
            double minv = (double)dist->min;
            double maxv = (double)dist->max;
            if (minv < 1.0) minv = 1.0;
            if (maxv < minv) maxv = minv;
            double a = dist->alpha;
            double exp = 1.0 - a;
            double min_pow = pow(minv, exp);
            double max_pow = pow(maxv, exp);
            double u = uniform01(&dist->state);
            double val = pow(min_pow + (max_pow - min_pow) * u, 1.0 / exp);
            return clamp_u32((uint32_t)llround(val), dist->min, dist->max);
        }
        default:
            return dist->min;
    }
}

