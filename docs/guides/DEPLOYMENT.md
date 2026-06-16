# RocketDB Cloud Deployment Guide

## Overview

RocketDB is an embedded Flash storage engine — it is a library, not a
network-accessible server.  Cloud deployment options focus on CI/CD
pipeline integration, cross-platform build validation, and containerised
test execution.

## Docker

A multi-stage Dockerfile is provided at the repository root:

```bash
# Build the image
docker build -t rocketdb:latest .

# Run the full CTest suite
docker run --rm rocketdb:latest

# Or use docker-compose
docker-compose up
```

The Dockerfile:
1. **Stage 1 (builder)**: Ubuntu 24.04 with build-essential, CMake, Ninja.
   Builds all libraries, examples, and tests in Release mode.
2. **Stage 2 (runtime)**: Minimal Ubuntu 24.04 with CMake + Python3.
   Copies the build artifacts and `tools/rdbdump` scripts.
   Default CMD runs `ctest --output-on-failure`.

## Kubernetes

Kubernetes manifests are not yet provided.  For K8s deployment, the
standard approach would be:

1. Build the Docker image and push to a registry.
2. Create a `Job` or `CronJob` manifest that runs the test suite
   as a post-deployment validation step.
3. For embedded targets, use a K8s node with attached SPI NOR flash
   hardware (e.g. via a USB-SPI adapter or QEMU emulation).

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
