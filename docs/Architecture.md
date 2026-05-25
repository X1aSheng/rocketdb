# RocketDB 驱动设计手册

---
## 文档控制

| 项目 | 内容 |
|------|------|
| 版本 | 1.3.0 |
| 状态 | 生产级基线 |
| 适用硬件 | SPI NOR Flash（W25Q系列及兼容型号） |
| 适用 MCU | Cortex-M0 / M3 / M4 / M7（已针对非对齐访问做结构体自然对齐） |
| 分区约束 | 单分区 ≤ 1MB，扇区 4KB |
| 可移植 RTOS | Zephyr OS（zephyr/rocketdb_port.c 适配层） |

---

## 第一章 架构

### 1.1 定位

RocketDB 是面向资源受限嵌入式系统的双模 Flash 存储引擎：

- **KVDB**：日志结构键值存储，少量 key 频繁更新，自动垃圾回收，具备静态磨损均衡能力
- **TSDB**：环形时序存储，追加写入、时间范围查询、自动淘汰，原生支持防时间戳回绕（Epoch 机制）

### 1.2 核心约束

| 约束 | 说明 |
|------|------|
| 零动态内存 | 全部缓冲区由调用者提供或栈上分配，杜绝内存碎片 |
| NOR Flash 安全 | 全部写操作严格遵循 1→0 位翻转，无原地覆盖 |
| 掉电一致性 | 任意时刻断电（含 GC 途中），下次初始化恢复到一致状态 |
| 确定性 GC | 迁移前完成可行性校验，不会出现中途无处可写 |
| 数据安全优先 | 写操作采用"先写新再删旧"策略，任何路径不以丢失已有数据为代价 |
| 结构体自然对齐 | 所有 On-Flash 结构体关键字段按 4 字节自然对齐，避免 Cortex-M0 HardFault |
| 实时性保障 | 提供 yield 回调，GC/rotation 期间主动让出 CPU 和喂狗 |

### 1.2.1 设计规则（内建约束）

- **确定性布局**：所有 On-Flash 结构体 `#pragma pack(push, 1)`，并将所有 `uint32_t` 字段放在 4 字节对齐偏移，填充字节保持 0xFF。
- **状态机一致性**：记录状态仅允许 1→0 位翻转，WRITING→VALID→DEAD，避免任何原地回写。
- **扫描只读规则**：查询/迭代路径只读不修复 WRITING 记录，不更新 write_off；仅 init 阶段允许修复与写偏移恢复。
- **损坏跳过策略**：遇到非法 record header 时按记录头大小（16B/12B）跳步，避免逐字节扫描导致误对齐。
- **空间可行性先行**：写入前计算最小空间需求，结合 `will_free` 估算，确保 `gc_reserve + 1` 安全水位。
- **安全封存协议**：TSDB 扇区封存按 count→end_off→hdr_crc 三步提交，CRC 作为最终提交点。
- **大记录读取策略**：超过 `RDB_STACK_BUF_SIZE` 的记录通过 `query_ex` 提供外部缓冲，默认回调返回 data=NULL。
- **环形一致性**：head/tail 更新遵循单调序列与回绕检测，覆盖时明确推进 tail 以保证 ring 可用。

### 1.2.2 代码审核沉淀规则

历次代码审核中的关键问题已被归纳为以下长期设计规则。后续实现、移植和测试均应以这些规则为门禁，而不是仅依赖历史审查报告。

| 主题 | 设计规则 | 来源问题 |
|------|----------|----------|
| 位宽与容量 | 所有扇区内偏移使用 `uint32_t`，RAM 中 TSDB `head_seq` 使用 `uint32_t`；On-Flash 字段宽度不变时必须在扫描/写入边界做范围校验 | 64KB+ 扇区 offset 截断、TSDB 65536 次 rotation 后序列回绕 |
| 序列号哨兵 | `RDB_SEQ_INVALID = 0xFFFFFFFFu`，正常 `write_seq` 可从 0 开始并允许 uint32 wrap-safe 比较 | write_seq 回绕到 0 与 invalid 冲突 |
| 提交失败处理 | 任何 record commit/state 写失败后，都必须推进写前沿或显式标记 DEAD，禁止后续写入复用同一物理地址 | KVDB set/migrate commit 失败后违反 NOR 1→0 约束 |
| GC 原子性 | victim 扇区只有在所有 live 记录迁移成功后才允许擦除；迁移失败时保留 victim，以 init/fixup 自愈 | GC 迁移中断潜在数据丢失 |
| 返回值传播 | Flash read/write/erase、seal、mark_dead、padding 写入等关键路径返回值必须检查并反映到错误码或统计计数 | 写失败/封存失败被静默忽略 |
| 查询无副作用 | get/query/iter/latest/oldest/time_range 不修复 Flash、不提交 WRITING、不推进写偏移；恢复只发生在 init 或显式写路径 | TSDB 查询触发 Flash 写入和额外磨损 |
| 锁语义 | 公共 API 内部围绕 Flash 与共享统计/元数据加锁；`iter_gen` 等代数计数必须在锁内更新 | stats/space/wear 并发读取竞态、迭代器过期窗口 |
| 零动态内存 | 引擎本体不使用堆；大临时状态优先复用 caller buffer，避免静态全局缓冲和大栈数组 | format 静态缓冲、GC 候选栈数组、TSDB 1KB 栈数组 |
| 磨损计数 | 擦除计数使用 RAM/Flash 双源取 max 后递增；format/init_sec 不得让 erase_cnt 回退 | 掉电或坏头导致 erase_cnt 丢失，磨损均衡失真 |
| CRC 边界 | On-Flash 头部 CRC 覆盖范围必须由 `_Static_assert(offsetof(...))` 固化；CRC 错误可见但不应破坏其它数据 | 硬编码 CRC 字节数漂移、头部字段未受保护 |
| 写入粒度 | 模拟器和 HAL 均必须强制写入粒度对齐；TSDB 当前仅验证 wg=0/1，若支持 wg≥2 需要重新审视 20B 扇区头和提交字节写入协议 | 单字节写绕过对齐、TSDB wg≥2 结构不兼容 |
| 测试有效性 | 压力目标必须与文档一致（GC/rotation ≥100）；随机测试记录 seed；故障注入覆盖 read/write/erase/power-loss/corrupt/bit-flip | 压力目标降级、seed 不可追溯、故障类型未覆盖 |

### 1.3 典型使用模式

**KVDB**：存储少量配置项（10\~1000 个 key），单个 key 被频繁读写更新。每次更新产生一条新记录和一条旧记录（垃圾）。系统稳定运行的核心是 GC 能持续回收旧记录占用的空间，并通过静态均衡机制（Phase 4）将长期不更新的冷扇区拉回擦写循环，保障全盘寿命均衡。

**TSDB**：存储传感器采样、告警日志等时序数据。数据只追加不修改，环形覆盖最旧数据。典型负载为固定大小的小记录（8\~128 字节），高级场景下支持写入接近单扇区容量的大记录（如波形快照），但需评估磨损与容量代价。

### 1.4 模块分层

```
┌──────────────────────────────────────────────┐
│              Application Layer               │
├────────────────┬─────────────────────────────┤
│   KVDB API     │        TSDB API             │
├────────────────┼─────────────────────────────┤
│  KVDB Engine   │      TSDB Engine            │
│  ┌──────────┐  │   ┌───────────┐             │
│  │ GC       │  │   │ Ring      │             │
│  │ (Scored  │  │   │ Rotation  │             │
│  │  +Wear)  │  │   │ + Epoch   │             │
│  └──────────┘  │   └───────────┘             │
├────────────────┴─────────────────────────────┤
│         Flash Abstraction Layer              │
│     read / write / erase / lock / yield      │
├──────────────────────────────────────────────┤
│      External Primitives（用户实现）          │
│        CRC16 / Hash16                        │
└──────────────────────────────────────────────┘
```

### 1.5 文件结构

```
rocketdb.h          公共类型、常量、API 声明
rocketdb_kvdb.c     KVDB 引擎实现
rocketdb_tsdb.c     TSDB 引擎实现

zephyr/
  rocketdb_port.c   Zephyr OS 适配层（flash ops + CRC + hash）
  rocketdb_port.h   Zephyr 适配层公共头文件
  Kconfig           Zephyr 构建配置项
  CMakeLists.txt    Zephyr 模块构建脚本
  module.yml        Zephyr 模块清单
```

---

## 第二章 公共定义

### 2.1 编译时配置

```c
#define RDB_MAX_KEY_LEN         32u
#define RDB_MAX_VAL_LEN         4095u
#define RDB_MAX_TS_DATA_LEN     0u       /* 0=由扇区决定；>0 为软限制 */
#define RDB_KV_CACHE_SIZE       0u       /* KVDB 键地址缓存槽位数 */
#define RDB_STACK_BUF_SIZE      64u
#define RDB_GC_GARBAGE_PCT      20u
#define RDB_GC_WEAR_THRESHOLD   100u
#define RDB_MIN_SECTOR_SIZE     4096u
#define RDB_KV_MIN_SECTORS      3u
#define RDB_TS_MIN_SECTORS      2u
#define RDB_MAX_SECTORS         255u
```

**`RDB_MAX_TS_DATA_LEN` 语义：**

| 配置值 | 行为 |
|--------|------|
| 0（默认） | `max_data_len` = 扇区物理上限 |
| 1\~65535 | `max_data_len` = min(物理上限, 配置值) |

建议显式配置，以便编译时确定 query 缓冲区大小。

**`RDB_KV_CACHE_SIZE` 语义：**

| 配置值 | 行为 |
|--------|------|
| 0（默认） | 禁用缓存，无 RAM 开销 |
| 1\~255 | 启用缓存，每个槽 16 字节（2 字节 hash + 1 字节 klen + 8 字节前缀 + 4 字节地址 + 1 字节填充） |

缓存采用直接映射 + 线性探测，消除热点 key 的全表扫描。推荐嵌入式工作负载配置 64 槽（1024 字节 RAM）。
键指纹 = FNV-1a 16-bit hash + key 长度 + key 前 8 字节，冲突时回退全表扫描并通过 memcmp 校验完整 key。

```c
_Static_assert(RDB_MAX_KEY_LEN >= 1u && RDB_MAX_KEY_LEN <= 32u,
               "RDB_MAX_KEY_LEN must be 1..32");
```

KVDB key 是嵌入式配置项标识符，不是文件路径或 JSON 字段路径。架构上限制
`RDB_MAX_KEY_LEN <= 32`，应用层若需要更长的层级名称，应映射为短 key 或枚举 ID
后再写入 KVDB。该约束用于稳定 Flash 占用、扫描成本、GC 迁移成本和栈缓冲边界。

### 2.2 错误码

```c
typedef enum {
    RDB_OK                 =   0,
    RDB_ERR_PARAM          =  -1,
    RDB_ERR_FLASH          =  -2,
    RDB_ERR_NO_SPACE       =  -3,
    RDB_ERR_NOT_FOUND      =  -4,
    RDB_ERR_TOO_LARGE      =  -5,
    RDB_ERR_CRC            =  -6,
    RDB_ERR_CORRUPT        =  -7,
    RDB_ERR_NOT_INIT       =  -8,
    RDB_ERR_GC_FAIL        =  -9,
    RDB_ERR_ITER_END       = -10,
    RDB_ERR_FULL           = -11,
    RDB_ERR_BUSY           = -12,
    RDB_ERR_TIME_EXHAUSTED = -13
} rdb_err_t;
```

### 2.3 记录状态机

```
NOR 安全（每步仅 1→0）：

  0xFF (WRITING) ──commit──► 0xFE (VALID) ──invalidate──► 0xFC (DEAD)

  新记录 state 保持 0xFF（无需额外写入）
  提交写 0xFE（bit0 清零）
  作废写 0xFC（bit1 清零）
  其他值视为 DEAD（损坏防御兜底）
```

```c
#define RDB_STATE_WRITING   0xFFu
#define RDB_STATE_VALID     0xFEu
#define RDB_STATE_DEAD      0xFCu
```

### 2.4 哨兵值与宏

```c
#define RDB_ADDR_INVALID    0xFFFFFFFFu
#define RDB_TIME_INVALID    0xFFFFFFFFu
#define RDB_TIME_MAX        0xFFFFFFFEu
#define RDB_SEQ_INVALID     0xFFFFFFFFu

#define RDB_ALIGN_UP(v, a)  (((uint32_t)(v) + (uint32_t)(a) - 1u) \
                              & ~((uint32_t)(a) - 1u))
#define RDB_MIN(a, b)       ((a) < (b) ? (a) : (b))
#define RDB_MAX(a, b)       ((a) > (b) ? (a) : (b))

#define RDB_GC_RESERVE(n)   ((uint8_t)(((n) >= 16u) ? 2u : 1u))
#define RDB_SEQ_GT(a, b)    ((int32_t)((a) - (b)) > 0)
#define RDB_SEQ16_GT(a, b)  ((int16_t)((uint16_t)(a) - (uint16_t)(b)) > 0)

#define RDB_TRUE    1
#define RDB_FALSE   0
#define RDB_ITER_CONTINUE   0
#define RDB_ITER_STOP       1
```

### 2.5 Flash 抽象层

```c
typedef struct {
    int  (*read)  (void *ctx, uint32_t addr, uint8_t *buf, size_t len);
    int  (*write) (void *ctx, uint32_t addr, const uint8_t *buf, size_t len);
    int  (*erase) (void *ctx, uint32_t addr);
    void (*lock)  (void *ctx);
    void (*unlock)(void *ctx);
    void (*yield) (void *ctx);
} rdb_flash_ops_t;
```

| 回调 | 返回值 | 说明 |
|------|--------|------|
| read | 0/非0 | 读取；ctx 为 rdb_partition_t.flash_ctx |
| write | 0/非0 | 写入；调用者保证不跨页 |
| erase | 0/非0 | 擦除整个扇区 |
| lock | 无 | 可选，NULL 跳过 |
| unlock | 无 | 可选，NULL 跳过 |
| yield | 无 | 可选，耗时操作期间喂狗/让出线程；NULL 跳过；内部禁止调用 RocketDB API |

每个回调的第一个参数 `void *ctx` 由 `rdb_partition_t.flash_ctx` 传入。
这使得同一张 ops 函数表可以被多个 flash 设备/分区共享，ctx 携带
每实例状态（如 Zephyr flash 设备指针 + 基地址偏移量）。
裸机单实例场景设置 flash_ctx = NULL 即向后兼容。

```c
typedef struct {
    const char            *name;
    uint32_t               base_addr;
    uint32_t               total_size;
    uint32_t               sector_size;
    uint8_t                write_gran;  /* 0→1B 1→2B 2→4B 3→8B */
    const rdb_flash_ops_t *ops;
    void                  *flash_ctx;   /* 传入所有 ops 回调的透明指针 */
} rdb_partition_t;
```

**约束（init 时校验）：**

- `sector_size ≥ 4096` 且为 2 的幂
- `total_size` 为 `sector_size` 整数倍
- KVDB：扇区数 ∈ \[3, 255\]；TSDB：扇区数 ∈ \[2, 255\]
- KVDB `write_gran` ∈ {0, 1, 2, 3}
- TSDB 当前 `write_gran` ∈ {0, 1}；`write_gran > 1` 在 init/format 阶段返回 `RDB_ERR_PARAM`

### 2.6 用户实现的外部函数

```c
extern uint16_t rdb_crc16     (const void *data, size_t len);
extern uint16_t rdb_crc16_cont(uint16_t crc, const void *data, size_t len);
extern uint16_t rdb_hash16    (const void *data, size_t len);
```

---

## 第三章 On-Flash 数据结构

> **对齐设计说明**：全部 On-Flash 结构体的 32 位字段精确对齐到 4 字节边界。`rdb_kv_record_hdr_t.seq` 位于偏移 8，`rdb_ts_record_hdr_t.time_delta` 位于偏移 4，均为 4 字节自然对齐。在 Cortex-M0 等不支持非对齐访问的 MCU 上，可直接将 flash 读取的缓冲区强制转换为结构体指针而不触发 HardFault。

### 3.1 KVDB 扇区头（16 字节）

```c
#pragma pack(push, 1)
typedef struct {
    uint32_t magic;         /* offset 0  — 0x4B564442 ("KVDB")  */
    uint16_t version;       /* offset 4  — 0x0001               */
    uint16_t hdr_crc;       /* offset 6  — CRC16(bytes[0..5])   */
    uint32_t erase_cnt;     /* offset 8  — 4B 对齐 ✓            */
    uint32_t create_seq;    /* offset 12 — 4B 对齐 ✓            */
} rdb_kv_sector_hdr_t;     /* 16 bytes                         */
#pragma pack(pop)
```

### 3.2 KVDB 记录头（16 字节）

```c
#pragma pack(push, 1)
typedef struct {
    uint8_t  magic;         /* offset 0                         */
    uint8_t  state;         /* offset 1                         */
    uint8_t  key_len;       /* offset 2                         */
    uint8_t  _pad0;         /* offset 3  — 0xFF                 */
    uint16_t val_len;       /* offset 4                         */
    uint16_t key_hash;      /* offset 6                         */
    uint32_t seq;           /* offset 8  — 4B 对齐 ✓            */
    uint16_t data_crc;      /* offset 12                        */
    uint16_t _pad1;         /* offset 14 — 0xFFFF               */
} rdb_kv_record_hdr_t;     /* 16 bytes                         */
#pragma pack(pop)
```

**Flash 布局：**

```
│ record_hdr (16B) │ key (aligned) │ val (aligned) │

total = 16 + ALIGN_UP(key_len, WR_GRAN) + ALIGN_UP(val_len, WR_GRAN)
对齐填充 = 0xFF
data_crc = CRC16(原始 key ∥ 原始 val)
```

### 3.3 TSDB 扇区头（20 字节）

```c
#pragma pack(push, 1)
typedef struct {
    uint32_t magic;         /* offset 0  — 0x54534442 ("TSDB")  */
    uint32_t erase_cnt;     /* offset 4  — 4B 对齐 ✓            */
    uint32_t time_base;     /* offset 8  — 4B 对齐 ✓            */
    uint16_t seq;           /* offset 12                        */
    uint16_t count;         /* offset 14 — 0xFFFF=未封存        */
    uint16_t end_off;       /* offset 16 — 0xFFFF=未封存        */
    uint16_t hdr_crc;       /* offset 18                        */
} rdb_ts_sector_hdr_t;     /* 20 bytes                         */
#pragma pack(pop)
```

**分阶段写入：**

| 阶段 | 字段 | 时机 |
|------|------|------|
| 创建 | magic, erase_cnt, seq | `ts_init_sec` |
| 首记录 | time_base | 第一次 `append` |
| 封存 | count → end_off → hdr_crc | `ts_seal` |

### 3.4 TSDB 记录头（12 字节）

```c
#pragma pack(push, 1)
typedef struct {
    uint8_t  magic;         /* offset 0                         */
    uint8_t  state;         /* offset 1                         */
    uint16_t data_len;      /* offset 2                         */
    uint32_t time_delta;    /* offset 4  — 4B 对齐 ✓            */
    uint16_t data_crc;      /* offset 8                         */
    uint16_t _pad;          /* offset 10 — 0xFFFF               */
} rdb_ts_record_hdr_t;     /* 12 bytes                         */
#pragma pack(pop)
```

**time_delta 使用 `uint32_t`**：同一扇区内时间跨度上限 `0xFFFFFFFE`（秒精度约 136 年，毫秒精度约 49.7 天）。rotation 几乎仅由扇区空间写满驱动，从根本上消除因时间差过大引发的非预期 rotation。

### 3.5 编译时断言

```c
_Static_assert(sizeof(rdb_kv_sector_hdr_t) == 16u, "");
_Static_assert(sizeof(rdb_kv_record_hdr_t) == 16u, "");
_Static_assert(sizeof(rdb_ts_sector_hdr_t) == 20u, "");
_Static_assert(sizeof(rdb_ts_record_hdr_t) == 12u, "");
```

### 3.6 扇区空间布局

```
KVDB (4096B, write_gran=0):
┌──────────────────┬──────────────────────────────────────────┐
│ sector_hdr (16B) │ record₁ │ record₂ │ ...  │ 0xFF...      │
│                  │◄──────── DATA_CAP = 4080B ─────────────►│
└──────────────────┴──────────────────────────────────────────┘

TSDB (4096B, write_gran=0):
┌──────────────────┬──────────────────────────────────────────┐
│ sector_hdr (20B) │ record₁ │ record₂ │ ...  │ 0xFF...      │
│                  │◄──────── DATA_CAP = 4076B ─────────────►│
└──────────────────┴──────────────────────────────────────────┘
```

---

## 第四章 KVDB 引擎设计

### 4.1 设计前提

KVDB 面向**少量 key 频繁更新**：

```
典型工况：
  key 数量：10~50
  value 大小：4~256 字节
  操作模式：读多写少，但写入持续

20 key, avg rec_sz=52B, 8 扇区:
  存活数据 = 1040B（MAX_LIVE 的 3.6%）
  单扇区更新容量 ≈ 78 次
  约 546 次更新后首次 GC
  稳态 GC = 直接擦除 zero-live 扇区，开销最小
```

### 4.2 RAM 数据结构

```c
typedef enum {
    RDB_SEC_ERASED  = 0,
    RDB_SEC_ACTIVE  = 1,
    RDB_SEC_SEALED  = 2,
    RDB_SEC_CORRUPT = 3
} rdb_sec_status_t;

typedef struct {
    uint32_t create_seq;
    uint32_t erase_cnt;
    uint32_t garbage_bytes;
    uint32_t write_off;
    uint8_t  status;
    uint8_t  _pad[3];
} rdb_kv_sector_meta_t;        /* 16 bytes */

typedef struct {
    uint32_t read_ops;
    uint32_t write_ops;
    uint32_t delete_ops;
    uint32_t gc_runs;
    uint32_t gc_reclaimed_bytes;
    uint32_t gc_migrated_recs;
    uint32_t flash_errors;
    uint32_t crc_errors;
    uint32_t corrupt_sectors;
} rdb_kv_stats_t;

typedef struct {
    const rdb_partition_t    *part;
    rdb_kv_sector_meta_t     *sectors;
    uint8_t   sector_cnt;
    uint8_t   gc_reserve;
    uint8_t   active_sec;
    uint8_t   initialized;
    uint32_t  write_seq;
    uint32_t  live_bytes;
    uint32_t  write_off;
    uint32_t  iter_gen;
    rdb_kv_stats_t stats;
} rdb_kvdb_t;

typedef struct {
    rdb_kvdb_t *db;
    uint32_t    gen;
    uint8_t     sector;
    uint16_t    offset;
    uint8_t     _pad;
} rdb_kv_iter_t;
```

**容量公式：**

```
WR_GRAN        = 1 << write_gran
DATA_START     = ALIGN_UP(16, WR_GRAN)
DATA_CAP       = sector_size - DATA_START
REC_SZ(kl,vl) = 16 + ALIGN_UP(kl, WR_GRAN) + ALIGN_UP(vl, WR_GRAN)
MAX_LIVE       = (sector_cnt - gc_reserve) × DATA_CAP
```

#### 4.2.1 KVDB 键地址缓存 (RDB_KV_CACHE_SIZE > 0)

启用后，`rdb_kvdb_t` 内嵌一个直接映射缓存，将键指纹映射到 Flash 绝对地址，消除热点 key 的全表扫描。

**缓存槽结构（16 字节）：**
```c
typedef struct {
    uint16_t hash;       // 16-bit FNV-1a 键哈希
    uint8_t  klen;       // 键长度 (0 = 空槽)
    uint8_t  prefix[8];  // 键前 8 字节去歧义
    uint32_t addr;       // VALID 记录的 Flash 绝对地址
} rdb_kv_cache_slot_t;
```

**一致性保证：**
- 缓存命中 → 读 Flash 验证 state==VALID 且 CRC 正确 → 完整 key 字节比较 → 不一致则失效回退全表扫描
- set/delete/GC 迁移时同步更新或失效条目
- format/init 时清空全部

**性能收益：** 热点 key 的 get() 从 O(N×M) 降至 O(1)+1 次 Flash 读。典型嵌入式场景（< 100 key）命中率 > 90%。

### 4.3 KVDB API

```c
rdb_err_t rdb_kvdb_init   (rdb_kvdb_t *db, const rdb_partition_t *part,
                            void *meta_buf);
rdb_err_t rdb_kvdb_format (rdb_kvdb_t *db);
rdb_err_t rdb_kvdb_set    (rdb_kvdb_t *db, const char *key,
                            const void *val, uint16_t len);
rdb_err_t rdb_kvdb_get    (rdb_kvdb_t *db, const char *key,
                            void *buf, uint16_t buf_len, uint16_t *out_len);
rdb_err_t rdb_kvdb_delete (rdb_kvdb_t *db, const char *key);
rdb_err_t rdb_kvdb_exists (rdb_kvdb_t *db, const char *key);
rdb_err_t rdb_kvdb_gc     (rdb_kvdb_t *db);
void      rdb_kvdb_space_info (rdb_kvdb_t *db,
                                uint32_t *total, uint32_t *used, uint32_t *avail);
void      rdb_kvdb_wear_info  (rdb_kvdb_t *db,
                                uint32_t *min_ec, uint32_t *max_ec, uint32_t *avg_ec);
void      rdb_kvdb_get_stats  (rdb_kvdb_t *db, rdb_kv_stats_t *out);
void      rdb_kvdb_reset_stats(rdb_kvdb_t *db);
rdb_err_t rdb_kv_iter_init(rdb_kv_iter_t *it, rdb_kvdb_t *db);
rdb_err_t rdb_kv_iter_next(rdb_kv_iter_t *it,
                             char *key_buf, uint16_t key_cap,
                             void *val_buf, uint16_t val_cap,
                             uint16_t *out_klen, uint16_t *out_vlen);
```

**API 规格摘要：**

| 函数 | 关键返回值 | 说明 |
|------|-----------|------|
| init | OK / PARAM / FLASH / CORRUPT | meta_buf = `rdb_kvdb_meta_size(n)` |
| format | OK / FLASH | 全擦除，erase_cnt 累加保留 |
| set | OK / TOO_LARGE / FULL / FLASH | rec_sz ≤ DATA_CAP 强制检查 |
| get | OK / NOT_FOUND / TOO_LARGE / CRC | buf 不够时 out_len 仍返回实际长度 |
| delete | OK / NOT_FOUND | 全部 VALID 副本标记 DEAD |
| exists | OK / NOT_FOUND | 不执行数据 CRC |
| gc | OK / FULL | 手动触发，通常无需调用 |
| iter_next | OK / ITER_END / BUSY | 迭代期间禁止修改 |

### 4.4 初始化流程（含防死锁兜底）

```
rdb_kvdb_init(db, part, meta_buf)
│
├── 参数校验
│
├── Phase 1：逐扇区扫描
│   ├── magic=0xFFFFFFFF → 三点擦除验证 → ERASED/CORRUPT
│   ├── magic≠KV_MAGIC 或 hdr_crc 不匹配 → CORRUPT
│   └── 有效 → 遍历记录：
│       ├── 跟踪最大 create_seq + 最大 record_seq → write_seq
│       └── WRITING: CRC 匹配→VALID, 不匹配→DEAD
│
├── Phase 2：清理过期副本
│   ├── fixup_stale: 每个 VALID 记录 → find_latest → 非最新→DEAD
│   │   并将废弃记录的 rsz 计入所在扇区的 garbage_bytes
│   ├── recalc_garbage_all: 重算每个扇区的 garbage_bytes
│   └── reconcile_live: 全局重算 live_bytes
│
├── Phase 3：选择活跃扇区
│   1. create_seq 最大且有余量（write_off < sector_size）
│   2. create_seq 最大（满）
│   3. rotate 分配新扇区
│   非 active 的 ACTIVE 扇区（write_off > data_start）降级为 SEALED
│
├── CORRUPT 扇区恢复
│   对每个 CORRUPT 扇区尝试 erase；
│   成功 → ERASED；失败 → 保持 CORRUPT（降级可用）
│
├── Phase 4：安全水位恢复（防死锁兜底）
│   │
│   │  if (count_erased(db) < gc_reserve + 1)
│   │      ensure_space(db, 0, 0, 0xFF)
│   │
│   │  确保上层应用介入前，系统具备抵御突发写入的安全余量。
│   │  杜绝"掉电后重启 erased=0 导致永久 ERR_FULL"的死锁场景。
│   │  阈值为 gc_reserve + 1 而非 gc_reserve，
│   │  确保 rotate 消耗一个 erased 后仍有 gc_reserve 个备用。
│   │
│   └── GC 失败不阻塞 init（降级为受限模式，仍可删除和读取）
│
└── initialized = 1
```

**write_seq 恢复设计要点**：

init Phase 1 同时扫描扇区头的 `create_seq` 和记录的 `seq`，取两者最大值作为 `write_seq` 的初始值。此设计防止以下掉电场景中序列号回退：format 完成后无记录即掉电重启时，若仅扫描 record_seq 则 write_seq=0，导致后续 create_seq 与已有 header 冲突；同时扫描 create_seq 可正确恢复。

### 4.5 scan_sector 设计

`scan_sector` 是 KVDB 的核心扫描原语，被多个上下文调用。

```c
static void scan_sector(rdb_kvdb_t *db, uint8_t s,
                         kv_scan_cb_t cb, void *ctx,
                         int update_woff);
```

**update_woff 策略**：只有 init Phase 1 传 TRUE（需恢复 write_off）；其余所有调用点（find_latest、fixup、garbage 统计、GC 迁移）均传 FALSE。保证只读操作路径不产生副作用。

**损坏记录跳过策略**：遇到不合法的 record header（magic 不匹配、key_len/val_len 越界）时，跳过 `ALIGN_UP(KV_REC_SZ, WR_GRAN)` 字节（最小 16 字节），即一个完整记录头的宽度。此设计有两个目的：

1. **避免逐字节爬行**：若步长为 1 字节，穿越一段损坏区域需要数十到数百次无效 Flash 读取，严重拖慢 init 和扫描速度
2. **降低虚假解析风险**：步长过小可能使扫描器对齐到记录中间的某个字节序列上，该序列恰好满足 magic/key_len/val_len 的校验条件，从而解析出一条虚假记录。16 字节步长使此概率降至可忽略水平

### 4.6 写入流程

```
rdb_kvdb_set(key, val, len)
│
├── 校验: key_len ∈ [1,MAX], len ≤ MAX_VAL, rec_sz ≤ DATA_CAP
│
├── Step 1: find_latest → old_addr, old_sec, old_rsz
│
├── Step 2: ensure_space(rec_sz, old_rsz, old_sec)
│   │
│   │  传入 will_free = old_rsz、free_sec = old_sec
│   │  GC 在评分中将旧记录计为"虚拟垃圾"，但不物理删除 → 掉电安全
│   │
│   │  如果返回 ERR_FULL：
│   │  直接返回给调用者，旧记录保持 VALID 不受影响
│   │  （不存在降级路径，不会先删旧再尝试写新）
│   │
│   └── 返回 OK 后保证 active 扇区有足够空间
│
├── Step 2b: GC 后重新定位
│   │  GC 可能迁移了旧记录（新 seq、新地址），需重新查找。
│   │  find_latest(key) → 刷新 fc.best_addr / best_sec / best_rsz
│   │
│   └── 若 active 仍空间不足 → rotate
│
├── Step 3: 两阶段写入
│   │
│   │  小记录（rsz ≤ RDB_STACK_BUF_SIZE）：
│   │    header + key + val 合并到栈缓冲区一次写入
│   │
│   │  大记录（rsz > RDB_STACK_BUF_SIZE）：
│   │    header → key(+0xFF padding) → val(+0xFF padding, 对齐分块写入)
│   │
│   │    value 流式写入时按 write_gran 取对齐 chunk，最后一块合并
│   │    真实数据和 0xFF padding，HAL 永远不会收到非粒度对齐尾块。
│   │
│   │  任何写入失败 → mark_dead(wa) → ERR_FLASH
│   │
│   └── 原子提交：单字节写入 state=VALID
│
├── Step 4: 记账 write_off += rec_sz, live_bytes += rec_sz
│
└── Step 5: 作废旧副本（后提交失效）
    │  使用 Step 2b 刷新后的 fc 定位旧记录。
    │  读取旧记录 header 确认 state 仍为 VALID 后：
    │    mark_dead → garbage_bytes += old_rsz → live_bytes -= old_rsz
    │
    └── 掉电安全保证：
        即使在 Step 5 中途掉电（旧副本未被标记 DEAD），
        下次 init 的 Phase 2 fixup_stale 会基于 seq 比较
        自动完成清理。find_latest 始终返回 seq 最大者。
```

**数据安全设计要点——"先写新再删旧"原则：**

1. `ensure_space` 通过 `will_free` 参数将旧记录的空间计入 GC 的虚拟可用空间，使 GC 能更准确地评估空间
2. 如果 GC 后仍无法腾出足够空间，直接返回 `ERR_FULL`，旧记录保持 VALID
3. 只有新记录成功提交后，才在 Step 5 中作废旧副本
4. 在任何掉电时刻，用户数据要么是旧值（新记录未提交），要么是新值（已提交），永远不会丢失

**ERR_FULL 的语义**：分区确实已满，调用者需要先删除部分 key 来释放空间。这是明确的、可处理的状态，优于静默丢数据。

### 4.7 GC 算法

#### 4.7.1 空间保障入口

```
ensure_space(db, need, will_free, free_sec)
│
├── 永久满: eff_live + need > MAX_LIVE → ERR_FULL
│   其中 eff_live = max(live_bytes - will_free, 0)
│
├── 快速退出: erased ≥ gc_reserve + 1 → OK
│
├── 次快退出: active 有足够空间 且 erased 严格大于 gc_reserve → OK
│   使用严格大于（>）而非大于等于（>=），原因如下：
│   当 erased == gc_reserve 时，本次写入可能填满 active 扇区。
│   后续 rotate 需要消耗一个 erased 扇区，将 erased 降至
│   gc_reserve - 1，侵蚀 GC 安全储备。严格大于确保
│   rotate 后仍保有 gc_reserve 个备用扇区。
│
├── GC 循环（最多 sector_cnt 轮）
│   │
│   │  ── 统一扫描 ──
│   │  对所有候选扇区构建 gc_prep_cache[]。
│   │
│   │  ── will_free 虚拟垃圾注入 ──
│   │  如果 will_free > 0 且 free_sec 有效：
│   │    cache[free_sec].garbage += min(will_free, cache[free_sec].live)
│   │    cache[free_sec].live    -= 同上
│   │  使旧记录在 GC 评分中被视为垃圾，提高其所在扇区被选为
│   │  victim 的概率，而无需物理修改 Flash。
│   │
│   ├── Phase 1：零存活极速回收
│   │   在非 active 的 ACTIVE/SEALED 扇区中找 live == 0 者。
│   │   不要求 garbage > 0——空 SEALED 扇区（已写 header 但
│   │   无记录，由 rotate 产生）的 garbage 和 live 均为 0，
│   │   仍应被回收以释放宝贵的扇区资源。
│   │   选 erase_cnt 最小者 → 直接擦除（无迁移）。
│   │   少量 key 稳态下最常命中此路径。
│   │
│   ├── Phase 2：评分选择
│   │   score = garbage_pct×7 + wear_pct×3 + cost_score×1
│   │   可行性前置: victim_live ≤ gc_avail (见 4.7.2)
│   │
│   ├── Phase 3：强制降级回收
│   │   Phase 1-2 均未找到时进入
│   │   选 garbage_bytes 最大（>0）的非 active 扇区
│   │   不受垃圾率阈值限制，仍需可行性检查
│   │   防止"全盘垃圾分散低于阈值"导致系统写死
│   │
│   ├── Phase 4：静态磨损均衡
│   │   Phase 1-3 均未找到时进入
│   │   条件: max_ec - min_ec ≥ RDB_GC_WEAR_THRESHOLD
│   │   从非 ERASED、非 active 扇区中选 erase_cnt 最小者
│   │   迁移全部 live 数据后擦除
│   │   将长期不更新的冷扇区拉回擦写循环
│   │
│   ├── 无 victim → break
│   │
│   └── gc_execute(victim)
│       见 4.7.3
│
└── 最终检查
    ├── active 有足够空间 → OK
    ├── erased ≥ 1 → rotate → 验证新 active 有足够空间 → OK
    └── 否则 → ERR_FULL
```

**Phase 4 静态磨损均衡实现要点：**

victim 选择采用两轮独立循环：第一轮计算全局（含 ERASED 扇区）的 min_ec 和 max_ec；第二轮从非 ERASED、非 active 扇区中找 erase_cnt 最小者。两轮分离确保全局最小 ec 恰好落在 ERASED 或 active 扇区时，仍能正确选出次小 ec 的 SEALED 扇区作为 victim。

#### 4.7.2 GC 空间估算

```
gc_avail(db, victim) → pre_erase_avail

  avail = 0
  active_sec 有效且 ≠ victim → avail += sector_size - write_off
  非 victim 非 active 的 ERASED 扇区 → avail += DATA_CAP (每个)
  返回 avail

判定: victim_live ≤ avail → 可行

关键规则：avail 只计迁移前空间，不含 victim 擦除后释放的空间。
因为迁移必须在擦除 victim 之前全部完成。
```

#### 4.7.3 GC 执行流程

```
gc_execute(victim)
│
├── Phase A：遍历 victim 扇区，迁移全部 live 记录
│   │
│   │  对每条记录：
│   │  ├── DEAD / 非最新 → 跳过（非最新的物理标记 DEAD）
│   │  ├── VALID 且最新 → CRC 校验
│   │  │   ├── 失败 → 物理标记 DEAD → 跳过
│   │  │   │   （立即标记确保掉电后不成为幽灵记录）
│   │  │   └── 通过 → migrate_one 迁移到 active
│   │  │       迁移后标记源记录 DEAD
│   │  └── 每条迁移后调用 yield
│   │
├── Phase B：擦除 victim 扇区
│   │  erase_cnt++ → 状态设为 ERASED
│   │  擦除后调用 yield
│   │
├── Phase C：目标化残留清理
│   │  仅当 Phase A 实际迁移了记录时执行（migrated > 0）。
│   │
│   │  遍历所有非 ERASED、非 CORRUPT、非 victim 扇区：
│   │    对每个 VALID 记录执行 find_latest，
│   │    非最新者物理标记 DEAD。
│   │    然后重算该扇区的 garbage_bytes。
│   │
│   │  目的：迁移产生的新副本（seq 更高）使其他扇区中
│   │  同 key 旧副本不再是 latest。Phase C 将这些旧副本
│   │  标记为 DEAD，使 garbage_bytes 统计准确。
│   │
│   │  跳过条件（migrated == 0）：如果 victim 中没有 live 记录
│   │  需要迁移（全部是 garbage），则不存在新旧副本共存问题，
│   │  跳过 Phase C 可减少约 50% 的 GC Flash 读操作。
│   │
│   │  掉电安全：如果在 Phase C 中途掉电，部分旧副本
│   │  未被标记 DEAD。下次 init Phase 2 的 fixup_stale
│   │  会完成清理。find_latest 始终基于 seq
│   │  返回最新副本，未清理的旧副本不影响正确性。
│   │
└── Phase D：重算 live_bytes
    reconcile_live 全局重算
```

**掉电安全分析：**

| 掉电发生在 | 恢复行为 | 数据完整性 |
|-----------|---------|-----------|
| Phase A 中途（部分迁移） | init fixup_stale 保留 seq 大者 | 无损 |
| Phase B 擦除中 | 三点检测→CORRUPT→后续恢复擦除 | 无损 |
| Phase B 完成后，C 未开始 | init fixup_stale 清理旧副本 | 无损 |
| Phase C 中途 | 下次 init fixup_stale 重新完成 | 无损 |

#### 4.7.4 GC 评分算法

```
score = garbage_pct × 7 + wear_pct × 3 + cost_score × 1

garbage_pct = reclaimable / used × 100
wear_pct    = (this_ec - min_ec) / (max_ec - min_ec) × 100
cost_score  = live==0 ? 100 : 100 - min(live*100/DATA_CAP, 100)

自适应阈值：
  erased > gc_reserve  → 20%
  erased == gc_reserve → 5%
  erased < gc_reserve  → 1%

Phase 3 不受此阈值限制。
```

### 4.8 Rotate（扇区轮转）

```
rotate(db)
│
├── 从 ERASED 扇区中选择 erase_cnt 最小的
├── init_sector: erase → write header → ACTIVE
│
├── 无条件封存旧 active
│   if (old_active < sector_cnt && status == ACTIVE)
│       status = SEALED
│   无论旧 active 是否写入过数据都标记为 SEALED。
│   空 SEALED 扇区（live==0, garbage==0）由 GC Phase 1 自动回收，
│   不会永久占用扇区资源。
│
└── 更新 active_sec, write_off
```

### 4.9 live_bytes 记账

| 场景 | 操作 |
|------|------|
| 新记录写入 | live_bytes += rec_sz |
| 作废旧副本 | live_bytes -= rec_sz（下溢 clamp 0） |
| GC/init 完成 | reconcile_live 全量重算 |

### 4.10 掉电恢复表

| 掉电时刻 | Flash 残留 | init 行为 | 影响 |
|----------|-----------|----------|------|
| 写 record_hdr 中 | header 不完整 | magic 不匹配→跳过 corrupt_skip 字节 | 无损 |
| 写 key/val 中 | header 完整, data 部分 | WRITING+CRC不匹配→DEAD | 无损 |
| 写 state=VALID 中 | header+data 完整 | WRITING+CRC匹配→VALID | 无损 |
| 作废旧记录中（Step 5） | 新VALID, 旧部分作废 | fixup_stale→DEAD | 无损 |
| GC 迁移中(源未DEAD) | 目标VALID, 源VALID | fixup 保留 seq 大者 | 无损 |
| GC 擦除中 | 擦除不完整 | 三点→CORRUPT→恢复 | 无损 |
| GC fixup 中 | 部分旧副本未清理 | 下次 init fixup 完成 | 无损 |

---

## 第五章 TSDB 引擎设计

### 5.1 RAM 数据结构

```c
typedef struct {
    uint32_t write_ops;
    uint32_t read_ops;
    uint32_t sector_rotations;
    uint32_t records_lost;
    uint32_t flash_errors;
    uint32_t crc_errors;
    uint32_t data_gaps;
} rdb_ts_stats_t;

typedef struct {
    const rdb_partition_t *part;
    uint32_t  *erase_cnts;
    uint32_t   sector_size;
    uint16_t   max_data_len;
    uint8_t    sector_cnt;
    uint8_t    initialized;
    uint8_t    head_sec;
    uint8_t    tail_sec;
    uint32_t   head_seq;
    uint32_t   head_off;
    uint16_t   head_count;
    uint16_t   _pad;
    uint32_t   head_time_base;
    uint32_t   last_time;
    uint32_t   total_count;
    rdb_ts_stats_t stats;
} rdb_tsdb_t;
```

**data_gaps 含义**：环内 EMPTY 扇区数量与 seq 非单调事件的累计计数。非零值表示分区曾经历过掉电导致的不完整状态，但数据已尽最大努力恢复。

**max_data_len 计算（init 时）：**

```c
uint32_t phy_max = DATA_CAP - TS_REC_SZ;
while (phy_max > 0 && trs(db, phy_max) > DATA_CAP) phy_max--;

#if RDB_MAX_TS_DATA_LEN > 0
    db->max_data_len = (uint16_t)RDB_MIN(phy_max, RDB_MAX_TS_DATA_LEN);
#else
    db->max_data_len = (uint16_t)phy_max;
#endif
```

### 5.2 data_len 对分区行为的影响

**扇区利用率（4KB 扇区, DATA_CAP=4076）：**

| data_len | rec_sz | 每扇区记录数 | 尾部浪费 | 利用率 |
|----------|--------|-------------|---------|--------|
| 8B | 20B | 203 | 16B | 99.6% |
| 128B | 140B | 29 | 16B | 99.6% |
| 1023B | 1035B | 3 | 971B | 76.2% |
| 2032B | 2044B | 1 | 2032B | **50.1%** |
| 4064B | 4076B | 1 | 0B | 100.0% |

**磨损速率（8 扇区, W25Q128 100K 寿命, 1条/秒）：**

| data_len | 每扇区条数 | 全盘寿命 | 运行时间 |
|----------|----------|---------|---------|
| 8B | 203 | 1.624 亿条 | ~5.1 年 |
| 128B | 29 | 2320 万条 | ~268 天 |
| 1023B | 3 | 240 万条 | ~27.8 天 |
| 4064B | 1 | 80 万条 | ~9.3 天 |

### 5.3 TSDB API

```c
rdb_err_t rdb_tsdb_init       (rdb_tsdb_t *db, const rdb_partition_t *part,
                                uint32_t *ec_buf);
rdb_err_t rdb_tsdb_format     (rdb_tsdb_t *db);
rdb_err_t rdb_tsdb_append     (rdb_tsdb_t *db, uint32_t time,
                                const void *data, uint16_t len);
rdb_err_t rdb_tsdb_reset_epoch(rdb_tsdb_t *db);
rdb_err_t rdb_tsdb_query      (rdb_tsdb_t *db, uint32_t from, uint32_t to,
                                rdb_ts_cb_t cb, void *arg);
rdb_err_t rdb_tsdb_query_ex   (rdb_tsdb_t *db, uint32_t from, uint32_t to,
                                rdb_ts_cb_t cb, void *arg,
                                void *read_buf, uint16_t buf_len);
rdb_err_t rdb_tsdb_get_latest (rdb_tsdb_t *db, uint32_t *time,
                                void *buf, uint16_t buf_len, uint16_t *out_len);
rdb_err_t rdb_tsdb_get_oldest (rdb_tsdb_t *db, uint32_t *time,
                                void *buf, uint16_t buf_len, uint16_t *out_len);
uint32_t  rdb_tsdb_count      (rdb_tsdb_t *db);
void      rdb_tsdb_time_range (rdb_tsdb_t *db, uint32_t *oldest, uint32_t *newest);
void      rdb_tsdb_wear_info  (rdb_tsdb_t *db,
                                uint32_t *min_ec, uint32_t *max_ec, uint32_t *avg_ec);
void      rdb_tsdb_get_stats  (rdb_tsdb_t *db, rdb_ts_stats_t *out);
void      rdb_tsdb_reset_stats(rdb_tsdb_t *db);
```

**关键 API 规格：**

| 函数 | 说明 |
|------|------|
| append | time=0/INVALID→自动分配；len ∈ [1, max_data_len] |
| reset_epoch | 封存 head → rotate → last_time=0；跨纪元 query 可返回两个纪元的数据 |
| query | to=0→TIME_MAX；data=NULL→存在但读取失败或超栈缓冲 |
| query_ex | 外部 read_buf 用于 data_len > STACK_BUF_SIZE |
| get_latest/oldest | CRC 失败自动查找下一个有效记录；支持降级 ACTIVE 扇区 |

### 5.4 扇区分类

| magic | count | end_off | hdr_crc | 判定 |
|-------|-------|---------|---------|------|
| 0xFFFFFFFF | — | — | — | EMPTY（三点擦除验证） |
| ≠ TSDB_MAGIC | — | — | — | CORRUPT |
| OK | 0xFFFF | — | — | ACTIVE |
| OK | — | 0xFFFF | — | ACTIVE |
| OK | 有效 | 有效 | 匹配 | SEALED |
| OK | 有效 | 有效 | 不匹配 | **ACTIVE（降级）** |

**降级规则**：封存中掉电（count + end_off 已写但 hdr_crc 未完成），降级为 ACTIVE 而非 CORRUPT。这保留了扇区中的全部记录数据，后续可通过扫描恢复。若误判为 CORRUPT 则整个扇区数据将被擦除丢失。

### 5.5 辅助函数

#### ts_active_info

恢复降级 ACTIVE 扇区的 time_base 和数据结束偏移。用于 query、get_latest、get_oldest、time_range 中处理非 head 的 ACTIVE 扇区。

```
ts_active_info(db, sector, &time_base, &end_offset)
│
├── 读取 sector header → time_base
├── 从 data_start 正向扫描记录
│   ├── magic == 0xFF && state == 0xFF → 到达写入前沿 → break
│   ├── magic ≠ TS_RECORD_MAGIC → 跳过 corrupt_skip 字节
│   └── 有效记录 → off += rec_size
└── end_offset = off
```

只读操作，不修改 Flash（不修复 WRITING 记录），适合在查询路径中调用。

#### ts_sector_count

统一的扇区 VALID 记录计数，用于 rotate 淘汰计数和周期性校准。

```
ts_sector_count(db, sector) → uint16_t
│
├── ts_classify(sector)
│   ├── SEALED + count ≠ 0xFFFF → 返回 header.count（O(1) 快速路径）
│   ├── ACTIVE 或 SEALED(count=0xFFFF) → 正向扫描计数 VALID 记录
│   │   只读，不修复 WRITING 记录
│   └── EMPTY / CORRUPT → 返回 0
```

此函数是降级 ACTIVE 扇区正确计数的关键。在 rotate 淘汰 tail 扇区时，如果 tail 因之前掉电导致 seal 不完整而降级为 ACTIVE，直接读取 header.count 将得到 0xFFFF（无效值）。ts_sector_count 在此场景下退化为扫描计数，确保 records_lost 和 total_count 的统计准确。

### 5.6 初始化流程

```
rdb_tsdb_init
│
├── 参数校验 + 计算 max_data_len
│
├── Phase 1: 扫描全部扇区
│   ├── CORRUPT → 尝试 erase
│   │   ├── erase 成功 → 标记可用（ec_buf 更新）
│   │   └── erase 失败 → flash_errors++，扇区不可用
│   │       （必须检查 erase 返回值，失败的扇区不可参与后续环形索引）
│   ├── EMPTY → 跳过
│   └── ACTIVE/SEALED → 记录 head(seq最大) / tail(seq最小)
│
├── 无有效扇区 → format
│
├── Phase 2: 环形完整性验证（tail→head）
│   ├── seq 严格递增 (SEQ16_GT)
│   ├── EMPTY 在环内 → data_gaps++（容忍，不报 CORRUPT）
│   └── seq 非单调 → data_gaps++（容忍，不触发 format）
│       设计理由：16-bit seq 回绕或异常写入可能产生非单调序列。
│       若直接 format 将丢失全分区数据。降级为 data_gaps 计数
│       后，init 仍以 seq 最大者为 head、最小者为 tail，
│       尽最大努力保留可恢复数据。应用层可通过 stats.data_gaps
│       检测此异常并决定是否手动 format。
│
├── Phase 3: 加载 head 扇区状态
│   ├── SEALED → head_off=sector_size（下次 append 触发 rotate）
│   │   扫描获取 last_time；损坏记录跳过 corrupt_skip 字节
│   └── ACTIVE → 扫描修复 WRITING 记录（CRC→VALID / DEAD）
│       确定 head_off, head_count, last_time
│
├── Phase 4: 统计 total_count
│   从 tail 到 head 遍历：
│     head 扇区使用 head_count（RAM 权威值）
│     其余扇区使用 ts_sector_count（处理 SEALED 和降级 ACTIVE）
│
├── Phase 5: last_time 兜底扫描
│   若 last_time==0 且 total_count>0，全扇区扫描取最大时间
│   支持 SEALED 和降级 ACTIVE 扇区（通过 ts_active_info）
│
└── initialized = 1
```

### 5.7 Append 流程

```
rdb_tsdb_append(db, time, data, len)
│
├── 校验: len ∈ [1, max_data_len]
│
├── 时间戳处理
│   ├── last_time ≥ TIME_MAX → ERR_TIME_EXHAUSTED
│   ├── time=0 或 INVALID → time = last_time + 1（自动分配）
│   ├── time ≤ last_time → time = last_time + 1（单调强制）
│   └── time > TIME_MAX → ERR_TIME_EXHAUSTED
│
├── Rotation 决策
│   ├── delta 溢出: head_time_base ≠ INVALID 且
│   │   (time < head_time_base 或 time - head_time_base > 0xFFFFFFFE)
│   └── 空间不足: head_off + rec_sz > sector_size
│       → ts_rotate()
│
├── time_base 写入（首条记录）
│
├── delta = (uint32_t)(time - head_time_base)
│
├── 两阶段写入 (WRITING → VALID)
│   ├── 小记录（rsz ≤ RDB_STACK_BUF_SIZE）：合并写入
│   └── 大记录：header → data → padding（循环写入）→ commit
│
├── 更新 head_off, head_count, last_time, total_count
│
├── 写入失败安全（v1.3.0）
│   ├── header/data 写失败 → ts_mark_dead(wa)，物理标记 DEAD
│   └── commit byte 写失败 → head_off+=rsz, head_count--，推进前沿
│
└── total_count 增量维护（v1.3.0）
    每次 append 成功 +1，rotation 覆盖 tail 时减去 lost。
    移除周期性 O(N) 全量重算，消除高频写入延迟尖峰。

### 5.8 Rotation 流程

```
ts_rotate(db)
│
├── 1. 封存当前 head: ts_seal(head_sec, head_count, head_off)
│      count → end_off → hdr_crc（三次独立写入）
│
├── 2. 计算 next = (head_sec + 1) % sector_cnt
│
├── 3. 如果 next == tail_sec（环形覆盖）
│   │
│   ├── lost = ts_sector_count(tail_sec)
│   │   统一处理 SEALED 和降级 ACTIVE 的计数（见 5.5）。
│   │   降级 ACTIVE 场景：之前 seal 的 CRC 写入失败导致扇区
│   │   被 ts_classify 判定为 ACTIVE 而非 SEALED。此时 header
│   │   中的 count 字段为 0xFFFF，ts_sector_count 自动退化为
│   │   正向扫描计数 VALID 记录，确保 records_lost 准确。
│   │
│   ├── records_lost += lost
│   ├── total_count -= lost（下溢 clamp 0）
│   └── tail_sec = (tail_sec + 1) % sector_cnt
│
├── 4. head_seq++
│
├── 5. ts_init_sec(next, head_seq)
│      erase_cnt 处理：取 max(RAM 中 ec_buf[s], Flash header.erase_cnt)
│      再加 1。取 max 而非直接使用 Flash 值，防止同一次运行中
│      多次 init_sec 同一扇区时 erase_cnt 回退。
│      erase → 写 header
│
├── 6. 更新 head_sec, head_off=data_start, head_count=0,
│      head_time_base=INVALID
│
├── 7. sector_rotations++
│
└── 8. yield（sector erase 是长操作，主动喂狗）
```

### 5.9 Query 设计

```
ts_query_impl(db, from, to, cb, arg, rbuf, rlen)
│
├── 从 tail 向 head 遍历 ring 中每个扇区
│   │
│   │  不执行基于 time_base 的扇区级跳过（early break）。
│   │
│   │  设计理由：reset_epoch 后，ring 中较晚位置的扇区
│   │  （新 epoch）可能有比较早位置扇区（旧 epoch）更小的
│   │  time_base。如果基于 time_base > to 提前终止遍历，
│   │  会跳过新 epoch 中落在查询范围内的有效数据。
│   │
│   │  对每个扇区：
│   │  ├── SEALED + time_base 有效 → ts_scan(time_base, end_off)
│   │  ├── ACTIVE + 是 head + head_count>0 + time_base 有效
│   │  │   → ts_scan(head_time_base, head_off)
│   │  └── ACTIVE + 非 head（降级扇区）
│   │      → ts_active_info 获取 time_base 和 end_off
│   │      → ts_scan(time_base, end_off)
│   │
│   │  ts_scan 内部由 ts_qcb 回调执行记录级范围过滤：
│   │  ├── time > to → ITER_STOP（停止当前扇区扫描）
│   │  ├── time < from → CONTINUE（跳过）
│   │  └── time ∈ [from, to] → CRC 验证 → 用户回调
│   │      CRC 失败 → 回调收到 data=NULL
│   │
│   └── 如果用户回调返回 ITER_STOP → 终止全部
│
└── 返回 OK
```

**跨 epoch query 的行为：**

`reset_epoch` 后查询 `query(from=1, to=100)` 可能同时返回旧 epoch 和新 epoch 中时间戳落在 [1,100] 的记录。这是设计预期——两个 epoch 的数据在 ring 中共存直到旧数据被自然覆盖。如果应用需要严格的 epoch 隔离，应在应用层维护 epoch 编号并在回调中过滤。

### 5.10 get_latest / get_oldest 设计

**get_latest 流程：**

```
1. head 扇区优先：若 head_count > 0 且 time_base 有效
   → ts_find_last_valid(head_sec, head_time_base, head_off)
   正向扫描只保留最后一条 VALID 记录（只读，不修复 WRITING）

2. 反向搜索：从 head 的前一个扇区向 tail 方向遍历
   ├── SEALED + time_base 有效 + count > 0
   │   → ts_find_last_valid(s, time_base, end_off)
   ├── ACTIVE（降级）
   │   → ts_active_info(s, &tb, &eo)
   │   → ts_find_last_valid(s, tb, eo)
   └── 到达 tail_sec 后停止

3. 找到后 CRC 校验数据，通过则返回
```

**get_oldest 流程：**

```
1. 从 tail 向 head 正向搜索
   ├── SEALED + time_base 有效 + count > 0
   │   → ts_scan 取第一条 VALID → ITER_STOP
   ├── ACTIVE + 是 head + head_count > 0 + time_base 有效
   │   → ts_scan 取第一条 VALID → ITER_STOP
   └── ACTIVE + 非 head（降级）
       → ts_active_info(s, &tb, &eo)
       → ts_scan 取第一条 VALID → ITER_STOP

2. 找到后 CRC 校验数据
```

降级 ACTIVE 扇区的处理是 get_latest/get_oldest/query/time_range 四个查询 API 共有的设计要求。降级扇区出现在以下场景：

- seal 过程中掉电（count/end_off 已写但 CRC 未完成）
- seal 过程中 Flash 写入失败（CRC 写入返回非0）

如果查询 API 不处理降级 ACTIVE，当 tail 扇区降级时 get_oldest 将跳过它，返回次老扇区的数据或 NOT_FOUND，导致应用层获取的时间范围不准确。

### 5.11 ts_data_crc 接口

```c
static int ts_data_crc(const rdb_tsdb_t *db,
                        uint32_t data_addr,
                        uint16_t data_len,
                        uint16_t *out_crc);
```

返回 0 成功，-1 Flash 失败。CRC 通过 `*out_crc` 返回。不用返回值本身作 CRC，避免 Flash 故障返回 0 与合法 CRC 0x0000 冲突导致虚假匹配。

### 5.12 掉电恢复表

| 掉电时刻 | init 行为 | 影响 |
|----------|----------|------|
| 写 time_base 中 | 非法值→head_count=0, 重写 | 无损 |
| 写 record_hdr 中 | 扫描终止（magic/state = 0xFF） | 无损 |
| 写 data 中 | WRITING+CRC不匹配→DEAD | 无损 |
| 写 state=VALID 中 | CRC匹配→VALID | 无损 |
| seal: 写 count 后 | end_off=0xFFFF→ACTIVE（降级） | 无损 |
| seal: 写 end_off 后 | hdr_crc=0xFFFF→CRC不匹配→ACTIVE（降级） | 无损 |
| seal: 写 crc 后 | SEALED | 无损 |
| rotate: seal 完, erase 前 | head=旧(SEALED) | 无损 |
| rotate: erase 完, init 前 | 新扇区 EMPTY→data_gaps | 无损 |

---

## 第六章 集成指南

### 6.1 初始化代码（裸机 / 传统 RTOS）

```c
static const rdb_flash_ops_t flash_ops = {
    .read   = w25q_read,
    .write  = w25q_page_program,
    .erase  = w25q_sector_erase_4k,
    .lock   = os_mutex_lock,
    .unlock = os_mutex_unlock,
    .yield  = iwdg_refresh,
};

static const rdb_partition_t kvdb1_part = {
    .name = "kvdb1", .base_addr = 0x000000,
    .total_size = 32*1024, .sector_size = 4096,
    .write_gran = 0,
    .ops = &flash_ops,
    .flash_ctx = NULL,  /* 裸机无上下文 */
};

static rdb_kvdb_t kvdb1;
static uint8_t    kvdb1_meta[128];  /* 8 × 16 */

void storage_init(void)
{
    if (rdb_kvdb_init(&kvdb1, &kvdb1_part, kvdb1_meta) != RDB_OK)
        rdb_kvdb_format(&kvdb1);
}
```

### 6.1.1 Zephyr OS 初始化

```c
#include <zephyr/kernel.h>
#include <zephyr/drivers/flash.h>
#include <rocketdb.h>
#include <rocketdb_port.h>

#define MY_FLASH_DEV  DEVICE_DT_GET(DT_NODELABEL(flash0))
#define MY_PART_OFF   0x00080000
#define MY_PART_SIZE  (16 * 4096)   /* 16 sectors @ 4KB */

static struct rocketdb_flash_ctx kv_ctx;
static rdb_partition_t kv_part;
static rdb_kvdb_t kvdb;
static uint8_t kv_meta[rdb_kvdb_meta_size(16)];

void storage_init(void)
{
    rocketdb_partition_init(&kv_part, &kv_ctx, MY_FLASH_DEV,
                            MY_PART_OFF, MY_PART_SIZE, 4096,
                            0, "config-kv");

    if (rdb_kvdb_init(&kvdb, &kv_part, kv_meta) != RDB_OK)
        rdb_kvdb_format(&kvdb);
}
```

Zephyr 适配文件位于 `zephyr/rocketdb_port.c`，提供：
- `rdb_flash_ops_t` 的 Zephyr flash API 实现
- `rdb_crc16 / rdb_crc16_cont`（CRC-16/MODBUS）
- `rdb_hash16`（FNV-1a folded to 16-bit，与仿真测试一致）
- `rocketdb_partition_init()` 工厂函数

Zephyr 适配层在 `write()` 回调中强制执行 `write_gran` 对齐，并在
read/write/erase 前检查 flash device ready 状态；`CONFIG_ROCKETDB_MAX_KEY_LEN`
与核心架构保持一致，范围为 1..32。

### 6.2 大记录 query_ex 缓冲

```c
#if RDB_MAX_TS_DATA_LEN > 0
    uint8_t qbuf[RDB_MAX_TS_DATA_LEN];
#else
    uint8_t qbuf[4096];
#endif

rdb_tsdb_query_ex(&tsdb, from, to, cb, arg, qbuf, sizeof(qbuf));
```

### 6.3 yield 调用点

| 模块 | 调用时机 |
|------|---------|
| KVDB GC | 每成功迁移一条记录后 |
| KVDB GC | 擦除 victim 扇区后 |
| TSDB | rotation 完成（含擦除）后 |

### 6.4 分区规划

```
W25Q128 (16MB):

地址           大小    用途
0x000000      32KB    KVDB1（设备配置，~20 key）
0x008000      32KB    KVDB2（运行参数，~30 key）
0x010000      32KB    TSDB1（告警日志，~64B/条）
0x018000      32KB    TSDB2（传感器采样，~8B/条）
0x020000      ~15.9MB 应用固件/预留
```

### 6.5 ERR_FULL 处理建议

```c
rdb_err_t rc = rdb_kvdb_set(&kvdb, key, val, len);
if (rc == RDB_ERR_FULL) {
    rdb_kvdb_delete(&kvdb, "low_priority_key");
    rc = rdb_kvdb_set(&kvdb, key, val, len);
    /* 旧值（如果存在）未被损坏，仍可通过 get 读取 */
}
```

### 6.6 时间戳建议

| 时间源 | 耗尽 | 建议 |
|--------|-----|------|
| Unix 秒 | ~136 年 | 直接使用 |
| 毫秒 SysTick | ~49.7 天 | 监控 last_time，及时 reset_epoch |
| 自增 (time=0) | ~42.9 亿次 | 每秒 100 次 1.36 年 |

### 6.7 移植与并发注意事项

| 主题 | 要求 |
|------|------|
| 初始化时机 | `init/format` 可能执行恢复写入、擦除和 GC，应在 Flash HAL 可用且锁机制已初始化后调用。若系统启动早期没有调度器，必须保证此阶段没有其它 Flash 访问者。 |
| 锁回调 | `lock/unlock` 应覆盖同一物理 Flash 上所有 RocketDB 分区以及应用层直接 Flash 访问。不要只保护单个 DB 句柄。 |
| yield 回调 | `yield` 只允许喂狗或让出 CPU，禁止在回调中再次调用 RocketDB API，避免重入死锁。 |
| 统计读取 | `space_info/wear_info/get_stats/time_range/count` 属于观测接口，仍需要遵守锁语义，因为底层元数据可能被 GC/rotation 更新。 |
| write_gran | HAL 必须拒绝未对齐写入。即使底层芯片允许 byte program，测试配置为 2/4/8B 时也要模拟目标平台约束。 |
| 真实硬件 | 软件模拟不能覆盖电源斜率、片选毛刺、页编程边界、擦除中断后的不确定内容。生产前需做目标板断电测试。 |

---

## 第七章 工具函数

```c
uint32_t    rdb_version(void);           /* 返回 0x010102 (v1.1.2) */
size_t      rdb_kvdb_meta_size(uint8_t sector_cnt);  /* N × 16 */
size_t      rdb_tsdb_ec_size(uint8_t sector_cnt);    /* N × 4  */
```

---

## 第八章 审核清单

### 8.0 审核报告整合索引

代码审核文件记录了从 v1.0.0 到 v1.1.2 的多轮缺陷发现和修复。本设计文档不逐条复制历史报告，而将关键点整合为以下架构门禁。

| 类别 | 已纳入设计的关键点 | 设计位置 |
|------|-------------------|----------|
| Flash 原子性 | WRITING→VALID→DEAD、提交失败推进写前沿、先写新再删旧、victim 迁移成功后才擦除 | 1.2.1、1.2.2、4.6、4.7 |
| 掉电恢复 | KVDB init fixup、TSDB seal 降级 ACTIVE、query 无副作用、三点擦除验证 + CRC 缓解 | 4.4、4.10、5.4、5.6、5.12 |
| 位宽与对齐 | On-Flash 结构体自然对齐、RAM offset/seq 使用 32 位、CRC offset 静态断言 | 2.4、3.5、4.2、5.1 |
| GC 与磨损 | gc_reserve+1 水位、will_free 虚拟垃圾、Phase 3 强制回收、Phase 4 静态磨损均衡、erase_cnt 单调 | 4.7、4.8、8.1 |
| 并发与锁 | 公共 API 锁语义、iter_gen 失效检测、stats/space/wear 读取一致性 | 2.5、4.3、6.7、8.1 |
| TSDB 查询 | 不按 time_base early break、跨 epoch 返回语义、降级 ACTIVE 参与 query/latest/oldest/time_range | 5.5、5.9、5.10 |
| 测试有效性 | 7 套测试、44 用例、约 39,000 断言；GC/rotation ≥100；故障注入覆盖 6 类故障；seed 可追溯 | 8.2 |
| 工程化 | CMake/CTest、bat、Makefile 都应构建真实测试套件；示例缺失不能破坏核心库构建 | 8.3 |

### 8.1 实现审核

| 编号 | 检查项 | 通过条件 |
|------|--------|---------|
| R-01 | 防死锁初始化 | init Phase 4：erased < gc_reserve+1 时强制 GC |
| R-02 | GC Phase C 目标化清理 | migrated > 0 时执行；migrated = 0 时跳过 |
| R-03 | GC 扫描缓存 | prep_cache 每轮每扇区仅构建一次 |
| R-04 | gc_avail | 返回迁移前空间，不含 victim 擦除后 |
| R-05 | GC CRC 失败 | 立即物理 DEAD，不留幽灵 |
| R-06 | ensure_space 次快退出 | erased 严格大于 gc_reserve（不含等于） |
| R-07 | Phase 3 强制降级 | garbage 最大，不受阈值 |
| R-08 | Phase 4 静态均衡 | 两轮独立循环：全局 min/max + 非 ERASED 非 active 最小 ec |
| R-09 | yield 调用 | 迁移每条后 + 擦除后 + rotation 后 |
| R-10 | ts_classify 降级 | count+end_off 有效但 CRC 不匹配 → ACTIVE |
| R-11 | delta 溢出检查 | uint32_t，独立于 head_count |
| R-12 | ts_data_crc | int 返回 + uint16_t* 输出 |
| R-13 | MAX_KEY_LEN 断言 | ≤ 32 |
| R-14 | reset_epoch | 封存 + rotate + last_time=0 |
| R-15 | ERR_TIME_EXHAUSTED | last_time ≥ TIME_MAX 时阻止 |
| R-16 | 对齐填充 | 0xFF |
| R-17 | live_bytes 下溢 | clamp 到 0 |
| R-18 | rec_sz ≤ DATA_CAP | set 入口检查 |
| R-19 | max_data_len | init 时 min(物理上限, RDB_MAX_TS_DATA_LEN) |
| R-20 | Cortex-M0 对齐 | seq@offset8, time_delta@offset4 → 4B 对齐 |
| R-21 | 迭代器隔离 | iter_next 期间写入 → ERR_BUSY |
| R-22 | EMPTY gap 容灾 | 环内 EMPTY 扇区计入 data_gaps，不报 CORRUPT |
| R-23 | 数据安全优先 | set 不存在降级路径；ensure_space 失败时旧记录保持 VALID |
| R-24 | write_seq 恢复 | init 取 max(最大 create_seq, 最大 record_seq) |
| R-25 | scan_sector 无副作用 | 只读调用路径传 update_woff=FALSE |
| R-26 | will_free 虚拟垃圾 | GC cache 中注入，不物理修改 Flash |
| R-27 | ensure_space 最终检查 | rotate 后验证新 active 有足够空间 |
| R-28 | TSDB query 无 early break | 不基于 time_base 提前终止，支持跨 epoch |
| R-29 | TSDB tail 降级态计数 | ts_sector_count 统一处理 SEALED 和 ACTIVE |
| R-30 | TSDB init CORRUPT erase | 检查 erase 返回值，失败时 flash_errors++ |
| R-31 | 损坏记录跳过步长 | ALIGN_UP(REC_HDR_SZ, WR_GRAN)，不逐字节 |
| R-32 | 大 val/data padding | 循环写入，不受固定缓冲区限制 |
| R-33 | rotate 封存 | 无条件封存旧 active；空 SEALED 由 Phase 1 回收 |
| R-34 | erase_cnt 单调性 | KVDB format 取 max；TSDB ts_init_sec 取 max |
| R-35 | seq 非单调容忍 | data_gaps++ 而非 format，保留数据 |
| R-36 | 降级 ACTIVE 查询 | get_oldest/get_latest/query/time_range 均支持 |
| R-37 | total_count 校准 | 仅在 append 中单点触发，不重复 |
| R-38 | ts_active_info 只读 | 不修复 WRITING，不写 Flash |
| R-39 | ts_sector_count 双路径 | SEALED 读 header O(1)；ACTIVE 扫描 O(N) |
| R-40 | GC Phase 1 零 live | 不要求 garbage > 0，空 SEALED 可回收 |
| R-41 | GC 后重新定位 | set Step 2b 中 find_latest 刷新旧记录位置 |
| R-42 | 扇区偏移位宽 | KVDB write_off、TSDB head_off 在 RAM 中均为 uint32_t |
| R-43 | TSDB head_seq 位宽 | RAM 中 head_seq 为 uint32_t，避免 65536 次 rotation 后失真 |
| R-44 | 提交失败清理 | commit/state 写失败后推进 write_off 或标记 DEAD，不复用物理地址 |
| R-45 | 关键返回值检查 | seal、mark_dead、padding write、erase、CRC read 失败均返回错误或计入 stats |
| R-46 | 公共观测接口锁 | stats/space/wear/time_range/count 读取共享元数据时遵守锁语义 |
| R-47 | 零动态内存 | 引擎无 malloc/free；无静态全局临时缓冲；大临时状态复用 caller buffer |
| R-48 | erase_cnt 不回退 | format/init_sec 使用 RAM 与 Flash header 中 erase_cnt 的较大值 |
| R-49 | CRC offset 断言 | sector header CRC 覆盖范围由 offsetof 静态断言守护 |
| R-50 | write_gran 对齐 | 模拟器与 HAL 对 addr/len 强制执行 write_gran 对齐 |
| R-51 | strlen 安全 | key 长度扫描最多检查 RDB_MAX_KEY_LEN+1，非 NUL 终止返回 PARAM/TOO_LARGE |
| R-52 | 故障统计可见 | Flash/CRC/corrupt/data_gaps 等异常不静默吞掉，应用可通过 stats 观测 |

### 8.2 测试审核

| 编号 | 场景 | 验证目标 |
|------|------|---------|
| T-01 | 8扇区 KVDB 20key 循环 ≥100 GC | 无死锁，数据一致 |
| T-02 | 所有扇区垃圾 < 1% 时写入 | Phase 3 生效 |
| T-03 | 静态扇区磨损差 ≥ 阈值 | Phase 4 触发 |
| T-04 | GC CRC 坏记录 | 标记 DEAD |
| T-05 | init 后 erased=0 | Phase 4 水位恢复生效 |
| T-06 | TSDB seal 全掉电点 | 降级 ACTIVE，数据可读 |
| T-07 | head_count=0 大 delta | 正确处理 |
| T-08 | 时间戳 TIME_MAX 边界 | 正确阻止/允许 |
| T-09 | reset_epoch 后写入 | 新纪元正确 |
| T-10 | GC 看门狗 | yield 调用 |
| T-11 | ts_data_crc flash 失败 | 无虚假匹配 |
| T-12 | victim_live > pre_erase_avail | 不选 |
| T-13 | 迭代期间写入 | ERR_BUSY |
| T-14 | key 长度 MAX+1 | ERR_TOO_LARGE |
| T-15 | 8扇区 TSDB ≥100 rotation | total_count 准确 |
| T-16 | format 后 erase_cnt | 累加正确 |
| T-17 | 满数据库更新已有 key | ERR_FULL，旧数据完整 |
| T-18 | value 4B↔512B 反复 | 写入正常，GC 稳定 |
| T-19 | TSDB max_data_len 边界 | len=max 成功，max+1 拒绝 |
| T-20 | TSDB 单扇区 1 条大记录 | 封存恢复正确 |
| T-21 | TSDB 同扇区跨 30 天 | delta 正确 |
| T-22 | query_ex 1KB+ 记录 | 数据完整 |
| T-23 | 20key 5000 次更新 | GC 稳定 |
| T-24 | RDB_MAX_TS_DATA_LEN=0 | max_data_len=物理上限 |
| T-25 | RDB_MAX_TS_DATA_LEN=128 | max_data_len=128 |
| T-26 | GC Phase C 中途掉电 | 下次 init fixup 完成 |
| T-27 | TSDB 环内 EMPTY gap | data_gaps++，不 format |
| T-28 | Cortex-M0 结构体访问 | 无 HardFault |
| T-29 | format 后无记录即掉电重启 | write_seq 正确恢复 |
| T-30 | reset_epoch 后 query(1,100) | 返回新旧两 epoch 数据 |
| T-31 | scan_sector 只读路径 | get/exists/iter 不修改 write_off |
| T-32 | will_free 路径 | GC 虚拟垃圾注入正确 |
| T-33 | TSDB init CORRUPT + erase 失败 | flash_errors++，不崩溃 |
| T-34 | 满数据库删除后更新 | 空间释放后 set 成功 |
| T-35 | GC migrated=0 | Phase C 跳过，zero-live 回收高效 |
| T-36 | erased == gc_reserve 时写入 | 触发 GC，不放行 |
| T-37 | 连续损坏区域扫描 | 跳过步长 ≥ 16B |
| T-38 | write_gran=3 大 value padding | 对齐完整 |
| T-39 | 空 active rotate | SEALED → Phase 1 回收 |
| T-40 | 同 sector 多次 ts_init_sec | erase_cnt 单调递增 |
| T-41 | seq 非单调初始化 | 数据保留，data_gaps++ |
| T-42 | 降级 ACTIVE tail 的 get_oldest | 正确返回第一条 VALID |
| T-43 | 100 次 rotation 后 total_count | 校准无偏差 |
| T-44 | READ_FAIL / WRITE_FAIL / ERASE_FAIL | 错误码和 stats.flash_errors 可观测 |
| T-45 | POWER_LOSS 部分写入 | 字节级部分写真实发生，init 后数据一致 |
| T-46 | DATA_CORRUPT / BIT_FLIP | CRC 检出，坏记录隔离，不影响其它记录 |
| T-47 | 随机 workload seed | 日志记录 seed，失败可复现 |
| T-48 | wear_heatmap | 验证 min/max/avg erase_count 分布，不使用永真断言 |
| T-49 | write_gran=0/1/2/3 | KVDB 全矩阵；TSDB 明确当前支持范围 |
| T-50 | CMake/CTest | 7 个测试目标注册且 100% 通过 |
| T-51 | Windows bat | `build\build.bat all build/test` 均成功，日志时间戳与 locale 无关 |

### 8.3 工程化审核

| 编号 | 检查项 | 通过条件 |
|------|--------|---------|
| B-01 | CMake 默认配置 | `BUILD_EXAMPLES=ON` 时，缺失示例不影响核心库和测试构建 |
| B-02 | CTest 注册 | 7 个 `test/sim/test_*.c` 均注册为独立测试 |
| B-03 | Windows Clang 链接 | 不使用 freestanding 链接规则，host-side 测试能链接 CRT |
| B-04 | bat 字符集 | 脚本仅使用 ASCII 控制文本，避免 cmd 编码误解析 |
| B-05 | 日志命名 | 时间戳使用 `yyyyMMdd_HHmmss`，不依赖系统区域格式 |
| B-06 | 文档同步 | README、test_plan、sim README、SpecKit 与当前 API/测试结果一致 |

---

## 第九章 已知限制

| 限制 | 说明 | 规避 |
|------|------|------|
| 单记录不跨扇区 | KVDB: rec_sz ≤ DATA_CAP; TSDB: data ≤ max_data_len | 分片或增大扇区 |
| TSDB 时间戳 32 位 | 秒 ~136 年；毫秒 ~49.7 天 | reset_epoch |
| 大 data_len 磨损快 | 每扇区记录少→rotation 频繁 | 控制 data_len 或增大分区 |
| 大 data_len 尾部浪费 | 最差 ~50% | 选择整除对齐的 data_len |
| 大记录 query 需外部缓冲 | > STACK_BUF_SIZE 时 data=NULL | 使用 query_ex |
| 无事务 | 多 key 非原子 | 应用层 |
| GC 不可中断 | yield 缓解 | 控制记录数 |
| 迭代器只读 | ERR_BUSY | 先完成 |
| 满数据库更新返回 FULL | 所有扇区充满有效数据且无垃圾 | 先删除部分 key |
| 三点擦除验证 | ~1.2% 覆盖 | 依赖硬件原子性 |
| 跨纪元查询返回多 epoch | reset_epoch 后时间戳可能重叠 | 应用层 epoch 过滤 |
| KVDB init fixup_stale 复杂度 | 最坏 O(N²)，32KB 可接受 | 大分区需索引优化 |
| KVDB delete 不触发 GC | 删除后空间不立即回收 | 后续 set 自动触发，或手动 gc |
| KVDB 查找无索引 | O(N) 线性扫描 | 大分区需引入索引机制 |
| TSDB wg≥2 未作为主支持目标 | 20B 扇区头、2B seal 字段和部分 HAL 粒度组合不兼容 | 当前 init/format 明确拒绝；生产使用前保持 wg=0/1，或重新设计头部布局 |
| header CRC 覆盖有限 | KVDB sector header CRC 不覆盖全部历史/磨损语义 | erase_cnt 采用 RAM/Flash max 策略，关键字段用静态断言约束 |
| uint32 序列比较理论边界 | 超过 2^31 差值时 wrap-safe 比较会翻转 | 典型 NOR 寿命内不可达；超长寿命场景需格式迁移到 64 位 |
| 查询线性扫描 | TSDB range query、KVDB get 在大分区下延迟增长 | 未来增加索引/游标；当前通过分区规划控制规模 |
| 真实硬件故障模型差异 | 模拟器无法覆盖所有电气边界 | 量产前执行目标板断电和长时耐久测试 |
