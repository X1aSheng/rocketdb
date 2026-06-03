/*****************************************************************************
 * rdb_client.c — RocketDB TCP Client
 *
 * Connects to a RocketDB server and provides an interactive shell
 * for executing KVDB and TSDB operations against a remote server.
 *
 * Supports both POSIX (Linux/macOS) and Windows (Winsock).
 *
 * Usage:
 *   rocketdb [-h host] [-p port]
 *
 * Default: localhost:8080
 *
 * Copyright (c) 2015 XiaSheng(info@zhis.net)
 * SPDX-License-Identifier: MIT
 *****************************************************************************/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <ctype.h>

/* Platform-specific networking */
#ifdef _WIN32
#define _WINSOCK_DEPRECATED_NO_WARNINGS
#include <winsock2.h>
#include <ws2tcpip.h>
#pragma comment(lib, "ws2_32.lib")
#define CLOSE_SOCK(fd) closesocket(fd)
typedef int socklen_t;
/* Windows lacks strcasecmp */
static int strcasecmp(const char *a, const char *b) {
    while (*a && *b) {
        int ca = toupper((unsigned char)*a);
        int cb = toupper((unsigned char)*b);
        if (ca != cb) return ca - cb;
        a++; b++;
    }
    return (unsigned char)*a - (unsigned char)*b;
}
#else
#define _POSIX_C_SOURCE 200112L
#include <unistd.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netdb.h>
#include <arpa/inet.h>
#define CLOSE_SOCK(fd) close(fd)
#endif

#define DEFAULT_HOST "127.0.0.1"
#define DEFAULT_PORT 8080
#define BUF_SIZE     4096

static const char *g_host = DEFAULT_HOST;
static int         g_port = DEFAULT_PORT;

#ifdef _WIN32
static int init_winsock(void) {
    WSADATA wsa;
    return WSAStartup(MAKEWORD(2, 2), &wsa);
}
#endif

static int connect_server(void) {
    struct hostent *he = gethostbyname(g_host);
    if (!he) {
        fprintf(stderr, "Unknown host: %s\n", g_host);
        return -1;
    }

    int fd = (int)socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        perror("socket");
        return -1;
    }

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons((uint16_t)g_port);
    memcpy(&addr.sin_addr, he->h_addr_list[0], (size_t)he->h_length);

    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("connect");
        CLOSE_SOCK(fd);
        return -1;
    }

    return fd;
}

static int send_recv(int fd, const char *cmd, char *resp, size_t resp_len) {
    char buf[BUF_SIZE];
    size_t len = strlen(cmd);
    memcpy(buf, cmd, len);
    buf[len] = '\n';
    buf[len + 1] = '\0';

    if (send(fd, buf, len + 1, 0) != (int)(len + 1))
        return -1;

    size_t pos = 0;
    while (pos < resp_len - 1) {
        char ch;
        int n = recv(fd, &ch, 1, 0);
        if (n <= 0) break;
        resp[pos++] = ch;
        if (ch == '\n') break;
    }
    resp[pos] = '\0';
    return 0;
}

static void print_banner(void) {
    printf("\n");
    printf("  RocketDB Client v1.0\n");
    printf("  Connected to %s:%d\n", g_host, g_port);
    printf("  Commands: SET, GET, DEL, EXISTS, APPEND, QUERY,\n");
    printf("            STATS, SPACE, BYE\n");
    printf("  Values are hex-encoded.\n");
    printf("\n");
}

int main(int argc, char **argv) {
#ifdef _WIN32
    if (init_winsock() != 0) {
        fprintf(stderr, "WSAStartup failed\n");
        return 1;
    }
#endif

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-h") == 0 && i + 1 < argc)
            g_host = argv[++i];
        else if (strcmp(argv[i], "-p") == 0 && i + 1 < argc)
            g_port = atoi(argv[++i]);
        else if (strcmp(argv[i], "--help") == 0) {
            printf("Usage: %s [-h host] [-p port]\n", argv[0]);
            return 0;
        }
    }

    int fd = connect_server();
    if (fd < 0)
        return 1;

    char resp[BUF_SIZE];
    if (send_recv(fd, "", resp, sizeof(resp)) == 0)
        printf("%s", resp);

    print_banner();

    char line[BUF_SIZE];
    printf("> ");
    fflush(stdout);

    while (fgets(line, sizeof(line), stdin)) {
        size_t len = strlen(line);
        while (len > 0 && (line[len-1] == '\n' || line[len-1] == '\r'))
            line[--len] = '\0';

        if (len == 0) {
            printf("> ");
            fflush(stdout);
            continue;
        }

        const char *cmd = line;
        while (*cmd && isspace((unsigned char)*cmd)) cmd++;

        if (strcasecmp(cmd, "BYE") == 0 || strcasecmp(cmd, "QUIT") == 0
            || strcasecmp(cmd, "EXIT") == 0) {
            send_recv(fd, "BYE", resp, sizeof(resp));
            printf("Bye.\n");
            break;
        }

        if (send_recv(fd, cmd, resp, sizeof(resp)) == 0) {
            printf("%s\n", resp);
        } else {
            printf("Error: connection lost\n");
            break;
        }

        printf("> ");
        fflush(stdout);
    }

    CLOSE_SOCK(fd);
#ifdef _WIN32
    WSACleanup();
#endif
    return 0;
}
