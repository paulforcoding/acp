# acp — Asynchronous File Copy Tool

**acp** = **a**sync **cp**

High-performance file copy using Linux AIO / io_uring and macOS Grand Central Dispatch, with optional inotify/FSEvents directory monitoring and block-level checksum verification.

## Project Vision

1. **acp** stands for **async cp** — a drop-in, high-performance replacement for the native `cp` command on Linux and macOS.
2. Designed to **replace native `cp`** with superior throughput via async I/O, while keeping all CLI behaviors identical to `cp`.
3. **cp-compatible behavior** — command-line usage, path handling, and exit semantics mirror the standard `cp` command.
4. **Supported file types** — regular files, directories, and symbolic links only. Special files (device files, sockets, FIFOs/pipes, etc.) are explicitly unsupported.
5. **Continuous replication** — optional `inotify` (Linux) / `FSEvents` (macOS) monitoring keeps the destination in sync with the source in real time.
6. **AI-agent friendly** — structured logging, progress telemetry, and machine-readable status make it easy for AI agents to observe and reason about copy operations.
7. **Enterprise-grade migration** — built for large-scale data migration, efficiently handling both massive quantities of small files and very large individual files.

## Features

- **Multiple Async I/O Backends**
  - Linux: `libaio` (default) or `io_uring` (`--enable-uring`)
  - macOS: `GCD` (Grand Central Dispatch)
- **Copy Modes**
  - `CopyOnly` — standard high-throughput copy
  - `CksumCopy` — read source and destination blocks, compare checksums, write only differing blocks
  - `CksumOnly` — verify without writing, results logged to `./cksum_result.log`
- **Live Directory Sync** — optional `inotify` (Linux) / `FSEvents` (macOS) monitoring for continuous replication
- **Direct I/O** support — bypass page cache for large sequential workloads
- **Sparse File Preservation** — detect and preserve holes
- **Configurable Parallelism** — multiple copy threads with per-thread I/O queue depth
- **Cross-Platform** — Linux and macOS

## System Requirements

### Linux

- C++20 compiler (`g++` recommended)
- `libaio-dev`
- `libspdlog-dev`
- `libfmt-dev`
- `libssl-dev`
- `libxxhash-dev`
- Optional: `liburing-dev` (for io_uring backend)

```bash
# Oracle Linux 9 / RHEL 9 / Rocky Linux 9
sudo dnf install gcc-c++ libaio-devel spdlog-devel fmt-devel openssl-devel xxhash-devel
# Optional:
sudo dnf install liburing-devel

# Ubuntu / Debian
sudo apt-get install g++ libaio-dev libspdlog-dev libfmt-dev libssl-dev libxxhash-dev
# Optional:
sudo apt-get install liburing-dev
```

### macOS

- Xcode Command Line Tools
- Homebrew packages:

```bash
brew install spdlog openssl xxhash
```

## Build

Requires CMake 3.20+.

```bash
# Configure (default: static linking)
cmake -B build

# Linux: enable io_uring backend
cmake -B build -DENABLE_LIBURING=ON

# macOS / dynamic linking
cmake -B build -DBUILD_STATIC=OFF

# Compile
cmake --build build -j$(nproc)
```

Build artifacts are placed under `build/`:

```bash
cmake --build build --target clean   # remove build artifacts
rm -rf build                         # full clean
```

## Configuration

`acp` reads `./acp_config.json` (local) or `/etc/acp_config.json` (system-wide).

Example `acp_config.json`:

```json
{
  "ProgramLogLevel": "info",
  "ProgramLogMode": "console",
  "ProgramLogFilePath": "./acp.log",
  "FileLogEnabled": false,
  "FileLogIntervalSec": 5,
  "FileLogPath": "./.acp_state.json",
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

### Configuration Fields

| Field | Description | Default |
|-------|-------------|---------|
| `ProgramLogLevel` | `"trace"`, `"debug"`, `"info"`, `"warn"`, `"error"`, `"fatal"` | `"info"` |
| `ProgramLogMode` | `"console"` or `"file"` | `"console"` |
| `ProgramLogFilePath` | Program log file path (mode=file) | `"./acp.log"` |
| `FileLogEnabled` | Enable structured file progress events | `false` |
| `FileLogIntervalSec` | Progress summary interval in seconds | `5` |
| `FileLogPath` | State file path (`""` to disable) | `"./.acp_state.json"` |
| `CopyEngine` | `"libaio"`, `"liburing"` (Linux), `"gcd"` (macOS) | `"libaio"` |
| `CopyMode` | `"CopyOnly"`, `"CksumCopy"`, `"CksumOnly"` | `"CopyOnly"` |
| `CksumAlgorithm` | `"xxhash64"`, `"md5"`, `"sha256"` | `"xxhash64"` |
| `CopyParallelism` | Number of concurrent copy threads | `1` |
| `IOSize` | Per-I/O read/write size in bytes | `1048576` (1 MiB) |
| `QueueDepth` | Max in-flight I/Os per thread | `8` |
| `DirectIO` | Use `O_DIRECT` bypassing page cache | `false` |
| `SyncWrites` | `fsync` each file after completion | `false` |
| `EnableInotify` | Monitor source for changes (never exits) | `false` |

## Usage

```bash
# Copy directory
./acp /path/to/src /path/to/dst

# Copy single file
./acp /path/to/src/file.dat /path/to/dst/file.dat

# Copy file into directory
./acp /path/to/src/file.dat /path/to/dst_dir/
```

When copying a directory to a directory, `acp` creates a subdirectory inside the destination with the same name as the source directory.

## Copy Modes Explained

### CopyOnly
Standard async I/O copy. Best for initial replication.

### CksumCopy
After reading a source block, reads the corresponding destination block and compares checksums. Only writes if they differ. Useful for incremental sync of large files where most blocks are unchanged.

### CksumOnly
Same comparison as `CksumCopy`, but never writes. Mismatches are logged to `./cksum_result.log` in CSV format:

```
/path/to/src,/path/to/dst,offset,MISMATCH
```

## Testing

```bash
# Run all tests
ctest --test-dir build --output-on-failure

# Or run the test binary directly
./build/test_acp

# Exclude slow integration tests (large dataset)
./build/test_acp "~[integration]"
```

Tests cover `Channel`, `CPFilePair`, `CPFilePairMgr`, `IOSlot`, `CksumCopy`, `CksumOnly`, `liburing`, `DirectIO`, `SyncWrites`, `Inotify`, and end-to-end integration scenarios.

## Docker (Recommended for Development)

A pre-configured Oracle Linux 9 image is provided:

```bash
# Build image
docker build -t acp-ol9-dev -f Dockerfile.ol9 .

# Compile inside container
docker run --rm -v "$(pwd):/acp" -w /acp acp-ol9-dev bash -c "cmake -B build -DENABLE_LIBURING=ON && cmake --build build"

# Run tests
docker run --rm -v "$(pwd):/acp" -w /acp acp-ol9-dev ./build/test_acp "~[integration]"

# Interactive shell
docker run -it --rm -v "$(pwd):/acp" -w /acp acp-ol9-dev bash
```

## Architecture

```
main.cpp
  ├── LoadCopyOptions()  →  ./acp_config.json
  ├── CopyDir() / CopyFile()
  │     ├── Channel<CopyEntry> (producer)
  │     └── CopyEngine
  │           ├── CPFilePairMgr (per-thread)
  │           └── IOSlotMgr
  │                 ├── AIOSlotMgr  (libaio)
  │                 ├── UIOSlotMgr  (io_uring)
  │                 └── GCDSlotMgr  (macOS GCD)
  └── Inotify / FSEvents (optional, live sync)
```

- **CopyEngine**: Main thread distributes `CopyEntry` to worker threads in round-robin fashion.
- **CPFilePairMgr**: Manages file pair lifecycle via `FPChannel` (pending → inflight → completed).
- **IOSlotMgr**: Abstract template base with shared completion logic (`HandleReadCompletion`, `HandleWriteCompletion`). Backends implement `Init`, `DoPrepareOneRead`, `SubmitOneRead`, `IOReap`, etc.
- **IOSlot**: State machine (`Init → ReadPrepared → ReadSubmitted → ReadReaped → WritePrepared → WriteSubmitted → WriteReaped`).

## Performance Notes

- **io_uring** generally outperforms `libaio` on Linux 5.1+ due to reduced syscall overhead.
- **Direct I/O** is beneficial for large sequential workloads but requires sector-aligned I/O sizes.
- Increase `QueueDepth` and `CopyParallelism` for high-IOPS storage (NVMe SSDs, RAID arrays).
- `CksumCopy` adds read amplification on the destination side; use when write bandwidth is the bottleneck.

## License

TBD
