# acp — 异步高性能文件复制工具

**acp** = **a**sync **cp**

基于 Linux AIO / io_uring 和 macOS Grand Central Dispatch 的高性能文件复制工具，支持可选的 inotify/FSEvents 目录监控和块级校验和验证。

## 项目定位

1. **acp = async cp = agent cp** — 核心定位：**Agent-First** 高性能文件复制工具。
2. **功能上替代原生 `cp`** — 通过异步 I/O 提供更高吞吐，CLI 用法与标准 `cp` 对齐；不利于 Agent 使用或观测的功能不做，不为追求 100% `cp` 兼容而牺牲 Agent 体验。
3. **与 `cp` 行为对齐** — 命令行用法、路径处理、退出语义在可行范围内与标准 `cp` 命令保持一致。
4. **仅支持三类文件** — 普通文件、目录、符号链接。设备文件、socket、pipe 等特殊文件不支持复制。
5. **持续复制服务** — 通过 `inotify`（Linux）/ `FSEvents`（macOS）监控源目录变化，提供持续同步能力。
6. **AI Agent 友好** — 结构化日志、进度遥测、机器可读状态，使 AI Agent 能够感知和推理文件复制状态。
7. **企业级文件迁移** — 定位于企业级大规模数据迁移，同时高效支持大量小文件和超大单文件的高性能复制。

## 特性

- **多种异步 I/O 后端**
  - Linux：`libaio`（默认）或 `io_uring`（`--enable-uring`）
  - macOS：`GCD`（Grand Central Dispatch）
- **复制模式**
  - `CopyOnly` — 标准高吞吐复制
  - `CksumCopy` — 块级去重，带 size+mtime 快速预检；仅写入差异块
  - `CksumOnly` — 仅校验不写入，结果记录到 `./cksum_result.log`
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
# 配置（默认静态链接）
cmake -B build

# Linux：启用 io_uring 后端
cmake -B build -DENABLE_LIBURING=ON

# macOS / 动态链接
cmake -B build -DBUILD_STATIC=OFF

# 编译
cmake --build build -j$(nproc)
```

编译产物位于 `build/`：

```bash
cmake --build build --target clean   # 清除编译产物
rm -rf build                         # 完全清理
```

## 配置

`acp` 读取 `./acp_config.json`（本地）或 `/etc/acp_config.json`（系统级）。

示例 `acp_config.json`：

```json
{
  "ProgramLogLevel": "info",
  "ProgramLogMode": "file",
  "ProgramLogFilePath": "/tmp/acp_program.log",
  "FileLogEnabled": true,
  "FileLogMode": "file",
  "FileLogIntervalSec": 5,
  "FileLogPath": "/tmp/acp_file_info.json",
  "CopyEngine": "libaio",
  "CopyMode": "CopyOnly",
  "CksumAlgorithm": "xxhash64",
  "CopyParallelism": 1,
  "CopyChanSize": 10,
  "EnableInotify": false,
  "PreserveSparseFiles": true,
  "PreserveMeta": true,
  "DirectIO": false,
  "SyncWrites": false,
  "CopyOptions": {
    "IOSize": 1048576,
    "QueueDepth": 8,
    "Batch": 8,
    "IOReapWait": 1
  }
}
```

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
| `CopyEngine` | `"libaio"`、`"liburing"`（Linux）、`"gcd"`（macOS） | `"libaio"` |
| `CopyMode` | `"CopyOnly"`、`"CksumCopy"`、`"CksumOnly"` | `"CopyOnly"` |
| `CksumAlgorithm` | `"xxhash64"`、`"md5"`、`"sha256"` | `"xxhash64"` |
| `CopyParallelism` | 并发复制线程数 | `1` |
| `IOSize` | 每次 I/O 读写大小（字节） | `1048576`（1 MiB） |
| `QueueDepth` | 每线程最大在途 I/O 数 | `8` |
| `Batch` | 每批次提交的 I/O 数 | `8` |
| `IOReapWait` | I/O 完成等待超时（秒） | `1` |
| `IOStuckTimeout` | 卡住 I/O 检测超时（秒，`0` = 关闭） | `0` |
| `DirectIO` | 使用 `O_DIRECT` 绕过页缓存 | `false` |
| `SyncWrites` | 每个文件完成后执行 `fsync` | `false` |
| `PreserveSparseFiles` | 保留稀疏文件空洞 | `true` |
| `PreserveMeta` | 保留时间戳、权限、所有者、xattr、ACL | `false` |
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
- **跨网络复制** — `acp` 是纯本地文件复制工具，不支持 `scp`、`rsync`、`sftp` 或任何网络协议。
- **特殊文件复制** — 设备文件、socket、FIFO、whiteout 文件会被跳过（日志记录为 unsupported）。如需复制这些文件，请使用 `cp -a` 或 `rsync`。
- **归档级元数据保真度** — 虽然 `PreserveMeta` 可保留基本元数据（时间戳、权限、所有者、xattr、ACL），但在某些边缘场景下可能仍不及 `cp -a` 或 `rsync -a`（例如 SELinux 上下文、所有文件系统上的亚秒级精度）。
- **交互式或脚本化的单文件操作** — 需要 `cp` 的简洁性和立即退出语义的场景。

## 复制模式说明

### CopyOnly
标准异步 I/O 复制。适合首次复制。

### CksumCopy
首先检查文件大小和修改时间；若与目标端完全一致则直接跳过整个文件。否则读取源块并与对应目标块比较校验和 —— 仅写入差异块。适合大文件增量同步，其中大部分数据未变更。

### CksumOnly
与 `CksumCopy` 相同的比较逻辑，但不写入。不匹配项以 CSV 格式记录到 `./cksum_result.log`：

```
/path/to/src,/path/to/dst,offset,MISMATCH
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

## Docker（推荐开发环境）

提供预配置的 Oracle Linux 9 镜像：

```bash
# 构建镜像
docker build -t acp-ol9-dev -f Dockerfile.ol9 .

# 容器内编译
docker run --rm -v "$(pwd):/acp" -w /acp acp-ol9-dev bash -c "cmake -B build -DENABLE_LIBURING=ON && cmake --build build"

# 运行测试
docker run --rm -v "$(pwd):/acp" -w /acp acp-ol9-dev ./build/test_acp "~[integration]"

# 交互式 shell
docker run -it --rm -v "$(pwd):/acp" -w /acp acp-ol9-dev bash
```

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
- **Direct I/O** 对大顺序工作负载有益，但需要扇区对齐的 I/O 大小。
- 对高 IOPS 存储（NVMe SSD、RAID 阵列）增加 `QueueDepth` 和 `CopyParallelism`。
- `CksumCopy` 会在目标端增加读放大；仅在写带宽为瓶颈时使用。size+mtime 快速路径可完全跳过未变更文件，消除这些文件的读放大。
- **看门狗**（`IOStuckTimeout`）可防护不稳定存储或内核驱动 bug 导致的 I/O 挂起。设置为预期最大 I/O 延迟的数倍（如 `30` 秒）。在调试器下运行或 I/O 延迟故意波动的系统上，设为 `0` 关闭。

### macOS GCD 后端说明

macOS 后端使用 **Grand Central Dispatch (GCD)**，通过 `dispatch_group_async` + `pread`/`pwrite` 来**模拟**异步 I/O。与 Linux 的 `io_uring` 或 `libaio`（在内核态排队和完成 I/O 请求）不同，GCD 是将阻塞式系统调用分发到线程池中执行。这意味着：

- macOS 后端是**"伪异步"** — 通过线程实现并发，而非真正的内核异步 I/O。
- 对大顺序复制场景，吞吐量可能与 Linux 相当，但**延迟和 CPU 开销更高**，因为需要线程管理。
- 在 macOS 上，`acp` 相对 `cp` 的性能提升**不如 Linux 上 io_uring 的提升那么显著**。
- 如果在 macOS 上追求最大吞吐量，可考虑使用 `rsync` 或支持 APFS clone 复制的 `cp -c`。

## 许可证

TBD
