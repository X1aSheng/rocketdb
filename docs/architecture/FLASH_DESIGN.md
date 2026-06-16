# Flash 芯片寿命评估模型

> RocketDB 驱动对 NOR Flash 芯片寿命影响的计算方法与实测数据

---

## 1. 核心公式

```
                        每个扇区可承受的 PE 周期 × 扇区数
Flash 寿命 (天数) = ───────────────────────────────────────
                      日均逻辑写入 × WAF × 单条记录扇区占比
```

### 1.1 符号定义

| 符号 | 含义 | 单位 |
|------|------|------|
| `PE_max` | 扇区最大擦写周期数 | 次 |
| `N_sec` | 分区扇区总数 | 个 |
| `S_sec` | 单个扇区容量 | 字节 |
| `R_avg` | 单条记录平均大小 (含 header+对齐) | 字节 |
| `W_day` | 日均逻辑写入次数 | 次/天 |
| `WAF` | 写放大因子 | 无单位 |
| `T_life` | flash 预期寿命 | 天 |

### 1.2 完整公式

```
T_life = PE_max × N_sec / (W_day × WAF × R_avg / S_sec)
```

简化形式：

```
T_life = PE_max × N_sec × S_sec / (W_day × WAF × R_avg)
```

---

## 2. WAF 分解模型

### 2.1 KVDB

```
WAF_kv = (P_small × 2 + P_large × (3 + ceil(V_avg / S_buf))) × (1 + GC_amp)
```

其中：
- `P_small` = 小记录占比 (记录总长 ≤ `RDB_STACK_BUF_SIZE`)
- `P_large` = 大记录占比 = 1 − P_small
- `V_avg` = 平均值大小 (字节)
- `S_buf` = `RDB_STACK_BUF_SIZE` (默认 64)
- `GC_amp` = GC 写放大系数 (通常 0.05~0.30)
- 常数 `2` = 合并写入 (1) + commit 字节 (1)
- 常数 `3` = header (1) + key (1) + commit (1)

### 2.2 TSDB

```
WAF_ts = P_small × 2 + P_large × (2 + ceil(D_avg × A_gran / S_buf))
```

其中：
- `P_small` = 小记录占比 (数据长 ≤ `RDB_STACK_BUF_SIZE` − TS_REC_SZ)
- `D_avg` = 平均数据长度 (字节)
- `A_gran` = 写入粒度对齐系数 (≥1.0)
- 常数 `2` = 合并写入 (1) + commit (1)

---

## 3. 合并缓冲区收益模型

### 3.1 操作数对比

| 记录类型 | 无合并写入 | 有合并写入 | 节省比 |
|----------|-----------|-----------|--------|
| KVDB 小记录 | 4 (header+key+val+commit) | 2 (merged+commit) | 2.0x |
| KVDB 大记录 | 3+N (header+key+N×chunk+commit) | 3+N | 1.0x |
| TSDB 小记录 | 3 (header+data+commit) | 2 (merged+commit) | 1.5x |
| TSDB 大记录 | 2+N (header+N×chunk+commit) | 2+N | 1.0x |

### 3.2 寿命延长比

```
L_ext = WAF_unopt / WAF_opt
      = (4 × P_small + (3+N_large) × P_large) / (2 × P_small + (3+N_large) × P_large)
```

当 P_small = 100% 时：L_ext = 4/2 = **2.0x** (KVDB), 3/2 = **1.5x** (TSDB)
当 P_small = 0% 时：L_ext = **1.0x** (无收益)

---

## 4. 实测数据

测试环境：`RDB_STACK_BUF_SIZE = 64`, W25QXX 仿真，SECTOR_SIZE = 4096

| 测试用例 | 逻辑操作 | Flash 写入 | WAF | 合并命中 | 均值大小 | 寿命提升 |
|----------|---------|-----------|-----|---------|---------|---------|
| kvdb_basic | 2,648 | 8,126 | 3.07 | 99% | 11 B | +30% |
| kvdb_stress | 4,184 | 23,267 | 5.56 | 65% | 131 B | +22% |
| tsdb_basic | 2,402 | 6,780 | 2.82 | 29% | 56 B | +17% |
| tsdb_stress | 2,719 | 18,256 | 6.71 | 21% | 260 B | +16% |
| integration | 18,731 | 87,150 | 4.65 | KV89%/TS21% | 74/134 B | +20% |

---

## 5. 典型场景寿命计算

假设：W25Q32 NOR Flash, PE_max = 100,000, 16 扇区 × 4096B

| 场景 | W_day | WAF | R_avg | T_life | 合并贡献 |
|------|-------|-----|-------|--------|---------|
| 传感器上报 (小数据, 99%命中) | 10,000 | 3.07 | 38 B | **47 年** | +11 年 |
| 混合负载 (中等数据, 65%命中) | 10,000 | 5.56 | 147 B | **18 年** | +3 年 |
| 日志记录 (大数据, 20%命中) | 1,000 | 6.71 | 280 B | **12 年** | +2 年 |
| 配置存储 (少量写入) | 100 | 3.07 | 38 B | **>100 年** | — |

---

## 6. 计算表格

配套 Excel 计算表见 `flash_lifespan_calc.csv`，包含：
- Sheet 1: 输入参数区
- Sheet 2: WAF 计算器
- Sheet 3: 实测数据对比
- Sheet 4: 场景寿命推算

导入方法：Excel → 数据 → 从文本/CSV → 选择 `flash_lifespan_calc.csv`
