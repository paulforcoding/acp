# acp — 异步高性能文件复制工具

**acp** = **a**sync **cp** = **agent** cp

核心定位：**Agent-First** 高性能文件复制工具

1. 使用 Linux AIO / io_uring 和 macOS Grand Central Dispatch 提高文件复制性能，支持通过 inotify/FSEvents 进行持续的文件复制
2. 对AI Agent友好：日志和文件复制进度都通过JSONL格式输出，让Agent知晓复制情况；并提供Agent skill来让agent更好地使用本项目

使用场景：

1. 日常使用：acp在功能上，涵盖cp的常用功能（冷僻的功能没有），足以覆盖日常使用场景
2. 企业级文件迁移：这是acp项目的目标。它具备CopyOnly（全量复制）CksumOnly（校验源端和目的端文件是否一致）CksumCopy（增量复制）三大功能，具备丰富的性能相关参数，支持海量小文件和大文件的数据迁移。



## 特性

- **多种异步 I/O 后端**
  - Linux：`libaio`（默认）或 `io_uring`（`--enable-uring`），内核级异步复制，对比一般复制手段性能提升明显
  - macOS：`GCD`（Grand Central Dispatch），非内核级异步复制，性能提升幅度不大
- **复制模式**
  - `CopyOnly` — 标准高性能异步复制
  - `CksumCopy` — 块级cksum校验源端和目的端文件，带 size+mtime 快速预检；仅复制差异文件
  - `CksumOnly` — 仅校验不写入，用来对比源端和目的端的文件差异情况，对比结果记录到文件报告。
- **CLI11 命令行参数** — 通过命令行标志覆盖任意配置字段，与 JSON 配置文件分层叠加
- **分层配置加载** — 配置解析顺序：硬编码默认值 → `/etc/acp_config.json` → `~/.acp_config.json` → `./acp_config.json` → CLI 参数（后者覆盖前者）
- **I/O 看门狗** — 自动检测并中止卡住的 I/O 操作（`IOStuckTimeout`）
- **实时目录同步** — 可选 `inotify`（Linux）/ `FSEvents`（macOS）监控，实现持续复制
- **Direct I/O 支持** — 大顺序工作负载绕过页缓存
- **稀疏文件保持** — 检测并保持文件空洞（类似 `cp --sparse=auto`）
- **元数据保留** — 保留时间戳、权限、所有者、扩展属性和 ACL（通过 `PreserveMeta` 可选启用）；目录元数据延迟到所有文件复制完成后保留
- **双日志系统** — 独立的 `ProgramLog`（程序诊断日志）和 `FileLog`（逐文件复制遥测），各自有独立的 mode 和文件路径配置
- **可配置并行度** — 多复制线程，每线程独立 I/O 队列深度
- **跨平台** — Linux 和 macOS

## 系统要求

### Linux

- C++20 编译器（推荐 `g++`）
- `libaio-dev`
- `libspdlog-dev`
- `libfmt-dev`
- `libssl-dev`
- `libxxhash-dev`
- 可选：`liburing-dev`（io_uring 后端）

```bash
# Oracle Linux 9 / RHEL 9 / Rocky Linux 9
sudo dnf install gcc-c++ libaio-devel spdlog-devel fmt-devel openssl-devel xxhash-devel
# 可选：
sudo dnf install liburing-devel

# Ubuntu / Debian
sudo apt-get install g++ libaio-dev libspdlog-dev libfmt-dev libssl-dev libxxhash-dev
# 可选：
sudo apt-get install liburing-dev
```

### macOS

- Xcode Command Line Tools
- Homebrew 包：

```bash
brew install spdlog openssl xxhash
```

## 编译

需要 CMake 3.20+。

```bash
# 配置（默认动态链接）
cmake -B build

# Linux：启用 io_uring 后端
cmake -B build -DENABLE_LIBURING=ON

# 静态链接（仅 Linux）
cmake -B build -DBUILD_STATIC=ON

# 编译
cmake --build build -j$(nproc)

# 安装 / 卸载
cmake --build build --target install   # 默认 prefix: /usr/local
cmake --build build --target uninstall
```

编译产物位于 `build/`：

```bash
cmake --build build --target clean   # 清除编译产物
rm -rf build                         # 完全清理
```

## 配置

`acp` 按以下顺序读取参数：硬编码默认值 → `/etc/acp_config.json` → `~/.acp_config.json` → `./acp_config.json` → CLI 参数（后者覆盖前者）。

### 日常使用场景配置参考

示例 `acp_config.json`：

```json
{
  "ProgramLogLevel": "error",
  "ProgramLogMode": "console",
  "ProgramLogFilePath": "/tmp/acp_program.log",
  "FileLogEnabled": false,
  "FileLogMode": "file",
  "FileLogIntervalSec": 5,
  "FileLogPath": "/tmp/acp_file_info.json",
  "CopyEngine": "liburing",
  "CopyMode": "CopyOnly",
  "CksumAlgorithm": "xxhash64",
  "CopyParallelism": 1,
  "CopyChanSize": 100,
  "EnableInotify": false,
  "PreserveSparseFiles": true,
  "PreserveMeta": true,
  "DirectIO": false,
  "SyncWrites": true,
  "CopyOptions": {
    "IOSize": 131072,
    "QueueDepth": 8,
    "Batch": 8,
    "IOReapWait": 1
  }
}
```

### 数据迁移场景配置参考

```json
{
  "ProgramLogLevel": "error",
  "ProgramLogMode": "file",
  "ProgramLogFilePath": "/tmp/acp_program.log",
  "FileLogEnabled": true,
  "FileLogMode": "file",
  "FileLogIntervalSec": 5,
  "FileLogPath": "/tmp/acp_file_info.json",
  "CopyEngine": "liburing",
  "CopyMode": "CopyOnly",
  "CksumAlgorithm": "xxhash64",
  "CopyParallelism": 1,
  "CopyChanSize": 1000,
  "EnableInotify": false,
  "PreserveSparseFiles": true,
  "PreserveMeta": true,
  "DirectIO": false,
  "SyncWrites": true,
  "CopyOptions": {
    "IOSize": 262144,
    "QueueDepth": 32,
    "Batch": 8,
    "IOReapWait": 1
  }
}
```

可以根据机器配置和具体文件系统调整性能参数

### 配置字段

| 字段 | 说明 | 默认值 |
|------|------|--------|
| `ProgramLogLevel` | `"trace"`、`"debug"`、`"info"`、`"warn"`、`"error"`、`"fatal"` | `"info"` |
| `ProgramLogMode` | `"console"` 或 `"file"` | `"console"` |
| `ProgramLogFilePath` | 程序日志文件路径（mode=file 时生效） | `"/tmp/acp_program.log"` |
| `FileLogEnabled` | 是否启用结构化文件进度事件 | `false` |
| `FileLogMode` | `"console"` 或 `"file"` | `"file"` |
| `FileLogIntervalSec` | 进度汇总输出间隔（秒） | `5` |
| `FileLogPath` | NDJSON 事件日志路径（file 模式） | `"/tmp/acp_file_info.json"` |
| `CopyEngine` | `"libaio"`、`"liburing"`（Linux）、`"gcd"`（macOS） | `"liburing"` |
| `CopyMode` | `"CopyOnly"`、`"CksumCopy"`、`"CksumOnly"` | `"CopyOnly"` |
| `CksumAlgorithm` | `"xxhash64"`、`"md5"`、`"sha256"` | `"xxhash64"` |
| `CopyParallelism` | 并发复制线程数（由于async复制，一般情况此值无需过大） | `1` |
| `IOSize` | 每次 I/O 读写大小（字节） | `131072`（128 KiB） |
| `QueueDepth` | 每线程最大在途 I/O 数 | `16` |
| `Batch` | 每批次提交的 I/O 数 | `8` |
| `IOReapWait` | I/O 完成等待超时（秒） | `1` |
| `IOStuckTimeout` | 卡住 I/O 检测超时（秒，`0` = 关闭） | `10` |
| `DirectIO` | 使用 `O_DIRECT` 绕过页缓存 | `false` |
| `SyncWrites` | 每个文件完成后执行 `fsync` | `true` |
| `PreserveSparseFiles` | 保留稀疏文件空洞 | `true` |
| `PreserveMeta` | 保留时间戳、权限、所有者、xattr、ACL | `true` |
| `EnableInotify` | 监控源目录变化（永不退出） | `false` |

## 用法

```bash
# 复制目录
./acp /path/to/src /path/to/dst

# 复制单个文件
./acp /path/to/src/file.dat /path/to/dst/file.dat

# 复制文件到目录
./acp /path/to/src/file.dat /path/to/dst_dir/
```

复制目录到目录时，`acp` 会在目标目录内创建与源目录同名的子目录。

### 命令行选项

所有配置字段均可通过 CLI 标志覆盖。分层解析顺序：硬编码默认值 → `/etc/acp_config.json` → `~/.acp_config.json` → `./acp_config.json` → CLI 参数（后者覆盖前者）。

```bash
# 通用选项
./acp --version                    # 显示版本（v0.5.0）
./acp --help                       # 显示所有标志

# 日志选项
./acp --log-level debug src dst    # trace/debug/info/warn/error/critical
./acp --log-mode console src dst   # console 或 file
./acp --log-file /tmp/acp.log src dst
./acp --enable-file-log src dst    # 启用逐文件 NDJSON 遥测
./acp --file-log-path /tmp/events.json src dst

# 复制引擎和模式
./acp -e liburing -m CksumCopy src dst    # 使用 io_uring + 校验和复制
./acp --cksum-algo sha256 src dst         # xxhash64/md5/sha256

# I/O 调优
./acp -p 4 -q 64 --io-size 4194304 src dst   # 4 线程，QD 64，4 MiB I/O
./acp -b 16 -w 2 src dst                     # Batch 16，reap wait 2 秒

# 看门狗（0 = 关闭）
./acp -t 30 src dst                # I/O 卡住超过 30 秒则中止

# 功能开关
./acp --enable-direct-io src dst
./acp --enable-sync src dst
./acp --enable-sparse src dst
./acp --enable-preserve-meta src dst
./acp --enable-inotify src dst     # 持续同步（永不退出）
```

布尔型标志支持 `--enable-*` / `--disable-*` 成对写法，用于显式覆盖。

## 适用场景 / 不适用场景

### 推荐使用 `acp`

- **大规模数据迁移** — 复制数百万文件或 TB 级数据集。异步 I/O 引擎和多线程流水线摊平启动开销，充分饱和存储带宽。
- **超大单文件** — 顺序读写多 GB 文件，队列深度和并行度是关键。
- **校验和增量同步** — `CksumCopy` / `CksumOnly` 带 size+mtime 快速预过滤 + 块级去重，适合大部分数据未变更的场景。
- **持续复制** — `EnableInotify` 实时监控源目录变化，保持目标端同步。
- **高带宽本地存储** — NVMe SSD、RAID 阵列或高速网络存储，`cp` 会成为 I/O 瓶颈的场景。

### 不推荐使用 `acp`

- **小文件、偶发复制** — 几个 KB 或少量文件时，原生 `cp` 更快，因为 `acp` 有 JSON 配置解析和线程池启动开销。
- **特殊文件复制** — 设备文件、socket、FIFO、whiteout 文件会被跳过（日志记录为 unsupported）。如需复制这些文件，请使用 `cp -a` 或 `rsync`。
- **交互式或脚本化的单文件操作** — 需要 `cp` 的简洁性和立即退出语义的场景。

## 复制模式说明

### CopyOnly
标准异步 I/O 复制。适合首次复制。

### CksumCopy
首先检查文件大小和修改时间；若与目标端完全一致则直接跳过整个文件。否则读取源块并与对应目标块比较校验和 —— 仅写入差异块。适合大文件增量同步，其中大部分数据未变更。

### CksumOnly
与 `CksumCopy` 相同的比较逻辑，但不写入。不匹配项以 NDJSON `cksum_result` 事件的形式通过 FileLog 系统输出。启用 `FileLog` 即可捕获：

```json
{"type":"file_info","event":"cksum_result","src":"/path/to/src","dst":"/path/to/dst","result":"mismatch","reason":"checksum_mismatch","offset":1048576,"detail":"xxhash64","timestamp":"2026-04-25T12:34:56Z"}
```

## 测试

```bash
# 运行全部测试
ctest --test-dir build --output-on-failure

# 或直接运行测试二进制文件
./build/test_acp

# 排除慢速集成测试（大数据集）
./build/test_acp "~[integration]"

# 使用 JSON 报告器运行并解析结果
./build/test_acp --reporter json -s -d yes > test_results.json
python3 tests/parse_test_results.py test_results.json
```

测试覆盖 `Channel`、`CPFilePair`、`CPFilePairMgr`、`IOSlot`、`CksumCopy`、`CksumOnly`、`liburing`、`DirectIO`、`SyncWrites`、`PreserveMeta`、`Inotify`、`Watchdog` 及端到端集成场景。

## 架构

```
main.cpp
  ├── MergeCopyOptions()  →  默认值 → /etc → ~ → ./ → CLI
  ├── CopyDir() / CopyFile()
  │     ├── Channel<CopyEntry> (生产者)
  │     └── CopyEngine
  │           ├── CPFilePairMgr (每线程一个)
  │           └── IOSlotMgr
  │                 ├── AIOSlotMgr  (libaio)
  │                 ├── UIOSlotMgr  (io_uring)
  │                 └── GCDSlotMgr  (macOS GCD)
  │           └── Watchdog (卡住 I/O 检测)
  └── Inotify / FSEvents (可选，实时同步)
```

- **MergeCopyOptions**：分层配置解析 — 硬编码默认值 → `/etc/acp_config.json` → `~/.acp_config.json` → `./acp_config.json` → CLI 参数。
- **CopyEngine**：主线程以轮询方式将 `CopyEntry` 分发给工作线程。
- **CPFilePairMgr**：通过 `FPChannel` 管理文件对生命周期（pending → inflight → completed）。
- **IOSlotMgr**：抽象模板基类，共享完成处理逻辑（`HandleReadCompletion`、`HandleWriteCompletion`）。后端实现 `Init`、`DoPrepareOneRead`、`SubmitOneRead`、`IOReap` 等。
- **Watchdog**：监控每个 slot 的 I/O 时间戳；若任何 slot 超过 `IOStuckTimeout` 仍卡住，则中止复制。
- **IOSlot**：状态机（`Init → ReadPrepared → ReadSubmitted → ReadReaped → WritePrepared → WriteSubmitted → WriteReaped`）。

## 性能提示

- **io_uring** 在 Linux 5.1+ 上通常优于 `libaio`，因为减少了系统调用开销。
- **Direct I/O** 对大顺序工作负载有益。
- 对高 IOPS 存储（NVMe SSD、RAID 阵列）增加 `QueueDepth` 和 `CopyParallelism`。
- **不要将 `QueueDepth` 设置为过大的值**（例如数十万或更高）。在 Linux 上，过大的队列深度可能导致 `io_setup` 挂起或耗尽内核资源，而不是返回明确的错误。请保持在合理范围内（几十到几百）。
- `CksumCopy` 会在目标端增加读而减少写，对某些读写性能差距较大的介质有很好的提速效果。size+mtime 快速路径可完全跳过未变更文件，消除这些文件的读放大。
- **看门狗**（`IOStuckTimeout`）可防护不稳定存储或内核驱动 bug 导致的 I/O 挂起。设置为预期最大 I/O 延迟的数倍（如 `30` 秒）。在调试器下运行或 I/O 延迟故意波动的系统上，设为 `0` 关闭。

### macOS GCD 后端说明

macOS 后端使用 **Grand Central Dispatch (GCD)**，通过 `dispatch_group_async` + `pread`/`pwrite` 来**模拟**异步 I/O。与 Linux 的 `io_uring` 或 `libaio`（在内核态排队和完成 I/O 请求）不同，GCD 是将阻塞式系统调用分发到线程池中执行。这意味着：

- macOS 后端是**"伪异步"** — 通过线程实现并发，而非真正的内核异步 I/O。
- 对大顺序复制场景，吞吐量可能与 Linux 相当，但**延迟和 CPU 开销更高**，因为需要线程管理。
- 在 macOS 上，`acp` 相对 `cp` 的性能提升**不如 Linux 上 io_uring 的提升那么显著**。

## 许可证

见LICENSE文件
