# RocketDB 离线 Flash 分区分析

嵌入式设备可将完整 Flash 分区按原始字节读出，上位机或服务器端使用 `tools/rdbdump` 解析、校验并导出可观察数据集。该流程用于现场问题定位、量产抽检、长期耐久数据分析，以及测试模拟器输出的交叉验证。

## 输入数据

离线分析需要两类输入：

| 输入 | 说明 |
|------|------|
| 原始镜像 | 完整 KVDB 或 TSDB 分区 dump，例如 `rdbdump_kvdb.bin` |
| manifest | 分区几何和类型，例如 `kind`、`sector_size`、`write_gran`、`base_addr`、`total_size` |

manifest 示例：

```json
{
  "kind": "kvdb",
  "input": "rdbdump_kvdb.bin",
  "sector_size": 4096,
  "write_gran": 0,
  "base_addr": 0,
  "total_size": 32768
}
```

## 命令形态

```bash
python tools/rdbdump/rdbdump.py inspect --manifest tests/out/rdbdump_kvdb.json
python tools/rdbdump/rdbdump.py verify --strict --manifest tests/out/rdbdump_tsdb.json
python tools/rdbdump/rdbdump.py export --manifest tests/out/rdbdump_kvdb.json --out tests/out/rdbdump_export/<YYMMDD-HHMMSS>/kvdb
```

通用参数：

| 参数 | 用途 |
|------|------|
| `--input` | 原始 Flash 分区镜像 |
| `--manifest` | 分区描述文件 |
| `--kind {auto,kvdb,tsdb}` | 自动识别或强制指定引擎 |
| `--sector-size` | 扇区大小 |
| `--write-gran` | 写入粒度，0/1/2/3 对应 1/2/4/8 字节 |
| `--base-addr` | 分区基地址，用于报告绝对地址 |
| `--total-size` | 分区总大小 |

## 输出分层

`export` 首先将原始扇区解析为可观察格式数据集，然后再生成有效数据集。

| 文件 | 内容 |
|------|------|
| `observable_dataset.json/csv` | 所有可观察对象：扇区头、记录、擦除尾部、扫描异常、原始 hex 前缀 |
| `valid_dataset.json/csv` | 已通过逻辑过滤的数据：KVDB 当前有效 key-value，TSDB 有效且 CRC 正确的记录 |
| `summary.json` | 类型、扇区数、记录数、有效数据量、异常数量 |
| `anomalies.json` | 扇区头异常、记录头异常、CRC 错误等 |
| `kv_current.json` / `kv_records.csv` | KVDB 兼容视图 |
| `ts_records.csv` | TSDB 兼容视图 |

集成测试会把导出结果写入带时间戳的目录：

```text
tests/out/rdbdump_export/<YYMMDD-HHMMSS>/kvdb
tests/out/rdbdump_export/<YYMMDD-HHMMSS>/tsdb
```

## 测试集成

`tests/sim/test_rdbdump_dump.c` 使用真实 RocketDB API 在模拟 Flash 上生成确定性 KVDB/TSDB 分区镜像。`build/run_all_tests.bat test` 在 8 个基础测试全部运行后执行：

1. 编译并运行 dump 生成器。
2. 用 `rdbdump verify --strict` 校验 KVDB/TSDB dump。
3. 用 `rdbdump export` 导出 KVDB/TSDB 数据集。

CMake/CTest 中注册 `rdbdump_offline_verify`，并依赖基础测试与示例 smoke test，确保离线解析验证作为最后阶段运行。
