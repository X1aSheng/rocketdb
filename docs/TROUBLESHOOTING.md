# RocketDB 故障排查指南

**版本**: v1.1.0  
**最后更新**: 2026-04-29  
**适用对象**: 开发者、集成工程师、测试人员

---

## 📋 快速目录

- [编译问题](#编译问题) - 编译失败、链接错误、警告处理
- [运行问题](#运行问题) - 测试失败、输出异常、性能问题
- [功能问题](#功能问题) - API错误、数据丢失、状态异常
- [集成问题](#集成问题) - MCU移植、多任务、Flash适配
- [调试技巧](#调试技巧) - 日志分析、工具使用、定位方法

---

## 编译问题

### Q1: 编译器找不到 (clang/gcc not found)

**症状**:
```
'clang' 不是内部或外部命令，也不是可运行的程序
或批处理文件。
```

**原因**: clang 未安装或未添加到系统 PATH

**解决方案**:

#### Windows
```bash
# 方法1: 使用 Chocolatey
choco install llvm

# 方法2: 手动安装
# 1. 下载 LLVM: https://releases.llvm.org/download.html
# 2. 安装时勾选 "Add LLVM to the system PATH"
# 3. 重启命令行

# 验证安装
clang --version
```

#### Linux (Ubuntu/Debian)
```bash
sudo apt-get update
sudo apt-get install clang

# 验证
clang --version
```

#### macOS
```bash
# 使用 Homebrew
brew install llvm

# 添加到 PATH (如果需要)
export PATH="/usr/local/opt/llvm/bin:$PATH"

# 验证
clang --version
```

**推荐版本**: clang 14+ 或 gcc 9+

---

### Q2: 编译警告 "unused parameter"

**症状**:
```
rocketdb_kvdb.c:123:45: warning: unused parameter 'src_sec' [-Wunused-parameter]
```

**原因**: 函数参数未使用（已在 v0.0.2 修复）

**解决方案**:

✅ **v0.0.2 已解决**: 所有 unused-parameter 警告已通过 `(void)param` 抑制

如果仍然出现:
1. 确认代码版本是最新的
2. 检查是否使用了正确的编译选项: `-Wall -Wextra`
3. 报告 GitHub Issue

**预期结果**: 0 warning (所有警告已清除)

---

### Q3: 链接失败 "undefined reference"

**症状**:
```
undefined reference to `sim_fault_inject'
undefined reference to `sim_dist_key_len'
```

**原因**: 缺少源文件或包含路径错误

**解决方案**:

#### 检查1: 确认所有源文件都包含在编译命令中

```bash
# build.bat 中应包含:
set SRCS=..\..\rocketdb_kvdb.c ..\..\rocketdb_tsdb.c ^
         sim_flash.c sim_fault.c sim_dist.c sim_vectors.c sim_crypto.c

# 检查文件是否存在
dir test\sim\sim_*.c
```

#### 检查2: 验证包含路径

```bash
# 编译选项应包含:
-I. -I..\..\  -Itest\sim

# 检查 test/sim/rocketdb.h 是否正确指向 ../../rocketdb.h
```

#### 检查3: 清理并重新编译

```bash
build.bat all clean
build.bat all build
```

**常见错误**:
- ❌ 忘记添加 `sim_fault.c` (T-204 新增)
- ❌ 忘记添加 `sim_dist.c` (T-205 新增)
- ❌ `#include` 路径错误

---

### Q4: 编译速度慢

**症状**: 每次编译都重新编译所有文件

**原因**: 使用批处理脚本而非增量编译

**解决方案**:

#### Windows: 使用 nmake (如果有 MSVC)
```bash
# 需要配置 nmake，复杂度较高
# 推荐使用 build.bat (简单但非增量)
```

#### Linux/macOS: 使用 Makefile
```bash
make              # 增量编译
make clean        # 清理
make test         # 编译并运行
```

**Makefile 优势**:
- ✅ 增量编译：只重新编译修改的文件
- ✅ 并行编译：`make -j4` 使用4个核心
- ✅ 依赖追踪：自动检测头文件变化

---

### Q5: 输出目录权限拒绝

**症状**:
```
拒绝访问。
无法创建 test\out\test_kv_basic.exe
```

**原因**: 
1. 输出目录不存在
2. 权限不足
3. 文件被占用（正在运行）

**解决方案**:

```bash
# 1. 手动创建输出目录
mkdir test\out          # Windows
mkdir -p test/out       # Linux/Mac

# 2. 以管理员权限运行（Windows）
# 右键 CMD/PowerShell -> "以管理员身份运行"

# 3. 关闭正在运行的测试程序
taskkill /F /IM test_kv_basic.exe    # Windows
killall test_kv_basic                # Linux/Mac

# 4. 检查目录权限
icacls test\out         # Windows
ls -la test/out         # Linux/Mac
```

---

## 运行问题

### Q6: 测试输出为空或无输出

**症状**: 
- 命令执行后没有任何输出
- 日志文件为空或不存在

**诊断步骤**:

#### 步骤1: 确认测试已编译
```bash
# 检查可执行文件
dir test\out\test_kv_basic.exe       # Windows
ls -l test/out/test_kv_basic         # Linux

# 如果不存在，先编译
build.bat kvdb build
```

#### 步骤2: 手动运行测试
```bash
# 直接运行可执行文件
cd test\out
.\test_kv_basic.exe          # Windows
./test_kv_basic              # Linux

# 应该看到测试输出
```

#### 步骤3: 检查日志文件
```bash
# 日志命名格式: test_<name>_log_YYYYMMDD_HHMMSS.log
dir test\out\test_kv_basic_log_*.log

# 查看最新日志
type test\out\test_kv_basic_log_*.log | more     # Windows
cat test/out/test_kv_basic_log_*.log             # Linux

# 预期末尾显示
✓ ALL TESTS PASSED
```

#### 步骤4: 检查编码问题（中文乱码）
```bash
# Windows PowerShell 编码问题
# 使用 CMD 而非 PowerShell
cmd /c "build.bat kvdb test"

# 或设置编码
chcp 65001   # UTF-8
chcp 936     # GBK (中文)
```

---

### Q7: 特定测试失败

**症状**:
```
[FAIL] kv_set_get_basic (assertion failed at line 123)
Expected: RDB_OK, Got: RDB_ERR_FULL
```

**诊断方法**:

#### 方法1: 单独运行该测试
```bash
# 运行单个测试以隔离问题
build_kvdb_all.bat test basic          # KVDB basic
build_tsdb_all.bat test epoch          # TSDB epoch

# 查看详细日志
type test\out\test_kv_basic_log_*.log
```

#### 方法2: 启用详细日志
```c
// 编辑 test/sim/test_framework.h
#define DEBUG 1              // 启用调试输出
#define VERBOSE 1            // 启用详细日志

// 重新编译
build.bat kvdb rebuild
```

#### 方法3: 检查 Flash 模拟器状态
```c
// 在测试中添加诊断代码
void test_debug() {
    printf("Flash size: %u\n", flash.size);
    printf("Sector count: %u\n", flash.sector_count);
    
    // 检查 Flash 状态
    for (int i = 0; i < flash.sector_count; i++) {
        printf("Sector %d: erase_cnt=%u\n", 
               i, flash.sectors[i].erase_count);
    }
}
```

**常见失败原因**:
- ❌ Flash 容量不足 → 检查 `RDB_PARTITION_SIZE`
- ❌ GC 未触发 → 检查 `gc_reserve` 配置
- ❌ 状态未重置 → 每个测试前调用 `format()`

---

### Q8: 性能异常 (测试运行时间过长)

**症状**: 测试卡住或运行时间超过预期

**诊断**:

#### 检查1: GC 是否陷入死循环
```c
// 查看日志中的 GC 运行次数
// 正常: T-301 GC 压力测试应在 10-30 秒内完成 200 次 GC

// 如果超过 60 秒，可能是:
// 1. Flash 擦除极慢（检查 sim_flash.c 的 erase 延迟）
// 2. GC 死锁（不应该发生，报告 bug）
// 3. 无限循环（代码逻辑错误）
```

#### 检查2: yield 回调是否生效
```c
// 编辑 test/sim/sim_flash.c
static void test_yield(void) {
    printf("yield called\n");  // 调试输出
    // 如果从未打印，说明 GC 未调用 yield
}
```

#### 检查3: CPU 占用率
```bash
# Windows
taskmgr     # 打开任务管理器，查看 CPU 占用

# Linux
top         # 查看进程 CPU 占用
htop        # 更友好的版本
```

**预期性能** (参考):
- test_kv_basic: < 1 秒
- test_kv_gc_stress (200 GC): 10-30 秒
- test_ts_rotation_stress (200 次): 10-30 秒
- test_mixed_workload (10K ops): 5-15 秒

---

### Q9: 日志文件命名混乱

**症状**: 多个日志文件，不知道哪个是最新的

**解决方案**:

#### 按时间排序查找最新日志
```bash
# Windows PowerShell
Get-ChildItem test\out\test_kv_basic_log_*.log | 
    Sort-Object LastWriteTime -Descending | 
    Select-Object -First 1

# Linux
ls -lt test/out/test_kv_basic_log_*.log | head -1
```

#### 使用符号链接（推荐）
```bash
# Linux/Mac: 创建符号链接指向最新日志
ln -sf test/out/test_kv_basic_log_$(date +%Y%m%d_%H%M%S).log \
       test/out/latest_kv_basic.log

# 查看最新日志
cat test/out/latest_kv_basic.log
```

#### 定期清理旧日志
```bash
# Windows: 删除 7 天前的日志
forfiles /p test\out /m test_*_log_*.log /d -7 /c "cmd /c del @path"

# Linux: 删除 7 天前的日志
find test/out -name "test_*_log_*.log" -mtime +7 -delete
```

---

## 功能问题

### Q10: init() 返回 RDB_ERR_CORRUPT

**症状**:
```c
rdb_err_t err = rdb_kvdb_init(&db, &flash_ops);
// err == RDB_ERR_CORRUPT
```

**原因**:
1. Flash 数据损坏
2. 格式版本不匹配 (v0.0.1 vs v0.0.2)
3. CRC 校验失败

**解决方案**:

#### 方案1: 格式化后重新初始化
```c
// 先格式化（清空所有数据）
rdb_kvdb_format(&db);

// 再初始化
rdb_err_t err = rdb_kvdb_init(&db, &flash_ops);
assert(err == RDB_OK);
```

#### 方案2: 检查 Flash 擦除是否成功
```c
// 在 hw_flash_erase() 中添加验证
int hw_flash_erase(uint32_t addr, uint32_t len) {
    // 擦除操作
    HAL_FLASHEx_Erase(&init, &SectorError);
    
    // 验证擦除结果
    uint8_t buf[256];
    hw_flash_read(addr, buf, sizeof(buf));
    for (int i = 0; i < sizeof(buf); i++) {
        if (buf[i] != 0xFF) {
            printf("Erase failed at offset %d: 0x%02X\n", i, buf[i]);
            return RDB_ERR;
        }
    }
    
    return RDB_OK;
}
```

#### 方案3: 查看 recovery 日志
```c
// init() 内部会打印 recovery 信息（如果启用了日志）
// 关注以下输出:
// - "KVDB Phase 1: Found N sectors"
// - "KVDB Phase 2: Recovered M records"
// - "KVDB Phase 3: Selected active sector X"
// - "KVDB Phase 4: GC reserve OK"
```

**注意**: v0.0.2 格式与 v0.0.1 **不兼容**，升级需重新格式化

---

### Q11: set() 返回 RDB_ERR_FULL

**症状**:
```c
rdb_err_t err = rdb_kvdb_set(&db, "key", "value", 5);
// err == RDB_ERR_FULL
```

**原因**:
1. 所有扇区已满
2. GC 未触发或失败
3. value 长度超过单扇区容量

**诊断**:

#### 检查1: 查看空间统计
```c
struct rdb_kv_info info;
rdb_kvdb_space_info(&db, &info);

printf("Total: %u, Used: %u, Avail: %u\n",
       info.total_bytes, info.used_bytes, info.avail_bytes);
printf("GC runs: %u\n", info.gc_runs);

// 如果 avail_bytes < 需要的空间，说明真的没空间了
// 如果 gc_runs == 0，说明 GC 从未触发
```

#### 检查2: 手动触发 GC（调试用）
```c
// 注意: RocketDB 没有公开的 GC API
// GC 会在 set/delete 时自动触发

// 解决方法1: 删除一些旧数据
rdb_kvdb_delete(&db, "old_key_1");
rdb_kvdb_delete(&db, "old_key_2");

// 再尝试 set
err = rdb_kvdb_set(&db, "new_key", "value", 5);
```

#### 检查3: 验证 GC 保留扇区配置
```c
// gc_reserve 计算逻辑 (见 rocketdb_kvdb.c)
// gc_reserve = (sector_cnt >= 16) ? 2 : 1;

// 如果 sector_cnt = 4, gc_reserve = 1
// 可用扇区 = 4 - 1 - 1(active) = 2
// 如果 2 个扇区都满了，就会 ERR_NO_SPACE

// 解决: 增加分区大小或减少数据量
```

**常见错误**:
- ❌ value 长度 > 4KB (超过单扇区容量)
- ❌ 分区太小 (只有 2-3 个扇区)
- ❌ 大量碎片化数据（需要更多 GC 保留空间）

---

### Q12: query() 返回 0 结果

**症状**: TSDB 查询返回 0 条记录，但数据确实存在

**诊断**:

#### 检查1: 验证时间范围
```c
// 查看实际数据的时间范围
uint32_t oldest, newest;
rdb_tsdb_time_range(&db, &oldest, &newest);
printf("Data range: %u ~ %u\n", oldest, newest);

// 确认查询范围是否覆盖数据
rdb_tsdb_query(&db, 1000, 2000, callback, arg);
// 如果 oldest=3000, newest=5000, 查询 [1000,2000] 当然返回 0
```

#### 检查2: Epoch 是否匹配
```c
// 如果发生时间戳倒序，epoch 会自动递增
// 旧 epoch 的数据对新 epoch 不可见

// 查看当前 epoch
printf("Current epoch: %u\n", db.env_current_epoch);

// 如果 epoch 发生变化，旧数据不会在 query 中返回
// 这是设计行为，防止时间戳混乱
```

#### 检查3: 回调函数是否正确
```c
// 回调函数必须返回 0 继续，返回 非0 终止
int my_callback(uint32_t ts, const void *data, uint16_t len, void *arg) {
    printf("Record: ts=%u, len=%u\n", ts, len);
    return 0;  // ← 必须返回 0！
}

// 错误示例: 返回 1 会立即终止查询
```

---

### Q13: 迭代器返回 RDB_ERR_BUSY

**症状**:
```c
rdb_kv_iter_t it;
rdb_kv_iter_init(&it, &db);

// 中途修改数据库
rdb_kvdb_set(&db, "new_key", "value", 5);

// 继续迭代
err = rdb_kv_iter_next(&it, ...);
// err == RDB_ERR_BUSY
```

**原因**: 迭代期间数据库被修改，iter_gen 不匹配

**解决方案**:

#### 方案1: 完成迭代后再修改
```c
// ✅ 正确做法
rdb_kv_iter_t it;
rdb_kv_iter_init(&it, &db);

while (rdb_kv_iter_next(&it, ...) == RDB_OK) {
    // 处理数据（只读）
}

// 迭代完成后再修改
rdb_kvdb_set(&db, "key", "value", 5);
```

#### 方案2: 重新初始化迭代器
```c
// 如果必须在迭代中修改
rdb_kv_iter_init(&it, &db);

while (rdb_kv_iter_next(&it, ...) == RDB_OK) {
    if (需要修改) {
        // 修改数据库
        rdb_kvdb_set(&db, "key", "value", 5);
        
        // 重新初始化迭代器
        rdb_kv_iter_init(&it, &db);
        // ⚠️ 会从头开始迭代
    }
}
```

**设计原因**: 
- 防止迭代期间数据不一致
- 嵌入式系统通常单任务，不需要复杂的快照隔离
- 如需并发修改，可在 v1.0.1 考虑支持

---

## 集成问题

### Q14: 如何在 STM32 上集成？

**完整步骤**:

#### 步骤1: 准备 Flash HAL 实现

创建 `hw_flash.c`:

```c
#include "stm32f4xx_hal.h"
#include "rocketdb.h"

// Flash 基地址（根据实际芯片调整）
#define FLASH_USER_START_ADDR   0x08010000   // 扇区4开始
#define FLASH_USER_END_ADDR     0x080FFFFF   // 960KB

// 初始化
static int hw_flash_init(void) {
    HAL_FLASH_Unlock();
    return RDB_OK;
}

// 读取
static int hw_flash_read(uint32_t addr, void* buf, uint32_t len) {
    memcpy(buf, (const void*)(FLASH_USER_START_ADDR + addr), len);
    return RDB_OK;
}

// 写入（STM32F4 支持双字写入）
static int hw_flash_write(uint32_t addr, const void* data, uint32_t len) {
    uint32_t flash_addr = FLASH_USER_START_ADDR + addr;
    const uint64_t *p_data = (const uint64_t*)data;
    
    for (uint32_t i = 0; i < len; i += 8) {
        if (HAL_FLASH_Program(FLASH_TYPEPROGRAM_DOUBLEWORD, 
                              flash_addr + i, *p_data++) != HAL_OK) {
            return RDB_ERR;
        }
    }
    
    return RDB_OK;
}

// 擦除
static int hw_flash_erase(uint32_t addr, uint32_t len) {
    FLASH_EraseInitTypeDef EraseInitStruct;
    uint32_t SectorError = 0;
    
    // STM32F407 扇区大小不统一，需要映射
    // 扇区0-3: 16KB, 扇区4: 64KB, 扇区5-11: 128KB
    // 这里假设用扇区5-11 (128KB * 7 = 896KB)
    
    EraseInitStruct.TypeErase = FLASH_TYPEERASE_SECTORS;
    EraseInitStruct.VoltageRange = FLASH_VOLTAGE_RANGE_3;
    EraseInitStruct.Sector = GetSectorFromAddr(addr);
    EraseInitStruct.NbSectors = 1;
    
    if (HAL_FLASHEx_Erase(&EraseInitStruct, &SectorError) != HAL_OK) {
        return RDB_ERR;
    }
    
    return RDB_OK;
}

// CRC16 计算（使用STM32硬件CRC32，截取低16位）
static uint16_t hw_crc16(const void* data, uint32_t len) {
    uint32_t crc32 = HAL_CRC_Calculate(&hcrc, (uint32_t*)data, len / 4);
    return (uint16_t)(crc32 & 0xFFFF);
}

// Yield 回调（FreeRTOS）
static void hw_yield(void) {
    taskYIELD();  // 让出 CPU 给高优先级任务
}

// 导出操作表
const struct rdb_flash_ops hw_flash_ops = {
    .init  = hw_flash_init,
    .read  = hw_flash_read,
    .write = hw_flash_write,
    .erase = hw_flash_erase,
    .crc16 = hw_crc16,
    .yield = hw_yield,
};
```

#### 步骤2: 初始化数据库

```c
#include "rocketdb.h"

// 全局变量
static rdb_kvdb_t g_kvdb;
static rdb_tsdb_t g_tsdb;

void app_db_init(void) {
    // 初始化 KVDB
    rdb_err_t err = rdb_kvdb_init(&g_kvdb, &hw_flash_ops);
    if (err == RDB_ERR_CORRUPT) {
        // 首次使用或数据损坏，需格式化
        rdb_kvdb_format(&g_kvdb);
        err = rdb_kvdb_init(&g_kvdb, &hw_flash_ops);
    }
    
    if (err != RDB_OK) {
        Error_Handler();  // 初始化失败
    }
    
    // 初始化 TSDB
    err = rdb_tsdb_init(&g_tsdb, &hw_flash_ops);
    if (err == RDB_ERR_CORRUPT) {
        rdb_tsdb_format(&g_tsdb);
        err = rdb_tsdb_init(&g_tsdb, &hw_flash_ops);
    }
    
    if (err != RDB_OK) {
        Error_Handler();
    }
}
```

#### 步骤3: 使用数据库

```c
void app_save_config(const char *key, const char *value) {
    rdb_kvdb_set(&g_kvdb, key, value, strlen(value));
}

void app_load_config(const char *key, char *buf, uint16_t buf_size) {
    uint16_t len;
    rdb_kvdb_get(&g_kvdb, key, buf, buf_size, &len);
    buf[len] = '\0';  // 添加字符串终止符
}

void app_log_sensor(uint32_t timestamp, float temperature) {
    rdb_tsdb_append(&g_tsdb, timestamp, &temperature, sizeof(temperature));
}
```

**完整集成指南** 👉 [docs/HAL_REFERENCE.md](HAL_REFERENCE.md)

---

### Q15: 多任务环境下如何使用？

**场景**: FreeRTOS/CMSIS-RTOS 多任务环境

**问题**: 
- 任务A和任务B同时访问数据库
- 可能导致数据竞争

**解决方案**:

#### 方案1: 使用互斥量保护（推荐）
```c
#include "cmsis_os.h"

static osMutexId_t db_mutex;
static rdb_kvdb_t g_kvdb;

void app_db_init(void) {
    // 创建互斥量
    db_mutex = osMutexNew(NULL);
    
    // 初始化数据库
    rdb_kvdb_init(&g_kvdb, &hw_flash_ops);
}

// 任务A: 写入
void taskA(void *arg) {
    while (1) {
        osMutexAcquire(db_mutex, osWaitForever);
        rdb_kvdb_set(&g_kvdb, "sensor1", &data, sizeof(data));
        osMutexRelease(db_mutex);
        
        osDelay(1000);
    }
}

// 任务B: 读取
void taskB(void *arg) {
    while (1) {
        osMutexAcquire( db_mutex, osWaitForever);
        uint16_t len;
        rdb_kvdb_get(&g_kvdb, "sensor1", buf, sizeof(buf), &len);
        osMutexRelease(db_mutex);
        
        osDelay(500);
    }
}
```

#### 方案2: 使用消息队列（解耦）
```c
// 定义数据库操作请求
typedef struct {
    enum { DB_OP_SET, DB_OP_GET, DB_OP_DELETE } op;
    char key[64];
    uint8_t value[256];
    uint16_t value_len;
} db_request_t;

static osMessageQueueId_t db_queue;

void db_task(void *arg) {
    db_request_t req;
    
    while (1) {
        // 等待请求
        osMessageQueueGet(db_queue, &req, NULL, osWaitForever);
        
        // 执行操作
        switch (req.op) {
            case DB_OP_SET:
                rdb_kvdb_set(&g_kvdb, req.key, req.value, req.value_len);
                break;
            case DB_OP_GET:
                rdb_kvdb_get(&g_kvdb, req.key, req.value, 256, &req.value_len);
                // 回复结果...
                break;
            case DB_OP_DELETE:
                rdb_kvdb_delete(&g_kvdb, req.key);
                break;
        }
    }
}

// 其他任务通过队列发送请求
void taskA(void *arg) {
    db_request_t req = {
        .op = DB_OP_SET,
        .key = "sensor1",
        .value_len = 4,
    };
    memcpy(req.value, &sensor_data, 4);
    osMessageQueuePut(db_queue, &req, 0, 0);
}
```

**注意**:
- ⚠️ 迭代器期间禁止修改 (返回 RDB_ERR_BUSY)
- ⚠️ GC 期间会调用 `yield()`，确保回调正确实现
- ⚠️ 不要在中断中直接调用数据库 API（可能阻塞）

---

### Q16: Flash 写入粒度问题

**症状**: STM32F1 只支持半字(2字节)写入，但 RocketDB 要求双字(8字节)

**原因**: 不同 MCU Flash 写入粒度不同:
- STM32F1: 半字 (2B)
- STM32F4: 双字 (8B)
- STM32F7: 双字 (8B)

**解决方案**:

#### 方案1: 使用缓冲写入（适配1/2/4字节）
```c
// 适配 2 字节写入粒度
static int hw_flash_write(uint32_t addr, const void* data, uint32_t len) {
    const uint16_t *p = (const uint16_t*)data;
    uint32_t flash_addr = FLASH_BASE + addr;
    
    for (uint32_t i = 0; i < len; i += 2) {
        if (HAL_FLASH_Program(FLASH_TYPEPROGRAM_HALFWORD, 
                              flash_addr + i, *p++) != HAL_OK) {
            return RDB_ERR;
        }
    }
    
    return RDB_OK;
}
```

#### 方案2: 配置 RocketDB 使用对齐写入
```c
// 在 rocketdb.h 中定义（如果有）
#define RDB_WRITE_ALIGN  2   // 2 字节对齐（STM32F1）
#define RDB_WRITE_ALIGN  4   // 4 字节对齐（其他）
#define RDB_WRITE_ALIGN  8   // 8 字节对齐（STM32F4）
```

**测试**: T-306 (test_kv_write_gran.c) 已验证 1/2/4/8 字节粒度

---

### Q17: GC 导致任务响应延迟

**症状**: 在 GC 期间，实时任务无法响应

**原因**: GC 可能需要擦除多个扇区（耗时操作）

**解决方案**:

#### 方案1: 使用 yield 回调
```c
// 确保 yield 回调正确实现
static void hw_yield(void) {
    taskYIELD();  // 让出 CPU
}

// GC 期间会定期调用 yield()
// 每处理 4KB 调用一次
```

#### 方案2: 异步 GC（手动实现）
```c
// RocketDB 目前不支持异步 GC
// 可以通过分时执行模拟:

void periodic_gc_task(void) {
    // 定期检查空间
    struct rdb_kv_info info;
    rdb_kvdb_space_info(&db, &info);
    
    if (info.avail_bytes < threshold) {
        // 手动触发 GC：写入一些数据触发
        // 或通过删除旧数据间接触发
        rdb_kvdb_delete(&db, "temp_trigger_gc");
    }
}
```

#### 方案3: 增加 GC 保留扇区
```c
// 修改 rocketdb_kvdb.c 的 gc_reserve 计算
// 更多保留扇区 = GC 触发更早 = 单次 GC 更快
```

**预期延迟** (参考):
- 单扇区擦除: 30-50ms (STM32F4)
- GC 处理 1 个扇区: 50-100ms
- yield 调用间隔: 每 4KB (~10ms)

---

## 调试技巧

### 技巧1: 启用详细日志

```c
// 编辑 test/sim/test_framework.h
#define DEBUG 1
#define VERBOSE 1

// 或在代码中添加调试输出
void debug_print_db_state(rdb_kvdb_t *db) {
    printf("=== DB State ===\n");
    printf("Active sector: %u\n", db->active_sec);
    printf("Write seq: %u\n", db->write_seq);
    printf("Iter gen: %u\n", db->iter_gen);
    
    struct rdb_kv_info info;
    rdb_kvdb_space_info(db, &info);
    printf("Space: total=%u, used=%u, avail=%u\n",
           info.total_bytes, info.used_bytes, info.avail_bytes);
    printf("Wear: min_ec=%u, max_ec=%u, avg_ec=%u\n",
           info.min_ec, info.max_ec, info.avg_ec);
    printf("Stats: write=%u, read=%u, delete=%u, gc=%u\n",
           info.write_ops, info.read_ops, info.delete_ops, info.gc_runs);
}
```

---

### 技巧2: 使用断言验证假设

```c
#include <assert.h>

// 验证 API 调用结果
rdb_err_t err = rdb_kvdb_set(&db, "key", "value", 5);
assert(err == RDB_OK);  // 如果失败会立即中断

// 验证数据完整性
uint16_t len;
rdb_kvdb_get(&db, "key", buf, sizeof(buf), &len);
assert(len == 5);
assert(memcmp(buf, "value", 5) == 0);
```

---

### 技巧3: 分析磨损热力图

```bash
# 运行磨损测试
build_wear_heatmap.bat

# 分析 CSV 文件
Import-Csv test\out\kv_wear_heatmap.csv | Format-Table

# 检查磨损分布
# 预期: max_ec - min_ec <= 2
```

---

### 技巧4: 使用 Git Bisect 定位回归

```bash
# 如果某个版本引入了 bug
git bisect start
git bisect bad          # 当前版本有问题
git bisect good v0.0.1  # v0.0.1 是好的

# Git 会自动切换到中间版本
build.bat all test      # 测试

# 根据结果标记
git bisect good   # 或 git bisect bad

# 重复直到找到引入 bug 的 commit
```

---

### 技巧5: 内存检查 (Valgrind/AddressSanitizer)

```bash
# Linux: 使用 Valgrind 检测内存泄漏
valgrind --leak-check=full ./test/out/test_kv_basic

# 或使用 AddressSanitizer (编译时)
clang -fsanitize=address test_kv_basic.c ... -o test_kv_basic
./test_kv_basic
```

---

## 📞 获取帮助

如果以上方法都无法解决问题:

1. **查看文档**:
   - [SpecKit/clarify.md](../SpecKit/clarify.md) - 设计决策
   - [SpecKit/analyze.md](../SpecKit/analyze.md) - 功能分析
   - [docs/EXAMPLES.md](EXAMPLES.md) - 代码示例

2. **检查日志**:
   - 测试日志: `test/out/test_*_log_*.log`
   - 回归报告: `SpecKit/ARCHIVE/REGRESSION_REPORT_2026-02-25.md`

3. **报告问题**:
   - 提供详细的错误信息
   - 附上测试日志
   - 说明复现步骤

---

**最后更新**: 2026-04-29  
**版本**: v1.1.0  
**反馈**: 如有改进建议，请更新本文档
