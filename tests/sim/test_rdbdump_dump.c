#include "sim_flash.h"
#include "../../src/rocketdb.h"
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include <direct.h>
#else
#include <sys/stat.h>
#endif

#define FLASH_SIZE  (32u * 1024u)
#define SECTOR_SIZE 4096u
#define PAGE_SIZE   256u
#define SECTOR_CNT  (FLASH_SIZE / SECTOR_SIZE)

static uint8_t g_flash_buf[FLASH_SIZE];
static sim_flash_t *g_flash;

static int fl_read(void *ctx, uint32_t addr, uint8_t *buf, size_t len)
{
    (void)ctx;
    return sim_flash_read(g_flash, addr, buf, len);
}

static int fl_write(void *ctx, uint32_t addr, const uint8_t *buf, size_t len)
{
    (void)ctx;
    return sim_flash_write(g_flash, addr, buf, len);
}

static int fl_erase(void *ctx, uint32_t addr)
{
    (void)ctx;
    return sim_flash_erase(g_flash, addr);
}

static void fl_lock(void *ctx) { (void)ctx; }
static void fl_unlock(void *ctx) { (void)ctx; }
static void fl_yield(void *ctx) { (void)ctx; }

static rdb_flash_ops_t g_ops = {
    .read = fl_read,
    .write = fl_write,
    .erase = fl_erase,
    .lock = fl_lock,
    .unlock = fl_unlock,
    .yield = fl_yield,
};

static int ensure_dir(const char *path)
{
#ifdef _WIN32
    if (_mkdir(path) == 0 || errno == EEXIST) return 0;
#else
    if (mkdir(path, 0777) == 0 || errno == EEXIST) return 0;
#endif
    return -1;
}

static void make_path(char *dst, size_t dst_len, const char *dir, const char *name)
{
    size_t n = strlen(dir);
#ifdef _WIN32
    const char *default_sep = "\\";
#else
    const char *default_sep = "/";
#endif
    const char *sep = (n > 0 && (dir[n - 1] == '\\' || dir[n - 1] == '/')) ? "" : default_sep;
    snprintf(dst, dst_len, "%s%s%s", dir, sep, name);
}

static int write_manifest(const char *path, const char *kind, const char *image_name)
{
    FILE *fp = fopen(path, "w");
    if (!fp) return -1;
    fprintf(fp,
            "{\n"
            "  \"kind\": \"%s\",\n"
            "  \"input\": \"%s\",\n"
            "  \"sector_size\": %u,\n"
            "  \"write_gran\": 0,\n"
            "  \"base_addr\": 0,\n"
            "  \"total_size\": %u\n"
            "}\n",
            kind, image_name, (unsigned)SECTOR_SIZE, (unsigned)FLASH_SIZE);
    return fclose(fp);
}

static int generate_kvdb(const char *out_dir)
{
    sim_flash_t flash;
    rdb_kvdb_t db;
    rdb_kv_sector_meta_t meta[SECTOR_CNT];
    rdb_partition_t part = {
        .name = "rdbdump-kvdb",
        .base_addr = 0,
        .total_size = FLASH_SIZE,
        .sector_size = SECTOR_SIZE,
        .write_gran = 0,
        .ops = &g_ops,
        .flash_ctx = NULL,
    };
    char image[512];
    char manifest[512];
    const uint8_t count[] = { 1u, 2u, 3u, 4u };

    g_flash = &flash;
    memset(&flash, 0, sizeof(flash));
    memset(&db, 0, sizeof(db));
    memset(meta, 0, sizeof(meta));
    if (sim_flash_init(&flash, g_flash_buf, FLASH_SIZE, SECTOR_SIZE, PAGE_SIZE, 0) != 0) return -1;
    db.part = &part;
    db.sectors = meta;
    db.sector_cnt = (uint8_t)SECTOR_CNT;
    if (rdb_kvdb_format(&db) != RDB_OK) return -1;
    if (rdb_kvdb_init(&db, &part, meta) != RDB_OK) return -1;
    if (rdb_kvdb_set(&db, "wifi", (const uint8_t *)"lab-net", 7) != RDB_OK) return -1;
    if (rdb_kvdb_set(&db, "mode", (const uint8_t *)"normal", 6) != RDB_OK) return -1;
    if (rdb_kvdb_set(&db, "mode", (const uint8_t *)"service", 7) != RDB_OK) return -1;
    if (rdb_kvdb_delete(&db, "wifi") != RDB_OK) return -1;
    if (rdb_kvdb_set(&db, "count", count, sizeof(count)) != RDB_OK) return -1;

    make_path(image, sizeof(image), out_dir, "rdbdump_kvdb.bin");
    make_path(manifest, sizeof(manifest), out_dir, "rdbdump_kvdb.json");
    if (sim_flash_save_file(&flash, image) != 0) return -1;
    if (write_manifest(manifest, "kvdb", "rdbdump_kvdb.bin") != 0) return -1;
    return 0;
}

static int generate_tsdb(const char *out_dir)
{
    sim_flash_t flash;
    rdb_tsdb_t db;
    uint32_t erase_cnts[SECTOR_CNT];
    rdb_partition_t part = {
        .name = "rdbdump-tsdb",
        .base_addr = 0,
        .total_size = FLASH_SIZE,
        .sector_size = SECTOR_SIZE,
        .write_gran = 0,
        .ops = &g_ops,
        .flash_ctx = NULL,
    };
    char image[512];
    char manifest[512];

    g_flash = &flash;
    memset(&flash, 0, sizeof(flash));
    memset(&db, 0, sizeof(db));
    memset(erase_cnts, 0, sizeof(erase_cnts));
    if (sim_flash_init(&flash, g_flash_buf, FLASH_SIZE, SECTOR_SIZE, PAGE_SIZE, 0) != 0) return -1;
    db.part = &part;
    db.erase_cnts = erase_cnts;
    db.sector_cnt = (uint8_t)SECTOR_CNT;
    rdb_err_t rc = rdb_tsdb_format(&db);
    if (rc != RDB_OK) {
        fprintf(stderr, "tsdb: format failed: %d\n", rc);
        return -1;
    }
    rc = rdb_tsdb_init(&db, &part, erase_cnts);
    if (rc != RDB_OK) {
        fprintf(stderr, "tsdb: init failed: %d\n", rc);
        return -1;
    }
    for (uint32_t i = 0; i < 24u; i++) {
        uint8_t data[5] = {
            (uint8_t)i,
            (uint8_t)(i + 1u),
            (uint8_t)(0xA0u + i),
            (uint8_t)(0x55u ^ i),
            (uint8_t)(i * 3u),
        };
        rc = rdb_tsdb_append(&db, 1000u + i, data, sizeof(data));
        if (rc != RDB_OK) {
            fprintf(stderr, "tsdb: append %u failed: %d\n", (unsigned)i, rc);
            return -1;
        }
    }

    make_path(image, sizeof(image), out_dir, "rdbdump_tsdb.bin");
    make_path(manifest, sizeof(manifest), out_dir, "rdbdump_tsdb.json");
    if (sim_flash_save_file(&flash, image) != 0) {
        fprintf(stderr, "tsdb: save failed\n");
        return -1;
    }
    if (write_manifest(manifest, "tsdb", "rdbdump_tsdb.bin") != 0) {
        fprintf(stderr, "tsdb: manifest failed\n");
        return -1;
    }
    return 0;
}

int main(int argc, char **argv)
{
#ifdef _WIN32
    const char *default_out_dir = "tests\\out";
#else
    const char *default_out_dir = "tests/out";
#endif
    const char *out_dir = (argc > 1) ? argv[1] : default_out_dir;
    if (ensure_dir(out_dir) != 0) {
        fprintf(stderr, "failed to create output dir: %s\n", out_dir);
        return 1;
    }
    if (generate_kvdb(out_dir) != 0) {
        fprintf(stderr, "failed to generate KVDB rdbdump fixture\n");
        return 1;
    }
    if (generate_tsdb(out_dir) != 0) {
        fprintf(stderr, "failed to generate TSDB rdbdump fixture\n");
        return 1;
    }
    printf("generated rdbdump fixtures in %s\n", out_dir);
    return 0;
}
