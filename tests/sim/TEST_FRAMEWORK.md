# 测试框架使用指南

## 概述

RocketDB 测试框架提供了一套完整的测试工具，包括：

- **测试用例注册与自动发现**：无需手动维护测试列表
- **丰富的断言宏**：覆盖各种测试场景
- **参数化测试**：使用不同参数组合运行同一测试
- **统计收集**：自动收集测试时间、断言数等
- **灵活配置**：支持详细模式、测试过滤、失败即停等

## 快速开始

### 1. 基础测试用例

```c
#include "test_framework.h"

TEST_CASE(my_test_case, "Category", "Test description")
{
    // 测试代码
    TEST_ASSERT_EQ(1 + 1, 2);
    return 0;
}
```

### 2. 注册测试用例

有两种方式注册测试用例：

**方法 A：自动注册（推荐）**
```c
TEST_CASE(my_test, "KVDB", "Test set/get") { ... }
REGISTER_TEST(my_test);  // 使用构造函数自动注册
```

**方法 B：手动注册**
```c
TEST_CASE(my_test, "KVDB", "Test set/get") { ... }

int main() {
    test_suite_t *suite = test_get_default_suite();
    test_register_case(suite, &test_case_my_test);
    ...
}
```

### 3. 运行测试

```c
int main(void)
{
    // 配置测试框架
    test_config_t config = {
        .log_file = fopen("test.log", "w"),
        .verbose = 1,
        .stop_on_fail = 0,
        .filter = NULL  // 或 "KVDB" 只运行 KVDB 测试
    };
    
    test_framework_init(&config);
    
    // 运行所有测试
    test_run_all(&my_context);
    
    // 打印报告
    test_print_report();
    
    // 清理
    if (config.log_file) fclose(config.log_file);
    
    return 0;
}
```

## 断言宏参考

### 基础断言

| 宏 | 说明 | 示例 |
|---|------|------|
| `TEST_ASSERT(cond)` | 断言条件为真 | `TEST_ASSERT(x > 0)` |
| `TEST_ASSERT_MSG(cond, msg)` | 带消息的断言 | `TEST_ASSERT_MSG(x > 0, "x must be positive")` |

### 整数比较

| 宏 | 说明 | 示例 |
|---|------|------|
| `TEST_ASSERT_EQ(a, b)` | a == b | `TEST_ASSERT_EQ(count, 10)` |
| `TEST_ASSERT_NE(a, b)` | a != b | `TEST_ASSERT_NE(ret, 0)` |
| `TEST_ASSERT_LT(a, b)` | a < b | `TEST_ASSERT_LT(val, 100)` |
| `TEST_ASSERT_LE(a, b)` | a <= b | `TEST_ASSERT_LE(val, 100)` |
| `TEST_ASSERT_GT(a, b)` | a > b | `TEST_ASSERT_GT(val, 0)` |
| `TEST_ASSERT_GE(a, b)` | a >= b | `TEST_ASSERT_GE(val, 0)` |

### 指针断言

| 宏 | 说明 | 示例 |
|---|------|------|
| `TEST_ASSERT_NULL(ptr)` | ptr == NULL | `TEST_ASSERT_NULL(ptr)` |
| `TEST_ASSERT_NOT_NULL(ptr)` | ptr != NULL | `TEST_ASSERT_NOT_NULL(buf)` |

### 内存与字符串

| 宏 | 说明 | 示例 |
|---|------|------|
| `TEST_ASSERT_MEM_EQ(a, b, len)` | 内存块相等 | `TEST_ASSERT_MEM_EQ(buf1, buf2, 64)` |
| `TEST_ASSERT_STR_EQ(a, b)` | 字符串相等 | `TEST_ASSERT_STR_EQ(str, "hello")` |

### RocketDB 专用

| 宏 | 说明 | 示例 |
|---|------|------|
| `TEST_ASSERT_RDB_OK(expr)` | 返回 RDB_OK | `TEST_ASSERT_RDB_OK(rdb_kvdb_set(...))` |
| `TEST_ASSERT_RDB_ERR(expr, err)` | 返回指定错误 | `TEST_ASSERT_RDB_ERR(ret, RDB_ERR_NOT_FOUND)` |

## 参数化测试

参数化测试允许使用不同参数运行同一测试逻辑。

### 定义参数化测试函数

```c
static int my_param_test(void *ctx, const test_params_t *params)
{
    // 使用 params 中的参数
    uint8_t gran = params->write_granularity;
    uint32_t sec_size = params->sector_size;
    
    TEST_ASSERT_GE(sec_size, 4096);
    
    // 执行测试...
    
    return 0;
}
```

### 运行参数化测试

```c
test_params_t params[] = {
    {.write_granularity = 0, .sector_size = 4096, .sector_count = 16, .max_record_size = 256},
    {.write_granularity = 1, .sector_size = 4096, .sector_count = 16, .max_record_size = 512},
    {.write_granularity = 2, .sector_size = 8192, .sector_count = 32, .max_record_size = 1024},
};

test_run_parameterized("my_param_test", my_param_test, &env, params, 3);
```

## 测试配置选项

```c
typedef struct {
    FILE       *log_file;       // 日志文件（NULL=不记录文件）
    int         verbose;        // 详细模式：0=简洁，1=正常，2=最详细
    int         stop_on_fail;   // 遇到失败即停止：0=继续，1=停止
    const char *filter;         // 测试名称过滤器（NULL=全部）
} test_config_t;
```

### 示例：只运行 KVDB 测试

```c
test_config_t config = {
    .log_file = NULL,
    .verbose = 1,
    .stop_on_fail = 0,
    .filter = "KVDB"  // 只运行名称包含 "KVDB" 的测试
};
```

### 示例：遇到失败即停止

```c
test_config_t config = {
    .log_file = fopen("test.log", "w"),
    .verbose = 2,  // 最详细模式
    .stop_on_fail = 1,  // 遇到失败立即停止
    .filter = NULL
};
```

## 测试报告

测试完成后会生成详细报告：

```
========================================
Test Report
========================================

Test Cases:
  Total:   15
  Passed:  14
  Failed:  1
  Skipped: 0

Assertions:
  Total:   93
  Passed:  92
  Failed:  1

Time:
  Elapsed: 245 ms

✗ SOME TESTS FAILED
========================================
```

## 最佳实践

### 1. 测试用例命名

使用清晰的命名约定：
- `test_<component>_<operation>` - 如 `test_kvdb_set_get`
- 使用分类标签（category）进行分组

### 2. 测试组织

将相关测试放在同一个文件中：
```
tests/sim/
├── test_kvdb_basic.c      - KVDB 基础测试
├── test_kvdb_gc.c         - KVDB GC 测试
├── test_tsdb_basic.c      - TSDB 基础测试
└── test_framework.c       - 测试框架实现
```

### 3. 使用上下文传递环境

```c
typedef struct {
    sim_flash_t *flash;
    rdb_kvdb_t  *kvdb;
    rdb_tsdb_t  *tsdb;
} test_env_t;

TEST_CASE(my_test, "KVDB", "Description")
{
    test_env_t *env = (test_env_t *)ctx;
    rdb_kvdb_t *db = env->kvdb;
    
    // 使用 db 进行测试...
    
    return 0;
}
```

### 4. 清理和隔离

每个测试用例应该：
- 不依赖其他测试的执行顺序
- 清理自己创建的数据
- 使用唯一的键名避免冲突

```c
TEST_CASE(my_test, "KVDB", "Description")
{
    const char *key = "my_test_key_12345";  // 唯一键名
    
    // ... 测试代码 ...
    
    // 清理
    rdb_kvdb_delete(db, key);
    
    return 0;
}
```

## 编译测试

### 使用 build.bat

```bat
# 编译测试框架示例
clang -std=c99 -Wall -Wextra -O2 ^
    -I. -Itest/sim ^
    -o tests/out/test_example.exe ^
    tests/sim/test_example.c ^
    tests/sim/test_framework.c ^
    tests/sim/sim_flash.c ^
    tests/sim/sim_crypto.c ^
    rocketdb_kvdb.c ^
    rocketdb_tsdb.c
```

### 使用 Makefile

```makefile
test_example: tests/sim/test_example.c tests/sim/test_framework.c
	$(CC) $(CFLAGS) -o tests/out/test_example.exe $^ $(OBJS)
```

## 迁移现有测试

如果你有现有的测试代码（如 test_comprehensive.c），可以逐步迁移到新框架：

### 原有代码：
```c
static void test_assert(int cond, const char *name) {
    if (cond) {
        printf(".");
    } else {
        printf("F");
    }
}

void test_kv_set_get(rdb_kvdb_t *db) {
    test_assert(rdb_kvdb_set(db, "k", "v", 1) == RDB_OK, "set");
    test_assert(rdb_kvdb_get(db, "k", buf, 64, &len) == RDB_OK, "get");
}
```

### 迁移后：
```c
TEST_CASE(test_kv_set_get, "KVDB", "Set and get operations")
{
    test_env_t *env = (test_env_t *)ctx;
    rdb_kvdb_t *db = env->kvdb;
    uint8_t buf[64];
    uint16_t len;
    
    TEST_ASSERT_RDB_OK(rdb_kvdb_set(db, "k", "v", 1));
    TEST_ASSERT_RDB_OK(rdb_kvdb_get(db, "k", buf, 64, &len));
    
    return 0;
}
REGISTER_TEST(test_kv_set_get);
```

## 参考

- [test_example.c](test_example.c) - 完整示例
- [test_framework.h](test_framework.h) - API 参考
- [test_framework.c](test_framework.c) - 实现细节
