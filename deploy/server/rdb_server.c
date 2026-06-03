/*****************************************************************************
 * rdb_server.c — RocketDB TCP Server Gateway
 *
 * Exposes RocketDB KVDB and TSDB operations over a simple text protocol
 * on a TCP socket.  Uses the simulator flash backend for cloud/server
 * deployment validation.
 *
 * NOTE: This is a Linux/POSIX-only build (Docker target).
 * The client (deploy/client/rdb_client.c) is portable.
 *
 * Protocol (line-based, \n terminated):
 *   SET <key> <hex_value>
 *   GET <key>
 *   DEL <key>
 *   EXISTS <key>
 *   APPEND <ts> <hex_data>
 *   QUERY <from> <to>
 *   STATS
 *   WEAR
 *   SPACE
 *   BYE
 *
 * Response format:
 *   +OK [message]
 *   -ERR [error code] [message]
 *   VALUE <len> <hex_data>
 *
 * Copyright (c) 2015 XiaSheng(info@zhis.net)
 * SPDX-License-Identifier: MIT
 *****************************************************************************/

#define _POSIX_C_SOURCE 200112L

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stddef.h>
#include <ctype.h>
#include <unistd.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>

/* RocketDB headers */
#include "rocketdb.h"

/* Simulator flash backend */
#include "sim_flash.h"

/* Forward declaration for error name helper */
static const char *rdb_err_name(rdb_err_t err);

/* ── Configuration ─────────────────────────────────────────────────────── */

#define DEFAULT_PORT     8080
#define MAX_BACKLOG      16
#define BUF_SIZE         4096
#define FLASH_SIZE       (256u * 1024u)
#define SECTOR_SIZE      4096u
#define PAGE_SIZE        256u
#define KV_PART_SIZE     (128u * 1024u)
#define TS_PART_SIZE     (128u * 1024u)
#define KV_SECTOR_CNT    (KV_PART_SIZE / SECTOR_SIZE)
#define TS_SECTOR_CNT    (TS_PART_SIZE / SECTOR_SIZE)

/* ── Global state ──────────────────────────────────────────────────────── */

static uint8_t              g_flash_buf[FLASH_SIZE];
static sim_flash_t          g_flash;
static rdb_partition_t      g_kv_part;
static rdb_kvdb_t           g_kvdb;
static rdb_kv_sector_meta_t g_kv_meta[KV_SECTOR_CNT];
static rdb_partition_t      g_ts_part;
static rdb_tsdb_t           g_tsdb;
static uint32_t             g_ts_ec[TS_SECTOR_CNT];
static int                  g_running = 1;

/* ── Flash ops (simulator backend) ──────────────────────────────────────── */

static int fl_read(void *ctx, uint32_t addr, uint8_t *buf, size_t len) {
    (void)ctx;
    return sim_flash_read(&g_flash, addr, buf, len);
}
static int fl_write(void *ctx, uint32_t addr, const uint8_t *buf, size_t len) {
    (void)ctx;
    return sim_flash_write(&g_flash, addr, buf, len);
}
static int fl_erase(void *ctx, uint32_t addr) {
    (void)ctx;
    return sim_flash_erase(&g_flash, addr);
}
static void fl_lock(void *ctx)   { (void)ctx; }
static void fl_unlock(void *ctx) { (void)ctx; }
static void fl_yield(void *ctx)  { (void)ctx; }

static rdb_flash_ops_t g_ops = {
    .read = fl_read, .write = fl_write, .erase = fl_erase,
    .lock = fl_lock, .unlock = fl_unlock, .yield = fl_yield
};

/* ── Initialisation ────────────────────────────────────────────────────── */

static int init_databases(void) {
    if (sim_flash_init(&g_flash, g_flash_buf, FLASH_SIZE,
                       SECTOR_SIZE, PAGE_SIZE, 0) != 0)
        return -1;

    /* KVDB partition */
    g_kv_part = (rdb_partition_t){
        .name = "kvdb", .base_addr = 0,
        .total_size = KV_PART_SIZE, .sector_size = SECTOR_SIZE,
        .write_gran = 0, .ops = &g_ops, .flash_ctx = NULL
    };
    memset(&g_kvdb, 0, sizeof(g_kvdb));
    g_kvdb.part = &g_kv_part;
    g_kvdb.sectors = g_kv_meta;
    g_kvdb.sector_cnt = (uint8_t)KV_SECTOR_CNT;
    if (rdb_kvdb_format(&g_kvdb) != RDB_OK) return -1;
    if (rdb_kvdb_init(&g_kvdb, &g_kv_part, g_kv_meta) != RDB_OK) return -1;

    /* TSDB partition */
    g_ts_part = (rdb_partition_t){
        .name = "tsdb", .base_addr = KV_PART_SIZE,
        .total_size = TS_PART_SIZE, .sector_size = SECTOR_SIZE,
        .write_gran = 0, .ops = &g_ops, .flash_ctx = NULL
    };
    memset(&g_tsdb, 0, sizeof(g_tsdb));
    g_tsdb.part = &g_ts_part;
    g_tsdb.erase_cnts = g_ts_ec;
    g_tsdb.sector_cnt = (uint8_t)TS_SECTOR_CNT;
    if (rdb_tsdb_format(&g_tsdb) != RDB_OK) return -1;
    if (rdb_tsdb_init(&g_tsdb, &g_ts_part, g_ts_ec) != RDB_OK) return -1;

    return 0;
}

/* ── Hex helpers ───────────────────────────────────────────────────────── */

static int hex_val(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static int hex_decode(const char *hex, uint8_t *out, size_t max_len) {
    size_t len = strlen(hex);
    if (len % 2 != 0) return -1;
    size_t n = len / 2;
    if (n > max_len) return -1;
    for (size_t i = 0; i < n; i++) {
        int hi = hex_val(hex[i * 2]);
        int lo = hex_val(hex[i * 2 + 1]);
        if (hi < 0 || lo < 0) return -1;
        out[i] = (uint8_t)((hi << 4) | lo);
    }
    return (int)n;
}

static void hex_encode(const uint8_t *data, size_t len, char *out) {
    static const char hex[] = "0123456789abcdef";
    for (size_t i = 0; i < len; i++) {
        out[i * 2]     = hex[(data[i] >> 4) & 0x0F];
        out[i * 2 + 1] = hex[data[i] & 0x0F];
    }
    out[len * 2] = '\0';
}

/* ── Safe send / recv ──────────────────────────────────────────────────── */

static int send_str(int fd, const char *s) {
    size_t len = strlen(s);
    return (int)write(fd, s, len);
}

static int send_ok(int fd, const char *msg) {
    char buf[BUF_SIZE];
    snprintf(buf, sizeof(buf), "+OK %s\n", msg);
    return send_str(fd, buf);
}

static int send_err(int fd, const char *code, const char *msg) {
    char buf[BUF_SIZE];
    snprintf(buf, sizeof(buf), "-ERR %s %s\n", code, msg);
    return send_str(fd, buf);
}

/* ── Command handlers ──────────────────────────────────────────────────── */

static int handle_set(int fd, const char *key, const char *hex_val) {
    if (!key || !*key || !hex_val)
        return send_err(fd, "PARAM", "Usage: SET <key> <hex_value>");

    uint8_t val[BUF_SIZE];
    int vlen = hex_decode(hex_val, val, sizeof(val));
    if (vlen < 0)
        return send_err(fd, "PARAM", "Invalid hex value");

    rdb_err_t rc = rdb_kvdb_set(&g_kvdb, key, val, (uint16_t)vlen);
    if (rc == RDB_OK) {
        char reply[128];
        snprintf(reply, sizeof(reply), "set %s (%d bytes)", key, vlen);
        return send_ok(fd, reply);
    }
    return send_err(fd, "KVDB", rdb_err_name(rc));
}

static int handle_get(int fd, const char *key) {
    if (!key || !*key)
        return send_err(fd, "PARAM", "Usage: GET <key>");

    uint8_t val[BUF_SIZE];
    uint16_t vlen = 0;
    rdb_err_t rc = rdb_kvdb_get(&g_kvdb, key, val, sizeof(val), &vlen);
    if (rc == RDB_OK || rc == RDB_ERR_TOO_LARGE) {
        char hex[BUF_SIZE * 2 + 1];
        hex_encode(val, vlen, hex);
        char reply[BUF_SIZE + 64];
        snprintf(reply, sizeof(reply), "VALUE %u %s\n", (unsigned)vlen, hex);
        return send_str(fd, reply);
    }
    if (rc == RDB_ERR_NOT_FOUND) {
        char reply[128];
        snprintf(reply, sizeof(reply), "key '%s' not found", key);
        return send_err(fd, "NOT_FOUND", reply);
    }
    return send_err(fd, "KVDB", rdb_err_name(rc));
}

static int handle_del(int fd, const char *key) {
    if (!key || !*key)
        return send_err(fd, "PARAM", "Usage: DEL <key>");

    rdb_err_t rc = rdb_kvdb_delete(&g_kvdb, key);
    if (rc == RDB_OK) {
        char reply[128];
        snprintf(reply, sizeof(reply), "deleted '%s'", key);
        return send_ok(fd, reply);
    }
    if (rc == RDB_ERR_NOT_FOUND) {
        char reply[128];
        snprintf(reply, sizeof(reply), "key '%s' not found", key);
        return send_err(fd, "NOT_FOUND", reply);
    }
    return send_err(fd, "KVDB", rdb_err_name(rc));
}

static int handle_exists(int fd, const char *key) {
    if (!key || !*key)
        return send_err(fd, "PARAM", "Usage: EXISTS <key>");

    rdb_err_t rc = rdb_kvdb_exists(&g_kvdb, key);
    if (rc == RDB_OK)
        return send_ok(fd, "exists");
    return send_err(fd, "NOT_FOUND", "key does not exist");
}

static int handle_append(int fd, uint32_t ts, const char *hex_data) {
    if (!hex_data)
        return send_err(fd, "PARAM", "Usage: APPEND <ts> <hex_data>");

    uint8_t data[BUF_SIZE];
    int dlen = hex_decode(hex_data, data, sizeof(data));
    if (dlen < 0)
        return send_err(fd, "PARAM", "Invalid hex data");

    rdb_err_t rc = rdb_tsdb_append(&g_tsdb, ts, data, (uint16_t)dlen);
    if (rc == RDB_OK) {
        char reply[128];
        snprintf(reply, sizeof(reply), "appended ts=%u len=%d", (unsigned)ts, dlen);
        return send_ok(fd, reply);
    }
    return send_err(fd, "TSDB", rdb_err_name(rc));
}

/* Query callback — counts results */
typedef struct {
    int fd;
    int count;
} query_ctx_t;

static int query_cb(uint32_t ts, const void *data, uint16_t len, void *arg) {
    query_ctx_t *q = (query_ctx_t *)arg;
    char hex[128];
    uint16_t show = (len > 64) ? 64 : len;
    hex_encode((const uint8_t *)data, show, hex);
    char line[BUF_SIZE];
    snprintf(line, sizeof(line), "  ts=%u len=%u data=%s%s\n",
             (unsigned)ts, (unsigned)len, hex,
             (len > 64) ? "..." : "");
    send_str(q->fd, line);
    q->count++;
    return RDB_ITER_CONTINUE;
}

static int handle_query(int fd, uint32_t from, uint32_t to) {
    query_ctx_t q = { .fd = fd, .count = 0 };
    rdb_err_t rc = rdb_tsdb_query(&g_tsdb, from, to, query_cb, &q);
    if (rc != RDB_OK)
        return send_err(fd, "TSDB", rdb_err_name(rc));

    char reply[128];
    snprintf(reply, sizeof(reply), "query returned %d records", q.count);
    return send_ok(fd, reply);
}

static int handle_stats(int fd) {
    rdb_kv_stats_t ks;
    rdb_kvdb_get_stats(&g_kvdb, &ks);
    rdb_ts_stats_t ts;
    rdb_tsdb_get_stats(&g_tsdb, &ts);

    uint32_t ktotal, kused, kavail;
    rdb_kvdb_space_info(&g_kvdb, &ktotal, &kused, &kavail);

    char buf[BUF_SIZE];
    int n = snprintf(buf, sizeof(buf),
        "KVDB: read_ops=%u write_ops=%u delete_ops=%u gc_runs=%u live=%u avail=%u\n"
        "TSDB: write_ops=%u sector_rotations=%u records_lost=%u total_count=%u\n",
        (unsigned)ks.read_ops, (unsigned)ks.write_ops, (unsigned)ks.delete_ops,
        (unsigned)ks.gc_runs, (unsigned)kused, (unsigned)kavail,
        (unsigned)ts.write_ops, (unsigned)ts.sector_rotations,
        (unsigned)ts.records_lost, (unsigned)rdb_tsdb_count(&g_tsdb));
    return send_str(fd, buf);
}

static int handle_space(int fd) {
    uint32_t total, used, avail;
    rdb_kvdb_space_info(&g_kvdb, &total, &used, &avail);
    uint32_t tsc = rdb_tsdb_count(&g_tsdb);

    char buf[256];
    snprintf(buf, sizeof(buf),
        "KVDB: total=%u used=%u avail=%u  TSDB: records=%u\n",
        (unsigned)total, (unsigned)used, (unsigned)avail, (unsigned)tsc);
    return send_str(fd, buf);
}

/* ── Parse and dispatch a command line ─────────────────────────────────── */

static int handle_command(int fd, char *line) {
    /* Strip trailing newline */
    size_t len = strlen(line);
    while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r'))
        line[--len] = '\0';

    if (len == 0)
        return 0;

    /* Tokenize */
    char *cmd = strtok(line, " \t");
    if (!cmd) return 0;

    if (strcasecmp(cmd, "BYE") == 0)
        return send_str(fd, "+OK bye\n"), -1;

    if (strcasecmp(cmd, "STATS") == 0)
        return handle_stats(fd);

    if (strcasecmp(cmd, "SPACE") == 0)
        return handle_space(fd);

    if (strcasecmp(cmd, "SET") == 0) {
        char *key = strtok(NULL, " \t");
        char *val = strtok(NULL, "");
        if (!val) val = "";
        while (*val == ' ' || *val == '\t') val++;
        return handle_set(fd, key, val);
    }

    if (strcasecmp(cmd, "GET") == 0) {
        char *key = strtok(NULL, " \t");
        return handle_get(fd, key);
    }

    if (strcasecmp(cmd, "DEL") == 0) {
        char *key = strtok(NULL, " \t");
        return handle_del(fd, key);
    }

    if (strcasecmp(cmd, "EXISTS") == 0) {
        char *key = strtok(NULL, " \t");
        return handle_exists(fd, key);
    }

    if (strcasecmp(cmd, "APPEND") == 0) {
        char *ts_str = strtok(NULL, " \t");
        char *data = strtok(NULL, "");
        if (!data) data = "";
        while (*data == ' ' || *data == '\t') data++;
        if (!ts_str) return send_err(fd, "PARAM", "Usage: APPEND <ts> <hex_data>");
        uint32_t ts = (uint32_t)atol(ts_str);
        return handle_append(fd, ts, data);
    }

    if (strcasecmp(cmd, "QUERY") == 0) {
        char *from_str = strtok(NULL, " \t");
        char *to_str = strtok(NULL, " \t");
        if (!from_str || !to_str)
            return send_err(fd, "PARAM", "Usage: QUERY <from> <to>");
        return handle_query(fd, (uint32_t)atol(from_str), (uint32_t)atol(to_str));
    }

    return send_err(fd, "UNKNOWN", cmd);
}

/* ── Client handler ────────────────────────────────────────────────────── */

static void handle_client(int fd) {
    char buf[BUF_SIZE];
    size_t pos = 0;

    send_str(fd, "+OK RocketDB Server ready\n");

    while (g_running) {
        /* Read one line */
        char ch;
        while (pos < sizeof(buf) - 1) {
            ssize_t n = read(fd, &ch, 1);
            if (n <= 0) goto done;
            if (ch == '\n') break;
            buf[pos++] = ch;
        }
        buf[pos] = '\0';

        if (handle_command(fd, buf) < 0)
            goto done;
        pos = 0;
    }

done:
    close(fd);
}

/* ── Signal handling ───────────────────────────────────────────────────── */

static void sigint_handler(int sig) {
    (void)sig;
    g_running = 0;
}

/* ── Main ──────────────────────────────────────────────────────────────── */

static void print_usage(const char *prog) {
    fprintf(stderr, "Usage: %s [-p port] [-h]\n", prog);
    fprintf(stderr, "  -p port    TCP port (default: %d)\n", DEFAULT_PORT);
    fprintf(stderr, "  -h         Show this help\n");
}

int main(int argc, char **argv) {
    int port = DEFAULT_PORT;

    /* Parse args */
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-p") == 0 && i + 1 < argc)
            port = atoi(argv[++i]);
        else if (strcmp(argv[i], "-h") == 0) {
            print_usage(argv[0]);
            return 0;
        }
    }

    /* Initialise databases */
    if (init_databases() != 0) {
        fprintf(stderr, "Failed to initialise databases\n");
        return 1;
    }
    fprintf(stderr, "RocketDB databases initialised\n");

    /* Create socket */
    int srv = socket(AF_INET, SOCK_STREAM, 0);
    if (srv < 0) {
        perror("socket");
        return 1;
    }

    int opt = 1;
    setsockopt(srv, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons((uint16_t)port);

    if (bind(srv, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("bind");
        close(srv);
        return 1;
    }

    if (listen(srv, MAX_BACKLOG) < 0) {
        perror("listen");
        close(srv);
        return 1;
    }

    signal(SIGINT, sigint_handler);
    signal(SIGPIPE, SIG_IGN);

    fprintf(stderr, "RocketDB server listening on port %d\n", port);

    while (g_running) {
        struct sockaddr_in client;
        socklen_t client_len = sizeof(client);
        int fd = accept(srv, (struct sockaddr *)&client, &client_len);
        if (fd < 0) {
            if (g_running) perror("accept");
            continue;
        }
        fprintf(stderr, "Connection from %s\n", inet_ntoa(client.sin_addr));

        /* Fork for simplicity */
        pid_t pid = fork();
        if (pid == 0) {
            /* Child */
            close(srv);
            handle_client(fd);
            exit(0);
        }
        close(fd);
    }

    close(srv);
    fprintf(stderr, "RocketDB server shutting down\n");
    return 0;
}

/* ── Error code names ──────────────────────────────────────────────────── */

const char *rdb_err_name(rdb_err_t err) {
    switch (err) {
        case RDB_OK:               return "OK";
        case RDB_ERR_PARAM:        return "PARAM";
        case RDB_ERR_FLASH:        return "FLASH";
        case RDB_ERR_NO_SPACE:     return "NO_SPACE";
        case RDB_ERR_NOT_FOUND:    return "NOT_FOUND";
        case RDB_ERR_TOO_LARGE:    return "TOO_LARGE";
        case RDB_ERR_CRC:          return "CRC";
        case RDB_ERR_CORRUPT:      return "CORRUPT";
        case RDB_ERR_NOT_INIT:     return "NOT_INIT";
        case RDB_ERR_GC_FAIL:      return "GC_FAIL";
        case RDB_ERR_ITER_END:     return "ITER_END";
        case RDB_ERR_FULL:         return "FULL";
        case RDB_ERR_BUSY:         return "BUSY";
        case RDB_ERR_TIME_EXHAUSTED: return "TIME_EXHAUSTED";
        default:                   return "UNKNOWN";
    }
}
