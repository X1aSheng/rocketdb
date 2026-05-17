# HAL_REFERENCE（STM32 HAL 集成指南）

本文档提供 RocketDB 在 STM32 微控制器上的 Flash HAL 集成详细指南。

---

## 概述

**Flash HAL 抽象层**用于屏蔽不同 MCU 的 Flash 驱动差异。RocketDB 提供标准接口：

```c
typedef struct {
    int       (*read)(uint32_t addr, uint8_t *buf, size_t len);
    int       (*write)(uint32_t addr, const uint8_t *buf, size_t len);
    int       (*erase)(uint32_t addr);
    void      (*lock)(void);
    void      (*unlock)(void);
    void      (*yield)(void);
} rdb_flash_ops_t;
```

| 函数 | 职责 | 返回值 |
|------|------|--------|
| `read()` | 读取 Flash 数据 | 0=成功，-1=失败 |
| `write()` | 写入 Flash 数据 | 0=成功，-1=失败 |
| `erase()` | 擦除包含地址的扇区 | 0=成功，-1=失败 |
| `lock()` | 禁用中断以保护 Flash 操作 | 无返回值 |
| `unlock()` | 启用中断 | 无返回值 |
| `yield()` | 喂看门狗或让出 CPU | 无返回值 |

---

## STM32F4 系列

### 硬件特性

| 参数 | STM32F407 | STM32F417 | 说明 |
|------|-----------|-----------|------|
| Flash 容量 | 512 KB / 1 MB | 512 KB / 1 MB | 多层级 |
| 扇区大小 | 16-256 KB | 16-256 KB | 参见分区表 |
| 写入粒度 | 程序字(32位) | 程序字(32位) | 必须 4 字节对齐 |
| 最大写速率 | ~10–15 字节/µs | ~10–15 字节/µs | 典型时钟 168 MHz |
| Erase 时间 | ~500 ms/扇区 | ~500 ms/扇区 | 依赖电压 |

### Flash 分区布局

**STM32F4 标准分区**（地址空间 0x0800 0000 开始）

```
Sector 0-3   : 16 KB × 4 = 64 KB    (0x0800 0000 - 0x0800 FFFF)
Sector 4     : 64 KB               (0x0801 0000 - 0x0801 FFFF)
Sector 5-7   : 128 KB × 3 = 384 KB (0x0802 0000 - 0x0805 FFFF)
Sector 8-10  : 256 KB × 3 = 768 KB (0x0806 0000 - 0x0809 FFFF)
...
Sector 11    : 256 KB             (0x080C 0000 - 0x080D FFFF)
```

**推荐分配**（1 MB Flash）

```
┌─────────────────────────────────────────┐
│ 0x0800 0000 - 0x0803 FFFF: Bootloader  │ 256 KB
│ (Sector 0-7)                            │
├─────────────────────────────────────────┤
│ 0x0804 0000 - 0x0805 FFFF: KVDB Config │ 128 KB
│ (Sector 8-9, 8×16KB)                    │
├─────────────────────────────────────────┤
│ 0x0806 0000 - 0x0807 FFFF: TSDB Logs   │ 128 KB
│ (Sector 10-11)                          │
├─────────────────────────────────────────┤
│ 0x0808 0000 - 0x080F FFFF: 空闲区    │ 512 KB
│ (Sector 12-15)                          │
└─────────────────────────────────────────┘
```

### STM32F4 HAL 实现

**头文件声明**

```c
// hal_rocketdb.h
#ifndef HAL_ROCKETDB_H
#define HAL_ROCKETDB_H

#include "stm32f4xx_hal.h"
#include "rocketdb.h"

// KVDB 分区定义
#define KVDB_START_ADDR   0x08040000    // Sector 8 开始
#define KVDB_SIZE         (128 * 1024)  // 128 KB
#define KVDB_SECTOR_SIZE  (16 * 1024)   // 16 KB

// TSDB 分区定义
#define TSDB_START_ADDR   0x08060000    // Sector 10 开始
#define TSDB_SIZE         (128 * 1024)  // 128 KB
#define TSDB_SECTOR_SIZE  (16 * 1024)   // 16 KB

extern rdb_kvdb_t g_kvdb;
extern rdb_tsdb_t g_tsdb;

int hal_rocketdb_init(void);

#endif // HAL_ROCKETDB_H
```

**实现文件**

```c
// hal_rocketdb.c
#include "hal_rocketdb.h"
#include <string.h>

// 全局数据库
rdb_kvdb_t g_kvdb;
rdb_tsdb_t g_tsdb;

// ============================================================================
// 低级 Flash 操作
// ============================================================================

/**
 * 将逻辑地址转换为物理 Flash 地址
 * @param logical_addr  相对分区的逻辑地址
 * @param partition     KVDB_START_ADDR 或 TSDB_START_ADDR
 * @return 绝对 Flash 物理地址
 */
static uint32_t hal_get_physical_addr(uint32_t logical_addr, uint32_t partition) {
    return partition + logical_addr;
}

/**
 * STM32F4 Flash 读操作
 * Flash 可直接读取，无需解锁
 */
static int hal_flash_read(uint32_t addr, uint8_t *buf, size_t len) {
    uint32_t physical_addr = hal_get_physical_addr(addr, KVDB_START_ADDR);
    
    // Flash 是可寻址的内存，直接 memcpy
    memcpy(buf, (const void*)physical_addr, len);
    return 0;
}

/**
 * STM32F4 Flash 写操作
 * 注意：STM32F4 必须以 32 位（4 字节）为单位编程
 * 地址和长度都必须 4 字节对齐
 */
static int hal_flash_write(uint32_t addr, const uint8_t *buf, size_t len) {
    uint32_t physical_addr = hal_get_physical_addr(addr, KVDB_START_ADDR);
    
    // 验证对齐（生产代码应正确处理非对齐情况）
    if ((physical_addr & 3) != 0 || (len & 3) != 0) {
        return -1;  // 对齐错误
    }

    HAL_FLASH_Unlock();

    // 以 32 位字为单位编程
    for (size_t i = 0; i < len; i += 4) {
        uint32_t word = *(uint32_t*)(buf + i);
        HAL_StatusTypeDef status = HAL_FLASH_Program(
            FLASH_TYPEPROGRAM_WORD,
            physical_addr + i,
            word
        );
        
        if (status != HAL_OK) {
            HAL_FLASH_Lock();
            return -1;
        }
    }

    HAL_FLASH_Lock();
    return 0;
}

/**
 * STM32F4 Flash 擦操作
 * 擦除包含给定地址的整个扇区
 */
static int hal_flash_erase(uint32_t addr) {
    uint32_t physical_addr = hal_get_physical_addr(addr, KVDB_START_ADDR);
    
    // 计算扇区号
    uint32_t sector;
    uint32_t offset = physical_addr - 0x08000000;
    
    if (offset < 16 * 1024) sector = 0;
    else if (offset < 32 * 1024) sector = 1;
    else if (offset < 48 * 1024) sector = 2;
    else if (offset < 64 * 1024) sector = 3;
    else if (offset < 128 * 1024) sector = 4;
    else if (offset < 256 * 1024) sector = 5;
    else if (offset < 384 * 1024) sector = 6;
    else if (offset < 512 * 1024) sector = 7;
    else if (offset < 768 * 1024) sector = 8;
    else if (offset < 1024 * 1024) sector = 9;
    else if (offset < 1280 * 1024) sector = 10;
    else sector = 11;  // 最后一个或更高的扇区

    // 初始化擦除参数
    FLASH_EraseInitTypeDef erase_init;
    erase_init.TypeErase = FLASH_TYPEERASE_SECTORS;
    erase_init.Sector = sector;
    erase_init.NbSectors = 1;
    erase_init.VoltageRange = FLASH_VOLTAGE_RANGE_3;  // 2.7-3.6 V

    uint32_t erase_error;

    HAL_FLASH_Unlock();
    HAL_StatusTypeDef status = HAL_FLASHEx_Erase(&erase_init, &erase_error);
    HAL_FLASH_Lock();

    if (status != HAL_OK) {
        // 可选：记录擦除错误 erase_error
        return -1;
    }

    return 0;
}

/**
 * 禁用中断以保护 Flash 操作
 */
static void hal_flash_lock(void) {
    __disable_irq();
}

/**
 * 启用中断
 */
static void hal_flash_unlock(void) {
    __enable_irq();
}

/**
 * 喂看门狗（防止在长时间操作中复位）
 */
static void hal_flash_yield(void) {
    IWDG_Feed();  // 假设使用独立看门狗
    
    // 可选：在这里也可以让出 CPU 给其他 RTOS 任务
    // extern void osThreadYield(void);
    // osThreadYield();
}

// ============================================================================
// RocketDB 初始化
// ============================================================================

int hal_rocketdb_init(void) {
    // 1. 配置 Flash 操作集
    static rdb_flash_ops_t flash_ops = {
        .read = hal_flash_read,
        .write = hal_flash_write,
        .erase = hal_flash_erase,
        .lock = hal_flash_lock,
        .unlock = hal_flash_unlock,
        .yield = hal_flash_yield
    };

    // 2. 配置 KVDB 分区
    static rdb_partition_t kvdb_partition = {
        .name = "KVDB",
        .base_addr = 0,               // 相对分区
        .total_size = KVDB_SIZE,
        .sector_size = KVDB_SECTOR_SIZE,
        .write_gran = 2,              // 4 字节粒度（STM32F4 要求）
        .ops = &flash_ops
    };

    // 3. 初始化 KVDB
    static rdb_kv_sector_meta_t kv_sectors[8];  // 8 个 16KB 扇区
    
    g_kvdb.part = &kvdb_partition;
    g_kvdb.sectors = kv_sectors;
    g_kvdb.sector_cnt = 8;

    int ret = rdb_kvdb_init(&g_kvdb, &kvdb_partition, kv_sectors);
    if (ret != RDB_OK) {
        // 未初始化可能是首次使用，尝试格式化
        rdb_kvdb_format(&g_kvdb);
    }

    return ret;
}

// ============================================================================
// 应用接口（可选的便利包装）
// ============================================================================

/**
 * 读取配置值
 */
int hal_config_get(const char *key, uint8_t *buf, uint16_t buf_cap, uint16_t *out_len) {
    return rdb_kvdb_get(&g_kvdb, key, buf, buf_cap, out_len);
}

/**
 * 写入配置值
 */
int hal_config_set(const char *key, const uint8_t *val, uint16_t val_len) {
    return rdb_kvdb_set(&g_kvdb, key, val, val_len);
}

/**
 * 删除配置值
 */
int hal_config_delete(const char *key) {
    return rdb_kvdb_delete(&g_kvdb, key);
}
```

### 在项目中集成

**1. 复制 HAL 文件到项目**

```
Project/
├── Core/
│   ├── Inc/
│   │   ├── main.h
│   │   ├── stm32f4xx_hal_conf.h
│   │   └── hal_rocketdb.h          ← 添加
│   └── Src/
│       ├── main.c
│       ├── stm32f4xx_it.c
│       └── hal_rocketdb.c          ← 添加
└── rocketdb/
    ├── rocketdb_kvdb.c
    ├── rocketdb_tsdb.c
    └── rocketdb.h
```

**2. 修改 main.c**

```c
#include "hal_rocketdb.h"

int main(void) {
    HAL_Init();
    SystemClock_Config();
    MX_GPIO_Init();
    // ... 其他初始化 ...
    
    // 初始化 RocketDB
    int ret = hal_rocketdb_init();
    if (ret != RDB_OK) {
        printf("[ERROR] RocketDB init failed: %d\n", ret);
        Error_Handler();
    }

    printf("[OK] RocketDB initialized\n");

    while (1) {
        // 应用代码
    }
}
```

**3. 编译选项（CubeMX 生成的 Makefile）**

```makefile
# 添加 RocketDB 源文件
SOURCES += \
    rocketdb/rocketdb_kvdb.c \
    rocketdb/rocketdb_tsdb.c \
    Core/Src/hal_rocketdb.c

# 添加包含路径
C_INCLUDES += \
    -Irocketdb \
    -ICore/Inc
```

---

## STM32L4 系列

### 特点与差异

| 特性 | STM32F4 | STM32L4 |
|------|---------|---------|
| Flash 写入粒度 | 32 位单字 | 64 位双字 |
| Erase 时间 | ~500 ms | ~100–200 ms（更快） |
| 耗电 | 中等 | 低功耗 |
| 典型频率 | 168 MHz | 80 MHz |

### HAL 差异

STM32L4 的 Flash 编程和擦除过程类似，但有以下差异：

```c
// STM32L4 Flash 写操作示例
static int hal_flash_write_l4(uint32_t addr, const uint8_t *buf, size_t len) {
    uint32_t physical_addr = hal_get_physical_addr(addr, KVDB_START_ADDR);
    
    // STM32L4 支持 64 位编程（双字）
    if ((physical_addr & 7) != 0 || (len & 7) != 0) {
        return -1;  // 必须 8 字节对齐
    }

    HAL_FLASH_Unlock();

    // 以 64 位双字为单位编程
    for (size_t i = 0; i < len; i += 8) {
        uint64_t dword = *(uint64_t*)(buf + i);
        HAL_StatusTypeDef status = HAL_FLASH_Program(
            FLASH_TYPEPROGRAM_DOUBLEWORD,
            physical_addr + i,
            dword
        );
        
        if (status != HAL_OK) {
            HAL_FLASH_Lock();
            return -1;
        }
    }

    HAL_FLASH_Lock();
    return 0;
}
```

**推荐参数调整**（STM32L4）

```c
rdb_partition_t kvdb_partition = {
    .name = "KVDB",
    .base_addr = 0,
    .total_size = (128 * 1024),
    .sector_size = (2 * 1024),      // L4 扇区更小
    .write_gran = 3,                // 8 字节粒度
    .ops = &flash_ops
};
```

---

## STM32G0/G4 系列（通用 MCU）

### Flash 特性

这些通用系列通常支持：
- **扇区大小**：512 B - 2 KB（可变）
- **写入粒度**：32 位（G0）、64 位（G4）
- **Erase 时间**：快速（~50 ms）

### 典型配置

```c
// STM32G034 示例（36 KB Flash）
static rdb_partition_t kvdb_partition_g0 = {
    .name = "KVDB",
    .base_addr = 0,
    .total_size = (16 * 1024),      // 16 KB 给 KVDB
    .sector_size = (2 * 1024),      // 2 KB 扇区
    .write_gran = 2,                // 4 字节
    .ops = &flash_ops
};
```

---

## 外部 Flash（SPI NOR）

如果 MCU 内部 Flash 不足，可使用外部 SPI NOR Flash（如 W25Q128）。

### HAL 实现框架

```c
#include "w25qxx.h"  // 第三方库

static int hal_spi_flash_read(uint32_t addr, uint8_t *buf, size_t len) {
    return W25qxx_ReadData(addr, buf, len) == W25Q_OK ? 0 : -1;
}

static int hal_spi_flash_write(uint32_t addr, const uint8_t *buf, size_t len) {
    // SPI NOR 页编程通常不能跨 256 字节页边界，HAL 必须分段。
    while (len > 0) {
        size_t page_rem = 256u - (addr & 0xFFu);
        size_t chunk = (len < page_rem) ? len : page_rem;
        if (W25qxx_WritePage(addr, buf, chunk) != W25Q_OK)
            return -1;
        addr += (uint32_t)chunk;
        buf += chunk;
        len -= chunk;
    }
    return 0;
}

static int hal_spi_flash_erase(uint32_t addr) {
    // 擦除扇区（通常 4 KB）
    uint32_t sector = addr / 4096;
    return W25qxx_EraseSector(sector) == W25Q_OK ? 0 : -1;
}

static void hal_spi_flash_lock(void) {
    // SPI 通常需要 GPIO 互斥（如 CS）
    HAL_GPIO_WritePin(SPI_CS_PORT, SPI_CS_PIN, GPIO_PIN_RESET);
}

static void hal_spi_flash_unlock(void) {
    HAL_GPIO_WritePin(SPI_CS_PORT, SPI_CS_PIN, GPIO_PIN_SET);
}

static void hal_spi_flash_yield(void) {
    // 喂看门狗
    HAL_IWDG_Refresh(&hiwdg);
}

// 配置
static rdb_flash_ops_t spi_flash_ops = {
    .read = hal_spi_flash_read,
    .write = hal_spi_flash_write,
    .erase = hal_spi_flash_erase,
    .lock = hal_spi_flash_lock,
    .unlock = hal_spi_flash_unlock,
    .yield = hal_spi_flash_yield
};
```

---

## 调试与诊断

### 常见问题与解决方案

| 问题 | 症状 | 原因 | 解决方案 |
|------|------|------|----------|
| Flash 写入失败 | HAL 返回错误，数据未写入 | 未擦除或地址错误 | 检查 erase 是否在 write 前调用 |
| CRC 校验失败 | 读取数据 CRC 不匹配 | Flash 损坏或硬件问题 | 运行 TROUBLESHOOTING.md 中的 CRC 诊断 |
| 掉电恢复失败 | 重启后数据丢失或损坏 | HAL write 未真正写入 | 添加调试日志验证写入 |
| 性能慢 | 操作耗时过长 | Yield 过度、看门狗频繁 | 减少 yield 频率或增加 MCU 频率 |

### 调试输出

```c
// 在 hal_rocketdb.c 中添加调试宏
#ifdef DEBUG_ROCKETDB
#define DBG_FLASH(fmt, ...) printf("[FLASH] " fmt "\n", ##__VA_ARGS__)
#else
#define DBG_FLASH(fmt, ...) do {} while(0)
#endif

static int hal_flash_write(uint32_t addr, const uint8_t *buf, size_t len) {
    DBG_FLASH("Writing %zu bytes at 0x%08x", len, addr);
    // ... 实现 ...
    DBG_FLASH("Write complete, verifying...");
    // 可选：验证写入
    uint8_t verify[32];
    if (len <= sizeof(verify)) {
        hal_flash_read(addr, verify, len);
        if (memcmp(verify, buf, len) == 0) {
            DBG_FLASH("✓ Verification passed");
        } else {
            DBG_FLASH("✗ Verification FAILED");
            return -1;
        }
    }
    return 0;
}
```

---

## 性能优化

### 减少 Erase 延迟

```c
// 在非关键路径中预先触发 erase
void background_erase_task(void) {
    if (g_kvdb.garbage_bytes > GARBAGE_THRESHOLD) {
        // 在空闲时运行 GC
        rdb_kvdb_gc(&g_kvdb);
    }
}
```

### 增加 Flash 写速率

```c
// 批量写（如果 HAL 支持）
// 避免频繁切换 Flash 驱动状态
uint8_t batch_buffer[256];
for (int i = 0; i < BATCH_SIZE; i++) {
    prepare_record_in_batch_buffer(i);
}
hal_flash_write(batch_offset, batch_buffer, sizeof(batch_buffer));
```

### 监视 Flash 健康

```c
void monitor_flash_health(void) {
    static uint32_t last_check = 0;
    
    if (HAL_GetTick() - last_check > 60000) {  // 每 60 秒检查一次
        last_check = HAL_GetTick();
        
        printf("[FLASH_HEALTH] ");
        printf("KV wear delta: %u, ", g_kvdb.max_wear - g_kvdb.min_wear);
        printf("GC runs: %u, ", g_kvdb.gc_count);
        printf("Garbage: %u bytes\n", g_kvdb.garbage_bytes);
    }
}
```

---

## 参考资源

- **RM0090**：STM32F4xx 参考手册（Flash 章节）
- **PM0214**：STM32L4xx 编程手册
- **AN2606**：STM32 Flash 编程指南
- [RocketDB API 文档](specify.md)
- [RocketDB 设计文档](design.md)

---

**最后更新**：2026年4月29日  
**版本**：RocketDB v1.1.2
