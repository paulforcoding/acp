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
- **Sparse File Preservation** — detect and preserve holes (like `cp --sparse=auto`)
- **Metadata Preservation** — preserve timestamps, mode, ownership, xattr, and ACL (optional via `PreserveMeta`)
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
| Field | Description | Default |
|-------|-------------|---------|
| `ProgramLogLevel` | `"trace"`, `"debug"`, `"info"`, `"warn"`, `"error"`, `"fatal"` | `"info"` |
| `ProgramLogMode` | `"console"` or `"file"` | `"file"` |
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

## When to Use / When NOT to Use

### Use `acp` when

- **Large-scale data migration** — copying millions of files or terabyte-scale datasets. The async I/O engine and multi-threaded pipeline amortize the fixed startup cost and saturate storage bandwidth.
- **Very large single files** — sequential read/write of multi-GB files where queue depth and parallelism matter.
- **Incremental sync with checksums** — `CksumCopy` / `CksumOnly` for block-level deduplication when most data is unchanged.
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

# Run with JSON reporter and parse results
./build/test_acp --reporter json -s -d yes > test_results.json
python3 tests/parse_test_results.py test_results.json
```

Tests cover `Channel`, `CPFilePair`, `CPFilePairMgr`, `IOSlot`, `CksumCopy`, `CksumOnly`, `liburing`, `DirectIO`, `SyncWrites`, `PreserveMeta`, `Inotify`, and end-to-end integration scenarios.

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

### macOS GCD Backend Caveat

The macOS backend uses **Grand Central Dispatch (GCD)** with `dispatch_group_async` + `pread`/`pwrite` to *emulate* asynchronous I/O. Unlike Linux `io_uring` or `libaio`, which queue and complete I/O requests inside the kernel, GCD dispatches blocking syscalls to a thread pool. This means:

- The macOS backend is **"pseudo-async"** — it achieves concurrency through threads, not true async I/O.
- For large sequential copies, throughput may be comparable to Linux, but **latency and CPU overhead are higher** due to thread management.
- On macOS, `acp` will not outperform `cp` as dramatically as `io_uring` does on Linux.
- If maximum throughput on macOS is critical, consider using `rsync` or `cp` with APFS clone copies (`cp -c`) instead.

## License

TBD
