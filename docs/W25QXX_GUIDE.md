# W25QXX Flash 集成指南

RocketDB 针对 W25QXX 系列 SPI NOR Flash 芯片的 HAL 集成完整指南。

---

## 1. W25QXX 特性概述

| 特性 | W25QXX | RocketDB 要求 | 兼容 |
|------|--------|---------------|------|
| 扇区大小 | 4KB（统一） | 可配置（≥4KB） | ✓ |
| 写入粒度 | 1 字节 | 1/2/4/8 字节 | ✓（wg=0） |
| NOR 1→0 语义 | 页编程支持 | 必须 | ✓ |
| 先擦后写 | 硬件强制 | 逻辑强制 | ✓ |
| 擦除时间 | 45ms typ（4KB 扇区） | yield 回调 | ✓ |
| 页编程 | 最大 256 字节，不能跨页 | HAL 需按页边界分段 | ✓ |
| 耐久度 | 100K 次 | 磨损均衡 | ✓ |

---

## 2. SPI 命令参考

```
命令              操作码    描述
──────────────────────────────────────────────
WREN              0x06      写使能（擦除/编程前必须发送）
WRDI              0x04      写禁止
RDSR1             0x05      读状态寄存器 1（检查 WIP/BUSY 位）
RDID              0x9F      JEDEC ID（识别芯片型号）
SECTOR_ERASE_4K   0x20      4KB 扇区擦除
BLOCK_ERASE_32K   0x52      32KB 块擦除
BLOCK_ERASE_64K   0xD8      64KB 块擦除
PAGE_PROGRAM      0x02      页编程（1-256 字节）
READ_DATA         0x03      读数据
CHIP_ERASE        0xC7/0x60 全片擦除
```

状态寄存器 1 位定义：

```
位  名称  描述
─────────────────
0   BUSY  1=忙（正在擦除/编程），0=就绪
1   WEL   1=写使能已锁存
2   BP0   块保护位 0
3   BP1   块保护位 1
4   BP2   块保护位 2
5   TB    顶部/底部保护
6   SEC   扇区保护
7   SRP0  状态寄存器保护 0
```

---

## 3. RocketDB 配置推荐

### W25Q32（4MB，1024×4KB 扇区）

```c
#define RDB_MIN_SECTOR_SIZE  4096u
#define RDB_MAX_KEY_LEN      63u
#define RDB_MAX_VAL_LEN      4095u
#define RDB_MAX_SECTORS      255u

/* KVDB: 32 扇区 × 4KB = 128KB（gc_reserve=4） */
/* TSDB: 32 扇区 × 4KB = 128KB                 */
```

### W25Q64（8MB，2048×4KB 扇区）

```c
#define RDB_MIN_SECTOR_SIZE  4096u
#define RDB_MAX_SECTORS      255u

/* 可按功能分区 */
/* 区域 0：KVDB — 64 扇区 × 4KB = 256KB（gc_reserve=8） */
/* 区域 1：TSDB — 64 扇区 × 4KB = 256KB                 */
```

### W25Q128（16MB，4096×4KB 扇区）

```c
#define RDB_MIN_SECTOR_SIZE  4096u
#define RDB_MAX_SECTORS      255u

/* 仅使用 4096 中的部分扇区 */
/* KVDB: 128 扇区 × 4KB = 512KB（gc_reserve=16） */
/* TSDB: 128 扇区 × 4KB = 512KB                 */
```

---

## 4. HAL 实现示例

### 4.1 SPI 写使能 + 忙等待

```c
static void w25q_write_enable(void) {
    uint8_t cmd = 0x06;
    spi_transfer(&cmd, NULL, 1);
}

static void w25q_wait_busy(void) {
    uint8_t cmd = 0x05, sr;
    do {
        spi_transfer(&cmd, &sr, 1);
    } while (sr & 0x01);
}
```

### 4.2 read() 实现

```c
static int flash_read(void *ctx, uint32_t addr, uint8_t *buf, size_t len) {
    (void)ctx;
    uint8_t cmd[4] = {
        0x03,
        (uint8_t)(addr >> 16),
        (uint8_t)(addr >> 8),
        (uint8_t)(addr)
    };
    spi_select();
    spi_transfer(cmd, NULL, 4);
    spi_transfer(NULL, buf, len);
    spi_deselect();
    return 0;
}
```

### 4.3 write() 实现

W25QXX 页编程不能跨越 256 字节页边界。`write()` 回调必须自行按页边界分段：
即使 RocketDB 典型写入较小，也不能假设调用地址天然落在页内安全范围。

```c
static int flash_write(void *ctx, uint32_t addr, const uint8_t *buf, size_t len) {
    (void)ctx;
    while (len > 0) {
        size_t page_rem = 256u - (addr & 0xFFu);
        size_t chunk = (len < page_rem) ? len : page_rem;

        w25q_write_enable();
        uint8_t cmd[4] = {
            0x02,
            (uint8_t)(addr >> 16),
            (uint8_t)(addr >> 8),
            (uint8_t)(addr)
        };
        spi_select();
        spi_transfer(cmd, NULL, 4);
        spi_transfer(buf, NULL, chunk);
        spi_deselect();
        w25q_wait_busy();

        addr += (uint32_t)chunk;
        buf += chunk;
        len -= chunk;
    }
    return 0;
}
```

### 4.4 erase() 实现

```c
static int flash_erase(void *ctx, uint32_t addr) {
    (void)ctx;
    w25q_write_enable();
    uint8_t cmd[4] = {
        0x20,  /* 4KB 扇区擦除 */
        (uint8_t)(addr >> 16),
        (uint8_t)(addr >> 8),
        (uint8_t)(addr)
    };
    spi_select();
    spi_transfer(cmd, NULL, 4);
    spi_deselect();
    w25q_wait_busy();
    return 0;
}
```

### 4.5 yield() 实现 — 看门狗喂狗

```c
static void flash_yield(void *ctx) {
    (void)ctx;
    /* 擦除操作耗时 ~45ms，期间喂狗防止复位 */
    HAL_IWDG_Refresh(&hiwdg);
}
```

### 4.6 lock/unlock — 多任务互斥

```c
static void flash_lock(void *ctx) {
    (void)ctx;
    /* RTOS: xSemaphoreTake(spi_mutex, portMAX_DELAY); */
    /* Bare-metal: __disable_irq();                     */
}

static void flash_unlock(void *ctx) {
    (void)ctx;
    /* RTOS: xSemaphoreGive(spi_mutex); */
    /* Bare-metal: __enable_irq();      */
}
```

### 4.7 完整 ops 结构体

```c
const rdb_flash_ops_t w25q_ops = {
    .read   = flash_read,
    .write  = flash_write,
    .erase  = flash_erase,
    .lock   = flash_lock,
    .unlock = flash_unlock,
    .yield  = flash_yield,
};
```

---

## 5. 分区示例

将 W25Q32 最后 64KB 分配给 RocketDB：

```c
#define W25Q32_TOTAL_SIZE    (4 * 1024 * 1024)  /* 4MB       */
#define W25Q32_SECTOR_SIZE   4096u               /* 4KB 扇区 */
#define ROCKETDB_OFFSET      (W25Q32_TOTAL_SIZE - 128 * 1024)
#define ROCKETDB_SIZE        (128 * 1024)        /* 128KB     */

static rdb_kv_sector_meta_t kv_meta[16]; /* 16 × 4KB = 64KB 分区 */
static uint32_t             ts_ec[16];

static rdb_partition_t kv_part = {
    .name        = "kvdb",
    .base_addr   = ROCKETDB_OFFSET,
    .total_size  = 64 * 1024,    /* 64KB      */
    .sector_size = 4096u,        /* 4KB 扇区  */
    .write_gran  = 0,            /* 1 字节    */
    .ops         = &w25q_ops,
    .flash_ctx   = NULL,
};

static rdb_partition_t ts_part = {
    .name        = "tsdb",
    .base_addr   = ROCKETDB_OFFSET + 64 * 1024,
    .total_size  = 64 * 1024,
    .sector_size = 4096u,
    .write_gran  = 0,
    .ops         = &w25q_ops,
    .flash_ctx   = NULL,
};

rdb_kvdb_t kvdb;
rdb_tsdb_t tsdb;

void rocketdb_w25q_init(void) {
    kvdb.part       = &kv_part;
    kvdb.sectors    = kv_meta;  /* rdb_kv_sector_meta_t[] */
    kvdb.sector_cnt = 16;       /* 16 × 4KB = 64KB, gc_reserve=2 */

    tsdb.part       = &ts_part;
    tsdb.erase_cnts = ts_ec;    /* uint32_t[] */
    tsdb.sector_cnt = 16;

    rdb_kvdb_format(&kvdb);
    rdb_kvdb_init(&kvdb, &kv_part, kv_meta);

    rdb_tsdb_format(&tsdb);
    rdb_tsdb_init(&tsdb, &ts_part, ts_ec);
}
```

---

## 6. 时序与性能

| 操作 | 典型时间 | 最大时间 | yield 策略 |
|------|----------|----------|------------|
| 页编程（256B） | 0.7ms | 3ms | 不需要 |
| 扇区擦除（4KB） | 45ms | 400ms | 每轮擦除 yield |
| 块擦除（32KB） | 120ms | 800ms | 每轮擦除 yield |
| 块擦除（64KB） | 150ms | 1000ms | 每轮擦除 yield |
| 读取（1B-∞） | ~40MHz SPI | — | 不需要 |

**RocketDB 实际负载**：
- KVDB set：小记录通常一次合并写入；大 value 以 `RDB_STACK_BUF_SIZE` 分块写入
- GC 迁移：批量读写，每扇区最多 1 次擦除
- TSDB append：小记录通常一次合并写入；大 data 以 `RDB_STACK_BUF_SIZE` 分块写入
- 扇区轮转：1 次擦除

HAL 仍需按 W25QXX 256B 页边界拆分，因为 64B 分块也可能从页尾附近开始。

---

## 7. 注意事项

### 7.1 电源保护

W25QXX 块保护位（BP0-BP2）可在擦除/编程期间保护关键扇区。
建议将 RocketDB 分区放在未保护区域，固件分区放在保护区域。

### 7.2 复位处理

如果 MCU 在 Flash 操作期间复位，W25QXX 会自动终止当前操作。
RocketDB 的两阶段提交协议通过检查 WRITING→VALID 状态转换
来处理这种部分写入场景，启动时可恢复一致性。

### 7.3 多任务 SPI 共享

如果 Flash 和 LCD/SD 卡共享 SPI 总线，必须在 `lock/unlock` 中
实现互斥锁：

```c
static SemaphoreHandle_t spi_mutex;

static void flash_lock(void) {
    xSemaphoreTake(spi_mutex, portMAX_DELAY);
}

static void flash_unlock(void) {
    xSemaphoreGive(spi_mutex);
}
```

### 7.4 写入粒度

W25QXX 支持单字节写入（页编程可在页内写入任意 ≤256 字节的数据）。
设置 `write_gran = 0`（1 字节粒度）。不要使用 `write_gran = 1`
（2 字节粒度），因为 W25QXX 无此对齐要求且会浪费空间。
