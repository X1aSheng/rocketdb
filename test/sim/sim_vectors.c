#define _CRT_SECURE_NO_WARNINGS 1
#include "sim_vectors.h"
#include <stdio.h>

static uint32_t lcg_next(uint32_t *s)
{
    *s = (*s * 1664525u) + 1013904223u;
    return *s;
}

static uint32_t rnd_range(uint32_t *s, uint32_t lo, uint32_t hi)
{
    uint32_t v = lcg_next(s);
    return lo + (v % (hi - lo + 1u));
}

static uint32_t pick_weighted(uint32_t *s, const uint32_t *w, uint32_t n)
{
    uint32_t sum = 0;
    for (uint32_t i = 0; i < n; i++) sum += w[i];
    uint32_t r = rnd_range(s, 1, sum);
    uint32_t acc = 0;
    for (uint32_t i = 0; i < n; i++) {
        acc += w[i];
        if (r <= acc) return i;
    }
    return n - 1;
}

static void gen_bytes(uint32_t *s, uint8_t *buf, uint32_t len)
{
    for (uint32_t i = 0; i < len; i++)
        buf[i] = (uint8_t)lcg_next(s);
}

static void gen_key(uint32_t *s, uint8_t *buf, uint8_t len)
{
    for (uint8_t i = 0; i < len; i++) {
        uint8_t v = (uint8_t)lcg_next(s);
        buf[i] = (uint8_t)('A' + (v % 26));
    }
}

static uint32_t clamp_u32(uint32_t v, uint32_t lo, uint32_t hi)
{
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}

static uint8_t default_klen(uint32_t *s, uint8_t max_k)
{
    const uint32_t w_k[4] = { 50, 30, 15, 5 };
    uint32_t k_sel = pick_weighted(s, w_k, 4);
    if (k_sel == 0) return (uint8_t)rnd_range(s, 1, 8);
    if (k_sel == 1) return (uint8_t)rnd_range(s, 9, 32);
    if (k_sel == 2) return (uint8_t)rnd_range(s, 33, (max_k > 33 ? max_k : 33));
    return max_k;
}

static uint16_t default_vlen(uint32_t *s, uint16_t max_v)
{
    const uint32_t w_v[5] = { 40, 40, 15, 4, 1 };
    uint32_t v_sel = pick_weighted(s, w_v, 5);
    if (v_sel == 0) return (uint16_t)rnd_range(s, 0, 32);
    if (v_sel == 1) return (uint16_t)rnd_range(s, 33, 256);
    if (v_sel == 2) return (uint16_t)rnd_range(s, 257, 1024);
    if (v_sel == 3) return (uint16_t)rnd_range(s, 1025, (max_v > 1025 ? max_v - 1 : max_v));
    return max_v;
}

static uint16_t default_ts_len(uint32_t *s, uint16_t max_dlen)
{
    const uint32_t w_d[5] = { 40, 40, 15, 4, 1 };
    uint32_t d_sel = pick_weighted(s, w_d, 5);
    if (d_sel == 0) return (uint16_t)rnd_range(s, 1, 32);
    if (d_sel == 1) return (uint16_t)rnd_range(s, 33, 256);
    if (d_sel == 2) return (uint16_t)rnd_range(s, 257, 1024);
    if (d_sel == 3) return (uint16_t)rnd_range(s, 1025, (max_dlen > 1025 ? max_dlen - 1 : max_dlen));
    return max_dlen;
}

int rdb_vec_generate_kv_dist(const char *path, uint32_t seed, uint32_t count,
                             uint8_t max_k, uint16_t max_v,
                             sim_dist_t *kdist, sim_dist_t *vdist)
{
    if (!path || count == 0) return -1;

    FILE *f = fopen(path, "wb");
    if (!f) return -1;

    rdb_vec_hdr_t hdr;
    hdr.magic = RDB_VEC_MAGIC;
    hdr.version = 1;
    hdr.kind = RDB_VEC_KIND_KV;
    hdr.count = count;
    fwrite(&hdr, sizeof(hdr), 1, f);

    uint32_t s = seed;

    for (uint32_t i = 0; i < count; i++) {
        uint8_t op;
        uint32_t op_r = rnd_range(&s, 1, 100);
        if (op_r <= 60) op = RDB_VEC_KV_SET;
        else if (op_r <= 85) op = RDB_VEC_KV_SET; /* update */
        else if (op_r <= 95) op = RDB_VEC_KV_GET;
        else op = RDB_VEC_KV_DEL;

        uint8_t klen;
        if (kdist) {
            uint32_t k_raw = sim_dist_next(kdist);
            klen = (uint8_t)clamp_u32(k_raw, 1, max_k);
        } else {
            klen = default_klen(&s, max_k);
        }

        uint16_t vlen;
        if (vdist) {
            uint32_t v_raw = sim_dist_next(vdist);
            vlen = (uint16_t)clamp_u32(v_raw, 0, max_v);
        } else {
            vlen = default_vlen(&s, max_v);
        }

        fwrite(&op, 1, 1, f);
        fwrite(&klen, 1, 1, f);
        fwrite(&vlen, 2, 1, f);

        uint8_t kbuf[256];
        uint8_t vbuf[65535];

        gen_key(&s, kbuf, klen);
        fwrite(kbuf, 1, klen, f);

        if (op == RDB_VEC_KV_SET && vlen > 0) {
            gen_bytes(&s, vbuf, vlen);
            fwrite(vbuf, 1, vlen, f);
        }
    }

    fclose(f);
    return 0;
}

int rdb_vec_generate_kv(const char *path, uint32_t seed, uint32_t count,
                        uint8_t max_k, uint16_t max_v)
{
    return rdb_vec_generate_kv_dist(path, seed, count, max_k, max_v, NULL, NULL);
}

int rdb_vec_generate_ts_dist(const char *path, uint32_t seed, uint32_t count,
                             uint16_t max_dlen, sim_dist_t *ddist)
{
    if (!path || count == 0) return -1;

    FILE *f = fopen(path, "wb");
    if (!f) return -1;

    rdb_vec_hdr_t hdr;
    hdr.magic = RDB_VEC_MAGIC;
    hdr.version = 1;
    hdr.kind = RDB_VEC_KIND_TS;
    hdr.count = count;
    fwrite(&hdr, sizeof(hdr), 1, f);

    uint32_t s = seed;
    uint32_t time = 1;

    for (uint32_t i = 0; i < count; i++) {
        uint32_t t_mode = rnd_range(&s, 1, 100);
        if (t_mode <= 70) time += rnd_range(&s, 1, 100);
        else if (t_mode <= 90) { /* keep time (non-increasing input) */ }
        else time = rnd_range(&s, 1, 100);  /* epoch-like reset */

        uint16_t dlen;
        if (ddist) {
            uint32_t d_raw = sim_dist_next(ddist);
            dlen = (uint16_t)clamp_u32(d_raw, 1, max_dlen);
        } else {
            dlen = default_ts_len(&s, max_dlen);
        }

        fwrite(&time, 4, 1, f);
        fwrite(&dlen, 2, 1, f);

        uint8_t dbuf[65535];
        gen_bytes(&s, dbuf, dlen);
        fwrite(dbuf, 1, dlen, f);
    }

    fclose(f);
    return 0;
}

int rdb_vec_generate_ts(const char *path, uint32_t seed, uint32_t count,
                        uint16_t max_dlen)
{
    return rdb_vec_generate_ts_dist(path, seed, count, max_dlen, NULL);
}

