# acp — Asynchronous File Copy Tool

**acp** = **a**sync **cp**

High-performance file copy using Linux AIO / io_uring and macOS Grand Central Dispatch, with optional inotify/FSEvents directory monitoring and block-level checksum verification.

## Project Vision

1. **acp = async cp = agent cp** — an **Agent-First** high-performance file copy tool.
2. Designed to **functionally replace native `cp`** with superior throughput via async I/O. CLI usage aligns with standard `cp`, but features that hinder agent observability or ergonomics are intentionally omitted — we do not sacrifice the agent experience for 100% `cp` compatibility.
3. **cp-aligned behavior** — command-line usage, path handling, and exit semantics mirror the standard `cp` command where practical.
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
  - `CksumCopy` — block-level deduplication with fast size+mtime pre-check; only differing blocks are written
  - `CksumOnly` — verify without writing, results logged to `./cksum_result.log`
- **CLI11 Argument Parsing** — override any config field via command-line flags, layered on top of JSON config files
- **Layered Configuration** — config resolution: hardcoded defaults → `/etc/acp_config.json` → `~/.acp_config.json` → `./acp_config.json` → CLI flags (last wins)
- **I/O Watchdog** — automatic detection and abort of stuck I/O operations (`IOStuckTimeout`)
- **Live Directory Sync** — optional `inotify` (Linux) / `FSEvents` (macOS) monitoring for continuous replication
- **Direct I/O** support — bypass page cache for large sequential workloads
- **Sparse File Preservation** — detect and preserve holes (like `cp --sparse=auto`)
- **Metadata Preservation** — preserve timestamps, mode, ownership, xattr, and ACL (optional via `PreserveMeta`); directory metadata is deferred until after all files complete
- **Dual Logging System** — independent `ProgramLog` (diagnostics) and `FileLog` (per-file progress telemetry) with separate mode/file-path controls
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

### Configuration Fields

| Field | Description | Default |
|-------|-------------|---------|
| `ProgramLogLevel` | `"trace"`, `"debug"`, `"info"`, `"warn"`, `"error"`, `"fatal"` | `"info"` |
| `ProgramLogMode` | `"console"` or `"file"` | `"console"` |
| `ProgramLogFilePath` | Program log file path (mode=file) | `"/tmp/acp_program.log"` |
| `FileLogEnabled` | Enable structured file progress events | `false` |
| `FileLogMode` | `"console"` or `"file"` | `"file"` |
| `FileLogIntervalSec` | Progress summary interval in seconds | `5` |
| `FileLogPath` | NDJSON event log path (file mode) | `"/tmp/acp_file_info.json"` |
| `CopyEngine` | `"libaio"`, `"liburing"` (Linux), `"gcd"` (macOS) | `"libaio"` |
| `CopyMode` | `"CopyOnly"`, `"CksumCopy"`, `"CksumOnly"` | `"CopyOnly"` |
| `CksumAlgorithm` | `"xxhash64"`, `"md5"`, `"sha256"` | `"xxhash64"` |
| `CopyParallelism` | Number of concurrent copy threads | `1` |
| `IOSize` | Per-I/O read/write size in bytes | `1048576` (1 MiB) |
| `QueueDepth` | Max in-flight I/Os per thread | `8` |
| `Batch` | I/Os submitted per batch | `8` |
| `IOReapWait` | I/O completion wait timeout (seconds) | `1` |
| `IOStuckTimeout` | Stuck I/O detection timeout (seconds, `0` = off) | `0` |
| `DirectIO` | Use `O_DIRECT` bypassing page cache | `false` |
| `SyncWrites` | `fsync` each file after completion | `false` |
| `PreserveSparseFiles` | Preserve sparse file holes | `true` |
| `PreserveMeta` | Preserve timestamps, mode, ownership, xattr, ACL | `false` |
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

### Command-Line Options

All config fields can be overridden via CLI flags. Layered resolution: hardcoded defaults → `/etc/acp_config.json` → `~/.acp_config.json` → `./acp_config.json` → CLI flags (last wins).

```bash
# General options
./acp --version                    # Show version (v0.5.0)
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
- **Cross-network copies** — `acp` is a pure local-file copy tool. It does not speak `scp`, `rsync`, `sftp`, or any network protocol.
- **Special file replication** — device files, sockets, FIFOs, and whiteout files are skipped (logged as unsupported). Use `cp -a` or `rsync` if you need these.
- **Archive-level metadata fidelity** — while `PreserveMeta` preserves basic metadata (timestamps, mode, ownership, xattr, ACL), it may not match `cp -a` or `rsync -a` in every edge case (e.g., SELinux contexts, sub-second precision on all filesystems).
- **Interactive or scripted single-file operations** — where `cp` simplicity and immediate exit semantics are preferred.

## Copy Modes Explained

### CopyOnly
Standard async I/O copy. Best for initial replication.

### CksumCopy
First checks file size and modification time; if both match the destination, the entire file is skipped. Otherwise, reads source blocks and compares checksums with the corresponding destination blocks — only differing blocks are written. Useful for incremental sync of large files where most data is unchanged.

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

# Run with JSON reporter and parse results
./build/test_acp --reporter json -s -d yes > test_results.json
python3 tests/parse_test_results.py test_results.json
```

Tests cover `Channel`, `CPFilePair`, `CPFilePairMgr`, `IOSlot`, `CksumCopy`, `CksumOnly`, `liburing`, `DirectIO`, `SyncWrites`, `PreserveMeta`, `Inotify`, `Watchdog`, and end-to-end integration scenarios.

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
- `CksumCopy` adds read amplification on the destination side; use when write bandwidth is the bottleneck. The size+mtime fast-path skips unchanged files entirely, eliminating read amplification for files that have not changed.
- **Watchdog** (`IOStuckTimeout`) protects against hung I/O on flaky storage or kernel driver bugs. Set to a value well above your expected max I/O latency (e.g., `30` for seconds). Disable with `0` when running under debuggers or on systems with intentionally variable I/O latency.

### macOS GCD Backend Caveat

The macOS backend uses **Grand Central Dispatch (GCD)** with `dispatch_group_async` + `pread`/`pwrite` to *emulate* asynchronous I/O. Unlike Linux `io_uring` or `libaio`, which queue and complete I/O requests inside the kernel, GCD dispatches blocking syscalls to a thread pool. This means:

- The macOS backend is **"pseudo-async"** — it achieves concurrency through threads, not true async I/O.
- For large sequential copies, throughput may be comparable to Linux, but **latency and CPU overhead are higher** due to thread management.
- On macOS, `acp` will not outperform `cp` as dramatically as `io_uring` does on Linux.
- If maximum throughput on macOS is critical, consider using `rsync` or `cp` with APFS clone copies (`cp -c`) instead.

## License

TBD
