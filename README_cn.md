# acp — 异步文件复制工具

基于 Linux AIO / io_uring 和 macOS Grand Central Dispatch 的高性能文件复制工具，支持可选的 inotify/FSEvents 目录监控和块级校验和验证。

## 功能特性

- **多种异步 I/O 后端**
  - Linux：`libaio`（默认）或 `io_uring`（`--enable-uring`）
  - macOS：`GCD`（Grand Central Dispatch）
- **复制模式**
  - `CopyOnly` — 标准高吞吐复制
  - `CksumCopy` — 读取源和目标数据块，比较校验和，仅写入差异块
  - `CksumOnly` — 仅校验不写入，结果记录到 `./cksum_result.log`
- **实时目录同步** — 可选 `inotify`（Linux）/ `FSEvents`（macOS）监控，实现持续复制
- **Direct I/O 支持** — 绕过页缓存，适合大文件顺序复制场景
- **稀疏文件保持** — 检测并保留文件空洞
- **可配置并行度** — 多复制线程，每个线程独立的 I/O 队列深度
- **跨平台** — 支持 Linux 和 macOS

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
- Homebrew 依赖包：

```bash
brew install spdlog openssl xxhash
```

## 编译构建

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

构建产物位于 `build/` 目录下：

```bash
cmake --build build --target clean   # 清除构建产物
rm -rf build                         # 完全清理
```

## 配置说明

`acp` 读取 `./acp_config.json`（本地）或 `/etc/acp_config.json`（系统级）。

示例 `acp_config.json`：

```json
{
  "LogLevel": "info",
  "LogMode": "console",
  "LogFilePath": "/var/log/acp.log",
  "CopyEngine": "libaio",
  "CopyMode": "CopyOnly",
  "CksumAlgorithm": "xxhash64",
  "CopyParallelism": 4,
  "CopyChanSize": 10,
  "EnableInotify": false,
  "PreserveSparseFiles": false,
  "DirectIO": false,
  "SyncWrites": false,
  "CopyOptions": {
    "IOSize": 1048576,
    "QueueDepth": 8,
    "Batch": 4,
    "IOReapWait": 1
  }
}
```

### 配置字段说明

| 字段 | 说明 | 默认值 |
|------|------|--------|
| `CopyEngine` | `"libaio"`、`"liburing"`（Linux）、`"gcd"`（macOS） | `"libaio"` |
| `CopyMode` | `"CopyOnly"`、`"CksumCopy"`、`"CksumOnly"` | `"CopyOnly"` |
| `CksumAlgorithm` | `"xxhash64"`、`"md5"`、`"sha256"` | `"xxhash64"` |
| `CopyParallelism` | 并发复制线程数 | `1` |
| `IOSize` | 每次 I/O 读/写大小（字节） | `1048576`（1 MiB） |
| `QueueDepth` | 每个线程的最大飞行中 I/O 数 | `8` |
| `DirectIO` | 使用 `O_DIRECT` 绕过页缓存 | `false` |
| `SyncWrites` | 每个文件复制完成后执行 `fsync` | `false` |
| `EnableInotify` | 监控源目录变化（永不退出） | `false` |

## 使用方式

```bash
# 复制目录
./acp /path/to/src /path/to/dst

# 复制单个文件
./acp /path/to/src/file.dat /path/to/dst/file.dat

# 复制文件到目录
./acp /path/to/src/file.dat /path/to/dst_dir/
```

目录到目录复制时，`acp` 会在目标目录下创建一个与源目录同名的子目录。

## 复制模式详解

### CopyOnly
标准异步 I/O 复制，适合初次全量复制。

### CksumCopy
读取源数据块后，读取目标对应偏移的数据块并比较校验和。仅在校验和不一致时才写入。适合大多数数据块未变化的大文件增量同步场景。

### CksumOnly
与 `CksumCopy` 相同的比较逻辑，但从不写入。不匹配项记录到 `./cksum_result.log`，CSV 格式：

```
/path/to/src,/path/to/dst,offset,MISMATCH
```

## 测试

```bash
# 运行全部测试
ctest --test-dir build --output-on-failure

# 或直接运行测试二进制
./build/test_acp

# 排除慢速集成测试（大文件集）
./build/test_acp "~[integration]"
```

测试覆盖 `Channel`、`CPFilePair`、`CPFilePairMgr`、`IOSlot`、`CksumCopy`、`CksumOnly`、`liburing`、`DirectIO`、`SyncWrites`、`Inotify` 以及端到端集成场景。

## Docker（推荐开发环境）

项目提供预配置的 Oracle Linux 9 镜像：

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
  ├── LoadCopyOptions()  →  ./acp_config.json
  ├── CopyDir() / CopyFile()
  │     ├── Channel<CopyEntry>（生产者）
  │     └── CopyEngine
  │           ├── CPFilePairMgr（每线程一个）
  │           └── IOSlotMgr
  │                 ├── AIOSlotMgr  (libaio)
  │                 ├── UIOSlotMgr  (io_uring)
  │                 └── GCDSlotMgr  (macOS GCD)
  └── Inotify / FSEvents（可选，实时同步）
```

- **CopyEngine**：主线程以轮询方式将 `CopyEntry` 分发给工作线程。
- **CPFilePairMgr**：通过 `FPChannel` 管理文件对生命周期（pending → inflight → completed）。
- **IOSlotMgr**：抽象模板基类，共享完成处理逻辑（`HandleReadCompletion`、`HandleWriteCompletion`）。各后端实现 `Init`、`DoPrepareOneRead`、`SubmitOneRead`、`IOReap` 等。
- **IOSlot**：状态机（`Init → ReadPrepared → ReadSubmitted → ReadReaped → WritePrepared → WriteSubmitted → WriteReaped`）。

## 性能说明

- **io_uring** 在 Linux 5.1+ 上通常优于 `libaio`，因为减少了系统调用开销。
- **Direct I/O** 适合大文件顺序复制场景，但要求 I/O 大小为扇区对齐。
- 对于高 IOPS 存储（NVMe SSD、RAID 阵列），提高 `QueueDepth` 和 `CopyParallelism` 可获得更好性能。
- `CksumCopy` 会增加目标侧的读取放大；仅在写入带宽是瓶颈时使用。

## 许可证

TBD
