# RocketDB 构建脚本使用指南

## 概述

RocketDB 提供三种构建方式：根目录 **Makefile**、根目录 **CMake/CTest**，以及 `build/` 下的 **批处理脚本**（Windows 专用）。

## 方式 1：Makefile（推荐）

根目录 `Makefile` 使用 **clang** 编译，支持 Windows/Linux/macOS。

### 快速开始

```bash
# 编译并运行全部 7 个测试
make test

# 仅编译不运行
make all

# 运行已编译的测试
make test

# 清理编译产物
make clean

# 查看帮助
make help
```

### 测试文件列表（7 个）

| 测试文件 | 内容 |
|----------|------|
| `test_kvdb_basic` | KVDB set/get/update/delete/write_gran/seq_wrap/mixed/corrupt |
| `test_kvdb_stress` | KVDB GC stress, iterator under GC, power-loss recovery |
| `test_tsdb_basic` | TSDB append/query/epoch/recount |
| `test_tsdb_stress` | TSDB rotation stress, append fail, CRC corruption, degraded |
| `test_integration` | KV+TS 联合测试：cycle stress, mixed workload, power-loss, wear |
| `test_example` | 示例/教程测试 |
| `test_fault_injection` | 故障注入演示 |

### 编译单个测试

```bash
# 编译单个测试
make test/out/test_kvdb_basic.exe

# 编译后手动运行
./test/out/test_kvdb_basic.exe
```

### 输出

所有日志输出到 `test/out/` 目录，格式为 `YYYYMMDD_HHMMSS_test_name.log`：
- `YYYYMMDD_HHMMSS_test_kvdb_basic.log`
- `YYYYMMDD_HHMMSS_test_kvdb_stress.log`
- `YYYYMMDD_HHMMSS_test_tsdb_basic.log`
- `YYYYMMDD_HHMMSS_test_tsdb_stress.log`
- `YYYYMMDD_HHMMSS_test_integration.log`
- `YYYYMMDD_HHMMSS_test_example.log`
- `YYYYMMDD_HHMMSS_test_fault_injection.log`

## 方式 2：批处理脚本（Windows）

`build/` 目录下提供了 Windows 批处理脚本，从项目根目录运行：

```bash
# 统一入口
build\build.bat all test          # 编译并运行全部测试
build\build.bat kvdb test         # 仅 KVDB 测试
build\build.bat tsdb test         # 仅 TSDB 测试

# 单独模块脚本
build\build_kvdb_all.bat          # KVDB 全部测试
build\build_tsdb_all.bat          # TSDB 全部测试
build\build_unified.bat           # 全量测试入口
build\run_all_tests.bat           # 编译并运行 7 套测试
build\build_perf.bat run          # 性能基准测试
```

## 方式 3：CMake/CTest

```bash
cmake -S . -B cmake-build -G Ninja -DCMAKE_C_COMPILER=D:/Programs/LLVM/bin/clang.exe
cmake --build cmake-build
ctest --test-dir cmake-build --output-on-failure
```

Windows Clang 构建会自动查找 `llvm-rc`/`rc` 资源编译器。若编译器安装在非标准位置，也可显式传入 `-DCMAKE_RC_COMPILER=<path-to-llvm-rc.exe>`。

## 编译要求

| 依赖 | 版本 | 说明 |
|------|------|------|
| Clang | ≥ 16.0 | 推荐编译器 |
| GCC | ≥ 12.0 | 备选编译器 |
| MSVC | ≥ 2019 | Windows 备选（clang-cl 兼容） |

编译标准：**C99**，无动态内存分配。

## 常见问题

### `make: command not found`

Windows 上可使用批处理脚本替代，或安装 MinGW/MSYS2：

```bash
# MSYS2 环境
pacman -S mingw-w64-x86_64-clang mingw-w64-x86_64-make
```

### `fatal error: 'rocketdb.h' file not found`

确保 include 路径包含 `src/` 和 `test/sim/`：
```bash
clang -Isrc -Itest/sim -std=c99 test/sim/test_kvdb_basic.c src/rocketdb_kvdb.c ...
```

### 编译警告 `-Wunused-but-set-variable`

C99 模式下 clang 可能报告此警告。添加 `-Wno-unused-but-set-variable` 可消除。

---

## 相关文档

- [测试框架说明](../test/sim/README.md)
- [故障注入文档](../test/sim/FAULT_INJECTION.md)
- [设计文档](Architecture.md)

---

**最后更新**: 2026-04-29
**版本**: RocketDB v1.1.2
