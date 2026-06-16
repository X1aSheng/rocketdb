# RocketDB

[![Version](https://img.shields.io/badge/version-1.2.0-blue)]()
[![C99](https://img.shields.io/badge/C-99-blue)]()
[![License](https://img.shields.io/badge/license-MIT-brightgreen.svg)](LICENSE)
[![Build](https://img.shields.io/badge/build-passing-brightgreen)]()

RocketDB is a zero-allocation, dual-mode Flash storage engine for resource-constrained embedded systems. It provides a log-structured key-value store (KVDB) and a ring-buffer time-series store (TSDB) on top of raw NOR Flash, with full power-loss consistency and static wear leveling.

### Table of Contents

  - [Instruction](#Instruction)
  - [Install](#Install)
  - [Usage](#Usage)
    - [KVDB basic](#kvdb-basic)
    - [TSDB basic](#tsdb-basic)
  - [Document](#Document)
  - [Contributing](#Contributing)
  - [License](#License)

### Instruction

`/src` includes RocketDB core engine source files.

`/interface` includes the hardware abstraction layer (HAL) template and header.

`/examples` includes RocketDB usage sample code (KVDB, TSDB, STM32F4).

`/tests` includes the simulation test framework, 8 host-side test suites,
performance benchmarks, and generated output under `tests/out`.

`/docs` includes design manual, API reference, integration guides, and troubleshooting.

`/build` includes Windows build/test scripts and benchmark runners.

`/tools/rdbdump` includes the offline Flash dump inspector/exporter for PC/server analysis.

`/zephyr` includes Zephyr OS port layer (flash adapter, Kconfig, module.yml).

### Install

#### Bare-metal / traditional RTOS

Reference `/interface` platform independent template and implement the Flash HAL for your target hardware:

1. Copy `interface/rocketdb_interface_template.c` into your project and fill in the function bodies.
2. Add `src/` to your include path and compile both `src/rocketdb_kvdb.c` and `src/rocketdb_tsdb.c`.
3. If you only need one engine, you may exclude the other `.c` file.

Required external primitives (implemented in the interface template):
- `rdb_crc16()` / `rdb_crc16_cont()` — CRC-16/MODBUS
- `rdb_hash16()` — 16-bit FNV-1a folded key hash
- `rdb_flash_ops_t` — Flash read / write / erase callbacks (with `void *ctx`)

The template also exports `rocketdb_interface_ops`, which can be assigned to
`rdb_partition_t.ops` after the hardware-specific flash functions are filled in.

#### Zephyr OS

Add RocketDB as a Zephyr module (see `zephyr/module.yml`):

1. Configure your west manifest to include this repository.
2. Enable `CONFIG_ROCKETDB=y` in your Kconfig.
3. Use `rocketdb_partition_init()` from `zephyr/rocketdb_port.h` to wire a
   Zephyr flash device to a RocketDB partition descriptor.

All external primitives (`rdb_crc16`, `rdb_hash16`, flash ops) are provided
by `zephyr/rocketdb_port.c`.  No additional HAL code is needed.

#### Docker

A multi-stage Dockerfile is provided for Linux build and test:

```bash
docker build -t rocketdb:latest .
docker run --rm rocketdb:latest
docker-compose up
```

### Build And Test

Windows host builds use LLVM/Clang by default and fall back to GCC:

- Default compiler: `D:\Programs\LLVM\bin\clang.exe`
- Fallback compiler: `D:\Programs\w64devkit\bin\gcc.exe`

All controllable build/test/performance/offline-analysis artifacts are written
to `tests/out`.

```bat
build\build.bat all test
build\build.bat kvdb test
build\build.bat tsdb test
build\build.bat perf run
```

```bash
cmake -S . -B cmake-build -DBUILD_TESTS=ON -DBUILD_PERF=ON
cmake --build cmake-build --config Debug
ctest --test-dir cmake-build --output-on-failure
```

Additional CMake options:

| Option | Default | Description |
|--------|---------|-------------|
| `-DENABLE_STRICT_WARNINGS=ON` | ON | Enable `-Wpedantic -Wshadow -Wconversion` |
| `-DENABLE_SANITIZER=ON` | OFF | Enable AddressSanitizer + UndefinedBehaviorSanitizer |
| `-DENABLE_DEBUG_LOGGING=ON` | OFF | Enable runtime debug trace output |
| `-DBUILD_PERF=ON` | OFF | Build performance benchmark |

CMake presets are also available for common configurations:

```bash
cmake --preset debug          # Debug build with tests
cmake --preset release        # Release build with tests
cmake --preset sanitize       # Debug build with sanitizers
cmake --preset minimal        # Release build without tests
```

```bash
# Or use the Makefile on Unix-like systems:
make test

### Usage

Reference the examples in `/examples` to complete your own driver.

#### KVDB basic

```c
#include "rocketdb.h"

/* User-provided flash ops (see interface template) */
static rdb_flash_ops_t flash_ops = { ... };

rdb_partition_t part = {
    .name        = "KVDB",
    .base_addr   = 0x00000000,
    .total_size  = 64 * 1024,
    .sector_size = 4 * 1024,
    .ops         = &flash_ops,
    .flash_ctx   = NULL,    /* bare-metal: no context */
};

rdb_kvdb_t db;
rdb_kv_sector_meta_t sectors[16];
rdb_kvdb_init(&db, &part, sectors);

rdb_kvdb_set(&db, "wifi_ssid", (const uint8_t*)"MyWiFi", 6);
rdb_kvdb_set(&db, "dev_id",   (const uint8_t*)"ABC123", 6);

uint8_t  val[32];
uint16_t vlen = 0;
rdb_kvdb_get(&db, "wifi_ssid", val, sizeof(val), &vlen);
```

#### TSDB basic

```c
rdb_tsdb_t ts;
uint32_t ec_buf[8];
rdb_tsdb_init(&ts, &part, ec_buf);

rdb_tsdb_append(&ts, 1000, data, 16);  /* timestamp = 1000 */
rdb_tsdb_append(&ts, 1001, data, 16);

/* Query range: timestamps 500 .. 2000 */
rdb_tsdb_query(&ts, 500, 2000, callback, user_data);
```

#### Flash write granularity

KVDB supports `write_gran = 0..3` (1/2/4/8-byte program alignment) and
pads large values in aligned chunks. TSDB currently supports `write_gran = 0`
and `1`; `rdb_tsdb_init()` and `rdb_tsdb_format()` reject `write_gran > 1`
because the current sector seal protocol commits 2-byte fields.

KVDB key length is architecturally limited to 32 bytes. Keep keys short,
stable, and enumerable; map longer application paths or JSON-style names to
short keys before storing them.

W25QXX-class SPI NOR parts should normally use `write_gran = 0`. The HAL
`write()` callback must still split page-program commands at 256-byte page
boundaries and preserve NOR 1-to-0 programming semantics.

### Document

Online API reference: [rocketdb.h](src/rocketdb.h) (Doxygen-formatted comments throughout).

Offline documents:

- [`docs/architecture/ARCHITECTURE.md`](docs/architecture/ARCHITECTURE.md) — Full architecture and design manual
- [`docs/README.md`](docs/README.md) — Documentation index
- [`docs/architecture/W25QXX_GUIDE.md`](docs/architecture/W25QXX_GUIDE.md) — W25QXX SPI NOR Flash integration guide
- [`docs/architecture/TEST_PLAN.md`](docs/architecture/TEST_PLAN.md) — Test plan and coverage assessment
- [`docs/architecture/OFFLINE_ANALYSIS.md`](docs/architecture/OFFLINE_ANALYSIS.md) — Offline raw Flash dump analysis with `tools/rdbdump`
- [`tests/sim/README.md`](tests/sim/README.md) — Simulation test methodology
- [`tests/perf/README.md`](tests/perf/README.md) — Performance benchmark methodology

After `build\run_all_tests.bat test`, simulator Flash images are verified by `rdbdump` and exported under `tests/out/rdbdump_export/<YYMMDD-HHMMSS>/`.

Generate HTML API docs with Doxygen:

```bash
doxygen Doxyfile
```

### Contributing

See [`CONTRIBUTING.md`](CONTRIBUTING.md) for contribution guidelines and coding standards.

### License

RocketDB is licensed under the MIT License. See [`LICENSE`](LICENSE) for the full text.

Copyright (c) 2015 XiaSheng(info@zhis.net)
