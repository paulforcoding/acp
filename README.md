# acp — Asynchronous High-Performance File Copy Tool

**acp** = **a**sync **cp** = **agent** cp

Core positioning: **Agent-First** high-performance file copy tool

1. Uses Linux AIO / io_uring and macOS Grand Central Dispatch to improve file copy performance, with optional inotify/FSEvents for continuous file replication
2. AI Agent friendly: logs and file copy progress are output in JSONL format so agents know the copy status; provides an Agent skill for better agent integration

Use cases:

1. **Daily use**: acp functionally covers cp's common features (obscure features are omitted) and is sufficient for everyday scenarios
2. **Enterprise-grade data migration**: this is acp's target. It offers three modes — CopyOnly (full copy), CksumOnly (verify source and destination consistency), and CksumCopy (incremental copy) — with rich performance tuning parameters, supporting massive small files and large single-file data migration.

## Features

- **Multiple Async I/O Backends**
  - Linux: `libaio` (default) or `io_uring` (`--enable-uring`), kernel-level async replication with noticeable performance gains over conventional copy methods
  - macOS: `GCD` (Grand Central Dispatch), non-kernel-level async replication with modest performance gains
- **Copy Modes**
  - `CopyOnly` — standard high-performance async copy
  - `CksumCopy` — block-level checksum verification between source and destination, with size+mtime fast pre-check; only differing files are copied
  - `CksumOnly` — verify without writing, used to compare source and destination file differences; results are logged to a file report
- **CLI11 Command-Line Arguments** — override any config field via command-line flags, layered on top of JSON config files
- **Layered Configuration Loading** — config resolution: hardcoded defaults → `/etc/acp_config.json` → `~/.acp_config.json` → `./acp_config.json` → CLI flags (last wins)
- **I/O Watchdog** — automatic detection and abort of stuck I/O operations (`IOStuckTimeout`)
- **Live Directory Sync** — optional `inotify` (Linux) / `FSEvents` (macOS) monitoring for continuous replication
- **Direct I/O Support** — bypass page cache for large sequential workloads
- **Sparse File Preservation** — detect and preserve file holes (similar to `cp --sparse=auto`)
- **Metadata Preservation** — preserve timestamps, permissions, ownership, extended attributes, and ACL (optionally via `PreserveMeta`); directory metadata is deferred until after all files complete
- **Dual Logging System** — independent `ProgramLog` (diagnostics) and `FileLog` (per-file copy telemetry), each with separate mode and file-path configuration
- **Configurable Parallelism** — multiple copy threads, each with independent I/O queue depth
- **Cross-Platform** — Linux and macOS

## System Requirements

### Linux

- C++20 compiler (`g++` recommended)
- `libaio-dev`
- `libspdlog-dev`
- `libfmt-dev`
- `libssl-dev`
- `libxxhash-dev`
- Optional: `liburing-dev` (io_uring backend)

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
# Configure (default: dynamic linking)
cmake -B build

# Linux: enable io_uring backend
cmake -B build -DENABLE_LIBURING=ON

# Static linking (Linux only)
cmake -B build -DBUILD_STATIC=ON

# Compile
cmake --build build -j$(nproc)

# Install
cmake --build build --target install   # default prefix: /usr/local
```

Build artifacts are placed under `build/`:

```bash
cmake --build build --target clean   # remove build artifacts
rm -rf build                         # full clean
```

## Configuration

`acp` reads parameters in the following order: hardcoded defaults → `/etc/acp_config.json` → `~/.acp_config.json` → `./acp_config.json` → CLI flags (last wins).

### Everyday Use Configuration Reference

Example `acp_config.json`:

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
    "QueueDepth": 16,
    "Batch": 8,
    "IOReapWait": 1
  }
}
```

### Data Migration Configuration Reference

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

Performance parameters can be tuned based on machine specs and filesystem.

### Configuration Fields

| Field | Description | Default |
|-------|-------------|---------|
| `ProgramLogLevel` | `"trace"`, `"debug"`, `"info"`, `"warn"`, `"error"`, `"fatal"` | `"info"` |
| `ProgramLogMode` | `"console"` or `"file"` | `"console"` |
| `ProgramLogFilePath` | Program log file path (mode=file) | `"/tmp/acp_program.log"` |
| `FileLogEnabled` | Enable structured file progress events | `false` |
| `FileLogMode` | `"console"` or `"file"` | `"file"` |
| `FileLogIntervalSec` | Progress summary output interval (seconds) | `5` |
| `FileLogPath` | NDJSON event log path (file mode) | `"/tmp/acp_file_info.json"` |
| `CopyEngine` | `"libaio"`, `"liburing"` (Linux), `"gcd"` (macOS) | `"liburing"` |
| `CopyMode` | `"CopyOnly"`, `"CksumCopy"`, `"CksumOnly"` | `"CopyOnly"` |
| `CksumAlgorithm` | `"xxhash64"`, `"md5"`, `"sha256"` | `"xxhash64"` |
| `CopyParallelism` | Number of concurrent copy threads (async replication generally does not need large values) | `1` |
| `IOSize` | Per-I/O read/write size (bytes) | `131072` (128 KiB) |
| `QueueDepth` | Max in-flight I/Os per thread | `16` |
| `Batch` | I/Os submitted per batch | `8` |
| `IOReapWait` | I/O completion wait timeout (seconds) | `1` |
| `IOStuckTimeout` | Stuck I/O detection timeout (seconds, `0` = off) | `10` |
| `DirectIO` | Use `O_DIRECT` to bypass page cache | `false` |
| `SyncWrites` | `fsync` after each file completion | `true` |
| `PreserveSparseFiles` | Preserve sparse file holes | `true` |
| `PreserveMeta` | Preserve timestamps, permissions, ownership, xattr, ACL | `true` |
| `EnableInotify` | Monitor source directory changes (never exits) | `false` |

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

### Command-Line Options

All config fields can be overridden via CLI flags. Layered resolution: hardcoded defaults → `/etc/acp_config.json` → `~/.acp_config.json` → `./acp_config.json` → CLI flags (last wins).

```bash
# General options
./acp --version                    # Show version (v0.5.1)
./acp --help                       # Show all flags

# Logging options
./acp --log-level debug src dst    # trace/debug/info/warn/error/critical
./acp --log-mode console src dst   # console or file
./acp --log-file /tmp/acp.log src dst
./acp --enable-file-log src dst    # Enable per-file NDJSON telemetry
./acp --file-log-path /tmp/events.json src dst

# Copy engine and mode
./acp -e liburing -m CksumCopy src dst    # Use io_uring with checksum copy
./acp --cksum-algo sha256 src dst         # xxhash64/md5/sha256

# I/O tuning
./acp -p 4 -q 64 --io-size 4194304 src dst   # 4 threads, QD 64, 4 MiB I/O
./acp -b 16 -w 2 src dst                     # Batch 16, reap wait 2 sec

# Watchdog (0 = disabled)
./acp -t 30 src dst                # Abort if I/O stuck for 30 seconds

# Feature toggles
./acp --enable-direct-io src dst
./acp --enable-sync src dst
./acp --enable-sparse src dst
./acp --enable-preserve-meta src dst
./acp --enable-inotify src dst     # Continuous sync (never exits)
```

Boolean flags support `--enable-*` / `--disable-*` pairs for explicit override.

## When to Use / When NOT to Use

### Use `acp` when

- **Large-scale data migration** — copying millions of files or terabyte-scale datasets. The async I/O engine and multi-threaded pipeline amortize the fixed startup cost and saturate storage bandwidth.
- **Very large single files** — sequential read/write of multi-GB files where queue depth and parallelism matter.
- **Incremental sync with checksums** — `CksumCopy` / `CksumOnly` with fast size+mtime pre-filter and block-level deduplication when most data is unchanged.
- **Continuous replication** — `EnableInotify` keeps destination in sync with source changes in real time.
- **High-bandwidth local storage** — NVMe SSDs, RAID arrays, or fast network-attached storage where `cp` becomes I/O-bound.

### Do NOT use `acp` when

- **Small, one-off copies** — for a few KB or a handful of files, native `cp` is faster because `acp` has JSON parsing and thread-pool startup overhead.
- **Special file replication** — device files, sockets, FIFOs, and whiteout files are skipped (logged as unsupported). Use `cp -a` or `rsync` if you need these.
- **Interactive or scripted single-file operations** — where `cp` simplicity and immediate exit semantics are preferred.

## Copy Modes Explained

### CopyOnly
Standard async I/O copy. Best for initial replication.

### CksumCopy
First checks file size and modification time; if both match the destination, the entire file is skipped. Otherwise reads source blocks and compares checksums with the corresponding destination blocks — only differing blocks are written. Useful for incremental sync of large files where most data is unchanged.

### CksumOnly
Same comparison logic as `CksumCopy`, but never writes. Mismatches are emitted as NDJSON `cksum_result` events through the FileLog system. Enable `FileLog` to capture them:

```json
{"type":"file_info","event":"cksum_result","src":"/path/to/src","dst":"/path/to/dst","result":"mismatch","reason":"checksum_mismatch","offset":1048576,"detail":"xxhash64","timestamp":"2026-04-25T12:34:56Z"}
```

## Testing

```bash
# Run all tests
ctest --test-dir build --output-on-failure

# Or run the test binary directly
./build/test_acp

# Exclude slow integration tests (large dataset)
./build/test_acp "~[integration]"

# Run with JSON reporter and parse results
./build/test_acp --reporter json -s -d yes > test_results.json
python3 tests/parse_test_results.py test_results.json
```

Tests cover `Channel`, `CPFilePair`, `CPFilePairMgr`, `IOSlot`, `CksumCopy`, `CksumOnly`, `liburing`, `DirectIO`, `SyncWrites`, `PreserveMeta`, `Inotify`, `Watchdog`, and end-to-end integration scenarios.

## Architecture

```
main.cpp
  ├── MergeCopyOptions()  →  defaults → /etc → ~ → ./ → CLI
  ├── CopyDir() / CopyFile()
  │     ├── Channel<CopyEntry> (producer)
  │     └── CopyEngine
  │           ├── CPFilePairMgr (per-thread)
  │           └── IOSlotMgr
  │                 ├── AIOSlotMgr  (libaio)
  │                 ├── UIOSlotMgr  (io_uring)
  │                 └── GCDSlotMgr  (macOS GCD)
  │           └── Watchdog (stuck I/O detection)
  └── Inotify / FSEvents (optional, live sync)
```

- **MergeCopyOptions**: Layered config resolution — hardcoded defaults → `/etc/acp_config.json` → `~/.acp_config.json` → `./acp_config.json` → CLI flags.
- **CopyEngine**: Main thread distributes `CopyEntry` to worker threads in round-robin fashion.
- **CPFilePairMgr**: Manages file pair lifecycle via `FPChannel` (pending → inflight → completed).
- **IOSlotMgr**: Abstract template base with shared completion logic (`HandleReadCompletion`, `HandleWriteCompletion`). Backends implement `Init`, `DoPrepareOneRead`, `SubmitOneRead`, `IOReap`, etc.
- **Watchdog**: Monitors per-slot I/O timestamps; aborts the copy if any slot remains stuck beyond `IOStuckTimeout`.
- **IOSlot**: State machine (`Init → ReadPrepared → ReadSubmitted → ReadReaped → WritePrepared → WriteSubmitted → WriteReaped`).

## Performance Notes

- **io_uring** generally outperforms `libaio` on Linux 5.1+ due to reduced syscall overhead.
- **Direct I/O** is beneficial for large sequential workloads but requires sector-aligned I/O sizes.
- Increase `QueueDepth` and `CopyParallelism` for high-IOPS storage (NVMe SSDs, RAID arrays).
- **Do NOT set `QueueDepth` to an excessively large value** (e.g., hundreds of thousands or more). On Linux, overly large queue depths can cause `io_setup` to hang or exhaust kernel resources instead of returning a clean error. Stay within reasonable bounds (tens to low hundreds).
- `CksumCopy` adds read amplification on the destination side; use when write bandwidth is the bottleneck. The size+mtime fast-path skips unchanged files entirely, eliminating read amplification for files that have not changed.
- **Watchdog** (`IOStuckTimeout`) protects against hung I/O on flaky storage or kernel driver bugs. Set to a value well above your expected max I/O latency (e.g., `30` for seconds). Disable with `0` when running under debuggers or on systems with intentionally variable I/O latency.

### macOS GCD Backend Caveat

The macOS backend uses **Grand Central Dispatch (GCD)** with `dispatch_group_async` + `pread`/`pwrite` to *emulate* asynchronous I/O. Unlike Linux `io_uring` or `libaio`, which queue and complete I/O requests inside the kernel, GCD dispatches blocking syscalls to a thread pool. This means:

- The macOS backend is **"pseudo-async"** — it achieves concurrency through threads, not true async I/O.
- For large sequential copies, throughput may be comparable to Linux, but **latency and CPU overhead are higher** due to thread management.
- On macOS, `acp` will not outperform `cp` as dramatically as `io_uring` does on Linux.

## License

See LICENSE file
