# Specify（规格）

## 模块概述

RocketDB v0.0.2 是针对资源受限嵌入式系统的双模 Flash 存储引擎。提供：

- **KVDB**：日志结构键值存储，少量 key 频繁更新，自动垃圾回收，四阶段评分 GC + 静态磨损均衡
- **TSDB**：环形时序存储，追加写入、时间范围查询、自动淘汰，原生支持防时间戳回绕（Epoch 机制）

## 构建与运行期望

### 工具链要求

| 工具 | 要求 | 说明 |
|------|------|------|
| 编译器 | Clang ≥14 或 GCC ≥11 | `D:\Programs\LLVM\bin\clang.exe`（build.bat 默认） |
| 标准 | C99 | `-std=c99` |
| 警告 | 全开 | `-Wall -Wextra` |
| 优化 | `-O2 -g` | 测试时保留调试符号 |
| Windows 兼容 | `-D_CRT_SECURE_NO_WARNINGS` | 压制 MSVC CRT 警告 |

### 源文件清单

```
# 引擎源文件（项目根目录）
rocketdb.h              公共头文件（含所有类型与 API 声明）
rocketdb_kvdb.c         KVDB 引擎实现
rocketdb_tsdb.c         TSDB 引擎实现

# 测试 / 模拟层（test/sim/）
test/sim/sim_flash.c    NOR Flash 模拟器（1→0 写入、故障注入）
test/sim/sim_flash.h
test/sim/sim_vectors.c  确定性测试向量生成器
test/sim/sim_vectors.h
test/sim/sim_crypto.c   CRC16 / Hash16 实现（外部依赖提供）
test/sim/sim_runner.c   测试主程序（main）
```

### Include 路径

```
-I.          # rocketdb.h
-Itest/sim   # sim_flash.h, sim_vectors.h
```

### 输出目录结构

所有编译产物、可执行文件、测试向量和日志统一输出到 `test/out/`：

```
test/out/
├── sim_test.exe                  测试可执行文件
├── *.o                           中间目标文件（Makefile 增量编译产生）
├── sim_log.txt                   最近一次 sim 运行的控制台日志
├── kv_vectors.bin                生成的 KVDB 测试向量（2000 条）
├── ts_vectors.bin                生成的 TSDB 测试向量（2000 条）
└── test_log_YYYYMMDD_HHMMSS.log  带时间戳的完整测试日志
```

> `test/out/` 应加入 `.gitignore`，不纳入版本控制。

### 构建命令

```bat
REM Windows — build.bat（推荐，无需 make）
build.bat            REM 仅编译，生成 test\out\sim_test.exe
build.bat test       REM 编译 + 运行 + 生成带时间戳日志
build.bat clean      REM 删除整个 test\out\ 目录
build.bat rebuild    REM clean 后重新编译
build.bat help       REM 显示帮助
```

```makefile
# Unix/Linux / Windows(make) — Makefile
make           # 增量编译（.o 文件 → sim_test.exe）
make test      # 编译并运行测试
make clean     # 删除 test/out/
make rebuild   # clean 后重新编译
```

### 测试日志格式

`build.bat test` 在 `test/out/` 目录生成 `test_log_YYYYMMDD_HHMMSS.log`，内容：

```
==========================================
RocketDB Sim Test Log
Test Date: YYYY-MM-DD_HH:mm:ss
==========================================
[来自 sim_test.exe 的全部 stdout/stderr]
```

测试可执行文件返回非零退出码时，`build.bat test` 同样以非零退出，便于 CI/CD 捕获失败。

## 编译时配置（来自 design.md 第二章）

### 核心配置宏

```c
#define RDB_MAX_KEY_LEN         32u       /* 1~32，KVDB key 最大长度 */
#define RDB_MAX_VAL_LEN         4095u     /* 0~65535，KVDB value 最大长度 */
#define RDB_MAX_TS_DATA_LEN     0u        /* 0=由扇区决定；>0=软限制 */
#define RDB_STACK_BUF_SIZE      64u       /* 栈缓冲大小，用于小记录合并写入 */
#define RDB_GC_GARBAGE_PCT      20u       /* 垃圾率触发 GC 的百分比（Phase 1）*/
#define RDB_GC_WEAR_THRESHOLD   100u      /* 磨损均衡差异阈值（Phase 4）*/
#define RDB_MIN_SECTOR_SIZE     4096u     /* 最小扇区大小 */
#define RDB_KV_MIN_SECTORS      3u        /* KVDB 最少扇区数 */
#define RDB_TS_MIN_SECTORS      2u        /* TSDB 最少扇区数 */
#define RDB_MAX_SECTORS         255u      /* 最多扇区数 */
```

### 配置说明

**RDB_MAX_TS_DATA_LEN 语义**：

| 配置值 | 行为 |
|--------|------|
| 0（默认） | `max_data_len` = 扇区物理上限（sector_size - 头部大小）|
| 1~65535 | `max_data_len` = min(物理上限, 配置值) |

建议显式配置，以便编译时确定 query 缓冲区大小。

**RDB_GC_RESERVE 计算方式**：
```c
#define RDB_GC_RESERVE(n)   ((uint8_t)(((n) >= 16u) ? 2u : 1u))
```
- ≥16 扇区：保留 2 个擦除扇区作为 GC 安全水位
- <16 扇区：保留 1 个擦除扇区

### 编译时断言

```c
_Static_assert(RDB_MAX_KEY_LEN >= 1u && RDB_MAX_KEY_LEN <= 32u,
               "RDB_MAX_KEY_LEN must be 1..32");
_Static_assert(RDB_MAX_VAL_LEN >= 0u && RDB_MAX_VAL_LEN <= 65535u,
               "RDB_MAX_VAL_LEN must be 0..65535");
_Static_assert(sizeof(rdb_kv_sector_hdr_t) == 16u, "KV sector header size");
_Static_assert(sizeof(rdb_kv_record_hdr_t) == 16u, "KV record header size");
_Static_assert(sizeof(rdb_ts_sector_hdr_t) == 20u, "TS sector header size");
_Static_assert(sizeof(rdb_ts_record_hdr_t) == 12u, "TS record header size");
```

## 公共错误码

```c
typedef enum {
    RDB_OK                  =   0,    /* 操作成功 */
    RDB_ERR_PARAM           =  -1,    /* 非法参数 */
    RDB_ERR_FLASH           =  -2,    /* Flash 读/写/擦除 I/O 错误 */
    RDB_ERR_NO_SPACE        =  -3,    /* 空间不足 */
    RDB_ERR_NOT_FOUND       =  -4,    /* 记录不存在 */
    RDB_ERR_TOO_LARGE       =  -5,    /* 单条记录超过容量限制 */
    RDB_ERR_CRC             =  -6,    /* 数据 CRC 校验失败 */
    RDB_ERR_CORRUPT         =  -7,    /* 扇区或记录损坏 */
    RDB_ERR_NOT_INIT        =  -8,    /* 数据库未初始化 */
    RDB_ERR_GC_FAIL         =  -9,    /* GC 失败 */
    RDB_ERR_ITER_END        = -10,    /* 迭代结束 */
    RDB_ERR_FULL            = -11,    /* 分区满，不可写 */
    RDB_ERR_BUSY            = -12,    /* 操作正在进行 */
    RDB_ERR_TIME_EXHAUSTED  = -13     /* TSDB 时间戳溢出 */
} rdb_err_t;
```

## KVDB 公共 API

### 生命周期

```c
rdb_err_t rdb_kvdb_init(
    rdb_kvdb_t *db,
    const rdb_partition_t *part,
    void *meta_buf
);

rdb_err_t rdb_kvdb_format(
    rdb_kvdb_t *db
);
```

| 函数 | 说明 | 关键参数 |
|------|------|---------|
| init | 初始化 KVDB，扫描所有扇区、恢复可用空间、选择活跃扇区 | meta_buf = 由 `rdb_kvdb_meta_size()` 返回大小的缓冲区 |
| format | 全盘擦除，erase_cnt 递增保留 | db 必须已 init |

### 基本操作

```c
rdb_err_t rdb_kvdb_set(
    rdb_kvdb_t *db,
    const char *key,
    const void *val,
    uint16_t len
);

rdb_err_t rdb_kvdb_get(
    rdb_kvdb_t *db,
    const char *key,
    void *buf,
    uint16_t buf_len,
    uint16_t *out_len
);

rdb_err_t rdb_kvdb_delete(
    rdb_kvdb_t *db,
    const char *key
);

int rdb_kvdb_exists(
    rdb_kvdb_t *db,
    const char *key
);
```

| 函数 | 返回 | 说明 |
|------|------|------|
| set | OK / TOO_LARGE / FULL / FLASH / GC_FAIL | 若空间不足自动触发 GC；单条记录 > DATA_CAP 校验失败 |
| get | OK / NOT_FOUND / TOO_LARGE / CRC | 若 buf 不足，out_len 仍返回实际长度，但不复制数据 |
| delete | OK / NOT_FOUND / FLASH | 将所有 VALID 副本标记为 DEAD |
| exists | RDB_TRUE(1) / RDB_FALSE(0) | 快速查询，不执行完整 CRC 校验 |

### 垃圾回收与空间管理

```c
rdb_err_t rdb_kvdb_gc(
    rdb_kvdb_t *db
);

void rdb_kvdb_space_info(
    rdb_kvdb_t *db,
    uint32_t *total,
    uint32_t *used,
    uint32_t *avail
);

void rdb_kvdb_wear_info(
    rdb_kvdb_t *db,
    uint32_t *min_ec,
    uint32_t *max_ec,
    uint32_t *avg_ec
);
```

| 函数 | 说明 |
|------|------|
| gc | 手动触发垃圾回收（通常自动执行） |
| space_info | 返回总容量、已用、可用空间（字节） |
| wear_info | 返回最小/最大/平均擦除次数（磨损管理统计） |

### 迭代

```c
rdb_err_t rdb_kv_iter_init(
    rdb_kv_iter_t *it,
    rdb_kvdb_t *db
);

rdb_err_t rdb_kv_iter_next(
    rdb_kv_iter_t *it,
    char *key_buf,
    uint16_t key_cap,
    void *val_buf,
    uint16_t val_cap,
    uint16_t *out_klen,
    uint16_t *out_vlen
);
```

| 函数 | 返回 | 说明 |
|------|------|------|
| iter_init | OK / PARAM | 初始化迭代器，注意：迭代期间禁止修改数据库 |
| iter_next | OK / ITER_END / CRC / BUSY | 遍历所有 VALID 记录（跳过 DEAD）；若数据库中途被修改返回 RDB_ERR_BUSY |

### 统计与诊断

```c
void rdb_kvdb_get_stats(
    rdb_kvdb_t *db,
    rdb_kv_stats_t *out
);

void rdb_kvdb_reset_stats(
    rdb_kvdb_t *db
);

size_t rdb_kvdb_meta_size(
    uint8_t sector_cnt
);
```

## TSDB 公共 API

### 生命周期

```c
rdb_err_t rdb_tsdb_init(
    rdb_tsdb_t *db,
    const rdb_partition_t *part,
    void *ec_buf
);

rdb_err_t rdb_tsdb_format(
    rdb_tsdb_t *db
);
```

| 函数 | 说明 | 关键参数 |
|------|------|---------|
| init | 初始化 TSDB，扫描环形分区、定位 head/tail | ec_buf = erase count 缓冲，大小由 `rdb_tsdb_ec_size()` 返回 |
| format | 全盘擦除，重置 head/tail/epoch | db 必须已 init |

### 基本操作

```c
rdb_err_t rdb_tsdb_append(
    rdb_tsdb_t *db,
    uint32_t ts,
    const void *data,
    uint16_t len
);

rdb_err_t rdb_tsdb_reset_epoch(
    rdb_tsdb_t *db
);

rdb_err_t rdb_tsdb_query(
    rdb_tsdb_t *db,
    uint32_t from_ts,
    uint32_t to_ts,
    rdb_ts_cb_t cb,
    void *arg
);

rdb_err_t rdb_tsdb_query_ex(
    rdb_tsdb_t *db,
    uint32_t from_ts,
    uint32_t to_ts,
    rdb_ts_cb_t cb,
    void *arg,
    void *read_buf,
    uint16_t buf_len
);

void rdb_tsdb_time_range(
    rdb_tsdb_t *db,
    uint32_t *oldest,
    uint32_t *newest
);
```

| 函数 | 返回 | 说明 |
|------|------|------|
| append | OK / TOO_LARGE / FULL / TIME_EXHAUSTED / FLASH | 自动轮转到下一扇区；时间戳回绕自动触发 epoch 重置 |
| reset_epoch | OK / FLASH | 强制开启新的时间 epoch |
| query | OK / PARAM / FLASH | 范围查询 [from_ts, to_ts]，通过回调返回匹配记录 |
| query_ex | OK / PARAM / FLASH | 与 query 相同，但使用调用者提供的读缓冲读取大 payload |
| time_range | 无 | 返回当前可查询的最旧/最新时间戳 |

### 统计与诊断

```c
void rdb_tsdb_get_stats(
    rdb_tsdb_t *db,
    rdb_ts_stats_t *out
);

void rdb_tsdb_reset_stats(
    rdb_tsdb_t *db
);

size_t rdb_tsdb_ec_size(
    uint8_t sector_cnt
);
```

## Flash 抽象层

```c
typedef struct {
    int  (*read)(uint32_t addr, uint8_t *buf, size_t len);
    int  (*write)(uint32_t addr, const uint8_t *buf, size_t len);
    int  (*erase)(uint32_t addr);
    void (*lock)(void);
    void (*unlock)(void);
    void (*yield)(void);
} rdb_flash_ops_t;

typedef struct {
    const char            *name;
    uint32_t               base_addr;
    uint32_t               total_size;
    uint32_t               sector_size;
    uint8_t                write_gran;    /* 0→1B, 1→2B, 2→4B, 3→8B */
    const rdb_flash_ops_t *ops;
} rdb_partition_t;
```

| 回调/字段 | 说明 | 约束 |
|----------|------|------|
| read | 读取 | 无特殊约束；可重入安全 |
| write | 写入 | 不跨页边界（256B）；写长度 ≤ write_gran 对齐 |
| erase | 擦除一个扇区 | 整扇区擦除（4KB）；最耗时操作 |
| lock / unlock | 互斥锁 | 可选（NULL 表示单线程）；禁止在 yield 中调用 RocketDB API |
| yield | 周期性喂狗 | 长操作（GC、rotation）期间定期调用；禁止再进入 RocketDB |

## 外部依赖（用户实现）

```c
extern uint16_t rdb_crc16(
    const void *data,
    size_t len
);

extern uint16_t rdb_crc16_cont(
    uint16_t crc,
    const void *data,
    size_t len
);

extern uint16_t rdb_hash16(
    const void *data,
    size_t len
);
```

| 函数 | 说明 |
|------|------|
| crc16 | CRC-16-MODBUS，支持流式计算初始化 |
| crc16_cont | CRC-16 增量计算（续接之前的结果） |
| hash16 | 16 位 hash，用于 KVDB key 快速拒绝 |

## 构建与运行期望

### 源文件结构

```
rocketdb.h           — 公共类型、常量、API 声明
rocketdb_kvdb.c      — KVDB 引擎实现
rocketdb_tsdb.c      — TSDB 引擎实现
```

### 编译要求

- C99 标准
- 无外部库（CRC16、hash16、Flash 操作由用户提供）
- 支持 GCC/Clang

### 测试期望

- 构建脚本：`build.bat`、`Makefile`
- 测试程序输出日志含时间戳及结果摘要
- 覆盖 KVDB/TSDB 的基本操作、GC、recovery 场景

## On-Flash 数据结构（来自 design.md 第三章）

### KVDB 扇区头（16 字节）

```c
#pragma pack(push, 1)
typedef struct {
    uint32_t magic;         /* offset 0   — 0x4B564442 ("KVDB") */
    uint16_t hdr_crc;       /* offset 4   — 头部 CRC16 */
    uint16_t _rsv1;         /* offset 6   — 0xFFFF */
    uint32_t create_seq;    /* offset 8   — 扇区创建序列号（4B 对齐）*/
    uint16_t erase_cnt;     /* offset 12  — 擦除计数 */
    uint16_t _rsv2;         /* offset 14  — 0xFFFF */
} rdb_kv_sector_hdr_t;     /* 16 bytes */
#pragma pack(pop)
```

### KVDB 记录头（16 字节）

```c
#pragma pack(push, 1)
typedef struct {
    uint8_t  magic;         /* offset 0   — 0xAA */
    uint8_t  state;         /* offset 1   — 0xFF(WRITING), 0xFE(VALID), 0xFC(DEAD) */
    uint8_t  key_len;       /* offset 2   — key 实际长度 */
    uint8_t  _pad1;         /* offset 3   — 0xFF */
    uint32_t seq;           /* offset 4   — 记录全局序列号，递增（4B 对齐）*/
    uint16_t val_len;       /* offset 8   — value 实际长度 */
    uint16_t data_crc;      /* offset 10  — CRC16(key ∥ val) */
} rdb_kv_record_hdr_t;     /* 16 bytes */
#pragma pack(pop)
```

**记录在 Flash 中的布局**：
```
│ record_hdr (16B) │ key (aligned) │ val (aligned) │
total_size = 16 + ALIGN_UP(key_len, WR_GRAN) + ALIGN_UP(val_len, WR_GRAN)
```

### TSDB 扇区头（20 字节）

```c
#pragma pack(push, 1)
typedef struct {
    uint32_t magic;         /* offset 0   — 0x54534442 ("TSDB") */
    uint16_t hdr_crc;       /* offset 4   — 头部 CRC16 */
    uint16_t seq;           /* offset 6   — 扇区序列号（16-bit） */
    uint32_t erase_cnt;     /* offset 8   — 擦除计数 */
    uint32_t time_base;     /* offset 12  — 第一个 record 的时间戳基准 */
    uint16_t count;         /* offset 16  — SEALED 后的记录数（0xFFFF = 未封存） */
    uint16_t end_off;       /* offset 18  — SEALED 后的数据末尾偏移（0xFFFF = 未封存） */
} rdb_ts_sector_hdr_t;     /* 20 bytes */
#pragma pack(pop)
```

### TSDB 记录头（12 字节）

```c
#pragma pack(push, 1)
typedef struct {
    uint8_t  magic;         /* offset 0   — 0xBB */
    uint8_t  state;         /* offset 1   — 0xFF(WRITING), 0xFE(VALID), 0xFC(DEAD) */
    uint16_t data_len;      /* offset 2   — data 实际长度 */
    uint32_t time_delta;    /* offset 4   — time - time_base（4B 对齐）*/
    uint16_t data_crc;      /* offset 8   — CRC16(data) */
} rdb_ts_record_hdr_t;     /* 12 bytes */
#pragma pack(pop)
```

**TSDB time_delta 设计要点**：
- 单扇区内时间跨度上限：0xFFFFFFFE
- 秒精度：约 136 年
- 毫秒精度：约 49.7 天
- 设计理由：rotation 由扇区容量驱动，不由时间差驱动

## 性能期望

| 场景 | 说明 |
|------|------|
| 读取 | O(1)，单扇区查找或哈希快速拒绝 |
| 写入 | O(1) 平摊（写入活跃扇区），偶发 O(N)（GC） |
| GC | 由垃圾率和扇区容量决定；定期调用 yield 避免长时间阻塞 |
| TSDB rotation | O(1)，自动切换到下一扇区 |

