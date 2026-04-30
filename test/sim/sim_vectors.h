#ifndef RDB_SIM_VECTORS_H
#define RDB_SIM_VECTORS_H

#include <stdint.h>
#include <stddef.h>

#include "sim_dist.h"

#ifdef __cplusplus
extern "C" {
#endif

#define RDB_VEC_MAGIC 0x54424452u /* "RDBT" */

typedef enum {
    RDB_VEC_KIND_KV = 1,
    RDB_VEC_KIND_TS = 2
} rdb_vec_kind_t;

typedef enum {
    RDB_VEC_KV_SET = 1,
    RDB_VEC_KV_DEL = 2,
    RDB_VEC_KV_GET = 3
} rdb_vec_kv_op_t;

typedef struct {
    uint32_t magic;
    uint16_t version;
    uint16_t kind;
    uint32_t count;
} rdb_vec_hdr_t;

int rdb_vec_generate_kv(const char *path, uint32_t seed, uint32_t count,
                        uint8_t max_k, uint16_t max_v);
int rdb_vec_generate_ts(const char *path, uint32_t seed, uint32_t count,
                        uint16_t max_dlen);

int rdb_vec_generate_kv_dist(const char *path, uint32_t seed, uint32_t count,
                             uint8_t max_k, uint16_t max_v,
                             sim_dist_t *kdist, sim_dist_t *vdist);
int rdb_vec_generate_ts_dist(const char *path, uint32_t seed, uint32_t count,
                             uint16_t max_dlen, sim_dist_t *ddist);

#ifdef __cplusplus
}
#endif

#endif
