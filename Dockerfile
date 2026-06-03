# RocketDB — Multi-stage Docker build
#
# Build stage: compile the engine, tests, and server gateway
# Runtime stage: minimal Alpine with the server binary
#
# Usage:
#   docker build -t rocketdb .
#   docker run --rm -p 8080:8080 rocketdb

# ── Stage 1: Build ───────────────────────────────────────────────────────────
FROM alpine:3.20 AS builder

RUN apk add --no-cache clang make python3 py3-pip

WORKDIR /src

# Copy all sources
COPY CHANGELOG.md CMakeLists.txt CONTRIBUTING.md Doxyfile LICENSE Makefile README.md ./
COPY src/ src/
COPY interface/ interface/
COPY examples/ examples/
COPY tests/ tests/
COPY tools/ tools/
COPY deploy/ deploy/

# Compile the server gateway
RUN mkdir -p tests/out && \
    clang -Wall -Wextra -std=c99 -O2 -g \
    -Isrc -Itests/sim \
    -o tests/out/rocketdb_server \
    deploy/server/rdb_server.c \
    src/rocketdb_kvdb.c src/rocketdb_tsdb.c \
    tests/sim/sim_flash.c tests/sim/sim_fault.c \
    tests/sim/sim_crypto.c tests/sim/sim_trace.c \
    -lz

# Also compile full test suite
RUN for t in test_kvdb_basic test_kvdb_stress test_tsdb_basic test_tsdb_stress \
    test_kvdb_cache test_integration test_example test_fault_injection; do \
    clang -Wall -Wextra -std=c99 -O2 -g \
    -Isrc -Itests/sim \
    -o tests/out/$t \
    src/rocketdb_kvdb.c src/rocketdb_tsdb.c \
    tests/sim/test_framework.c tests/sim/sim_flash.c \
    tests/sim/sim_fault.c tests/sim/sim_crypto.c \
    tests/sim/sim_dist.c tests/sim/sim_trace.c \
    tests/sim/$t.c; \
    done

# Compile the client
RUN clang -Wall -Wextra -std=c99 -O2 -g \
    -Isrc -Itests/sim \
    -o tests/out/rocketdb_client \
    deploy/client/rdb_client.c

# ── Stage 2: Runtime ─────────────────────────────────────────────────────────
FROM alpine:3.20

RUN apk add --no-cache python3

COPY --from=builder /src/tests/out/rocketdb_server /usr/local/bin/rocketdbd
COPY --from=builder /src/tests/out/rocketdb_client /usr/local/bin/rocketdb
COPY --from=builder /src/tests/out/test_* /usr/local/bin/
COPY --from=builder /src/tools/rdbdump/rdbdump.py /usr/local/bin/rdbdump.py
COPY --from=builder /src/tools/rdbdump/verify_offline.py /usr/local/bin/
COPY --from=builder /src/tools/rdbdump/README.md /usr/local/share/rdbdump/

EXPOSE 8080

# Default: run the server gateway
CMD ["rocketdbd", "-p", "8080"]
