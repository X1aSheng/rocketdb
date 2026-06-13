# RocketDB — Multi-stage Docker Build
#
# Stage 1: Build the library, examples, and tests.
# Stage 2: Minimal runtime image for smoke testing.
#
# Usage:
#   docker build -t rocketdb:latest .
#   docker run --rm rocketdb:latest
#   docker run --rm rocketdb:latest ctest --test-dir /build --output-on-failure -V

# ── Stage 1: Builder ──────────────────────────────────────────────────────────
FROM ubuntu:24.04 AS builder

LABEL description="RocketDB Flash Storage Engine — Build & Test"

RUN apt-get update -qq && \
    DEBIAN_FRONTEND=noninteractive apt-get install -y -qq \
        build-essential \
        cmake \
        ninja-build \
        python3 \
        git \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /src
COPY . .

RUN cmake -S . -B /build -G Ninja \
        -DCMAKE_BUILD_TYPE=Release \
        -DBUILD_TESTS=ON \
        -DBUILD_EXAMPLES=ON \
        -DBUILD_PERF=ON \
    && cmake --build /build --parallel

# ── Stage 2: Test runner ──────────────────────────────────────────────────────
FROM ubuntu:24.04

RUN apt-get update -qq && \
    DEBIAN_FRONTEND=noninteractive apt-get install -y -qq \
        cmake \
        python3 \
    && rm -rf /var/lib/apt/lists/*

COPY --from=builder /build /build
COPY --from=builder /src/tools /src/tools

WORKDIR /src

CMD ["ctest", "--test-dir", "/build", "--output-on-failure"]
