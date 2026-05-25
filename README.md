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

`/test` includes the simulation test framework and comprehensive test suites
(8 batch suites plus CTest examples, 40,000+ assertions depending on build
configuration).

`/docs` includes design manual, API reference, integration guides, and troubleshooting.

`/build` includes Windows build/test scripts and benchmark runners.

`/zephyr` includes Zephyr OS port layer (flash adapter, Kconfig, module.yml).

### Install

#### Bare-metal / traditional RTOS

Reference `/interface` platform independent template and implement the Flash HAL for your target hardware:

1. Copy `interface/rocketdb_interface_template.c` into your project and fill in the function bodies.
2. Add `src/` to your include path and compile both `src/rocketdb_kvdb.c` and `src/rocketdb_tsdb.c`.
3. If you only need one engine, you may exclude the other `.c` file.

Required external primitives (implemented in the interface template):
- `rdb_crc16()` / `rdb_crc16_cont()` — CRC-16/MODBUS
- `rdb_hash16()` — 16-bit key hash (DJB2 or similar)
- `rdb_flash_ops_t` — Flash read / write / erase callbacks (with `void *ctx`)

#### Zephyr OS

Add RocketDB as a Zephyr module (see `zephyr/module.yml`):

1. Configure your west manifest to include this repository.
2. Enable `CONFIG_ROCKETDB=y` in your Kconfig.
3. Use `rocketdb_partition_init()` from `zephyr/rocketdb_port.h` to wire a
   Zephyr flash device to a RocketDB partition descriptor.

All external primitives (`rdb_crc16`, `rdb_hash16`, flash ops) are provided
by `zephyr/rocketdb_port.c`.  No additional HAL code is needed.

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

- [`docs/Architecture.md`](docs/Architecture.md) — Full architecture and design manual
- [`docs/EXAMPLES.md`](docs/EXAMPLES.md) — 8 complete code examples
- [`docs/HAL_REFERENCE.md`](docs/HAL_REFERENCE.md) — STM32 MCU integration guide
- [`docs/W25QXX_GUIDE.md`](docs/W25QXX_GUIDE.md) — W25QXX SPI NOR Flash integration guide
- [`docs/TROUBLESHOOTING.md`](docs/TROUBLESHOOTING.md) — Common problems and solutions
- [`docs/test_plan.md`](docs/test_plan.md) — Test plan and coverage assessment
- [`docs/CODE_REVIEW_CONSOLIDATED_260517.md`](docs/CODE_REVIEW_CONSOLIDATED_260517.md) — Consolidated legacy code-review status

Generate HTML API docs with Doxygen:

```bash
doxygen Doxyfile
```

### Contributing

See [`CONTRIBUTING.md`](CONTRIBUTING.md) for contribution guidelines and coding standards.

### License

RocketDB is licensed under the MIT License. See [`LICENSE`](LICENSE) for the full text.

Copyright (c) 2015 XiaSheng(info@zhis.net)
