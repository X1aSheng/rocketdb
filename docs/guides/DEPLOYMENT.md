# RocketDB Cloud Deployment Guide

## Overview

RocketDB is an embedded Flash storage engine — it is a library, not a
network-accessible server.  Cloud deployment options focus on CI/CD
pipeline integration, cross-platform build validation, and containerised
test execution.

## Docker / Container

Docker support was removed in v1.6.0 because RocketDB is an embedded library,
not a network service.  For CI pipeline validation, use the CMake build system
directly (see [Build And Test](../../README.md#build-and-test)).

If containerised test execution is required, create a minimal Dockerfile:
```dockerfile
FROM ubuntu:24.04
RUN apt-get update && apt-get install -y cmake ninja-build clang python3
COPY . /rocketdb
WORKDIR /rocketdb
RUN cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release \
    -DBUILD_TESTS=ON -DBUILD_EXAMPLES=ON && \
    cmake --build build && \
    ctest --test-dir build --output-on-failure
```

## Kubernetes

Kubernetes manifests are not provided.  For containerised CI validation
on K8s, use a `Job` resource that builds and runs the test suite as
described above.

## Cloud Server Validation

When deploying to a cloud VM (e.g. Alibaba Cloud, AWS EC2, Azure):

```bash
# 1. Install dependencies
sudo apt-get update && sudo apt-get install -y cmake ninja-build gcc python3

# 2. Clone and build
git clone <repo-url> rocketdb
cd rocketdb
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release \
    -DBUILD_TESTS=ON -DBUILD_EXAMPLES=ON
cmake --build build --parallel

# 3. Run tests
ctest --test-dir build --output-on-failure

# 4. Run offline rdbdump verification (requires Python)
python3 tools/rdbdump/rdbdump.py verify --strict \
    --manifest tests/out/rdbdump_kvdb.json \
    --input tests/out/rdbdump_kvdb.bin
```

## Architecture Notes

- **Bare-metal targets**: RocketDB runs on any Cortex-M or similar MCU
  with NOR Flash.  No OS dependencies beyond the HAL layer.
- **Linux host simulation**: The `tests/sim/` framework provides a
  deterministic in-memory Flash simulator for host-side testing.
- **Docker CI**: The Dockerfile matches the GitHub Actions CI workflow
  (Ubuntu + CMake + Ninja) so local Docker builds reproduce CI results.
