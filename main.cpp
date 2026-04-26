#include <iostream>
#include <vector>
#include <filesystem>
#include <algorithm>
#include "lib/mainlib.hpp"
#include "lib/thirdparty/CLI11.hpp"

constexpr const char* kVersion = "0.5.1";

#ifdef __APPLE__
constexpr const char* kPlatform = "macOS";
#elif defined(__linux__)
constexpr const char* kPlatform = "Linux";
#else
constexpr const char* kPlatform = "Unknown";
#endif

#if defined(__arm64__) || defined(__aarch64__)
constexpr const char* kArch = "arm64";
#elif defined(__x86_64__)
constexpr const char* kArch = "x86_64";
#else
constexpr const char* kArch = "unknown";
#endif

#ifdef __clang__
constexpr const char* kCompiler = "Clang " __clang_version__;
#elif defined(__GNUC__)
constexpr const char* kCompiler = "GCC " __VERSION__;
#else
constexpr const char* kCompiler = "Unknown";
#endif

static void PrintVersion()
{
    std::cout <<
        "acp version " << kVersion << "\n"
        "\n"
        "BUILD\n"
        "    Platform     " << kPlatform << "\n"
        "    Architecture " << kArch << "\n"
        "    Compiler     " << kCompiler << "\n"
        "\n"
        "SUPPORTED ENGINES\n"
#ifdef __APPLE__
        "    GCD          yes   (default)\n"
        "    libaio       no\n"
        "    liburing     no\n"
#else
        "    libaio       yes   (default)\n"
#ifdef ENABLE_LIBURING
        "    liburing     yes\n"
#else
        "    liburing     no    (build with -DENABLE_LIBURING=ON)\n"
#endif
        "    GCD          no\n"
#endif
        "\n";
}

static void PrintHelp(const char* /*program_name*/)
{
    std::cout <<
        "# acp — async cp = agent cp\n"
        "\n"
        "High-performance async file copy for Linux/macOS.\n"
        "\n"
        "## 功能场景\n"
        "\n"
        "### 全量复制 (CopyOnly)\n"
        "\n"
        "```\n"
        "acp [OPTIONS] <src>... <dst>\n"
        "```\n"
        "\n"
        "将源复制到目标。行为与 `cp` 类似。支持多源复制和 inotify 持续监控。\n"
        "\n"
        "### 数据校验 (CksumOnly)\n"
        "\n"
        "```\n"
        "acp --mode=CksumOnly <src> <dst>\n"
        "```\n"
        "\n"
        "校验源与目标的数据差异，不执行写入。始终做逐块校验，不做 mtime+size 快速跳过。\n"
        "源和目标必须同为文件或同为目录，且必须都存在。\n"
        "路径语义：`acp <src> <dst>` 校验 `<src>` 与 `<dst>`，而非 `<dst>/<src>`。\n"
        "\n"
        "### 增量复制 (CksumCopy)\n"
        "\n"
        "```\n"
        "acp --mode=CksumCopy <src> <dst>\n"
        "```\n"
        "\n"
        "校验源与目标的差异，仅写入差异数据。当 size+mtime 一致时快速跳过，仅对不一致的文件做逐块校验。\n"
        "注意：快速跳过仅检查 size+mtime，不检测权限/属主/xattr/ACL 等其他元数据差异。\n"
        "如需检测元数据差异，请使用 CksumOnly 模式。\n"
        "源和目标必须同为文件或同为目录，且必须都存在。\n"
        "路径语义：`acp <src> <dst>` 校验并复制 `<src>` 与 `<dst>`，而非 `<dst>/<src>`。\n"
        "\n"
        "## 使用场景\n"
        "\n"
        "| 场景 | 特点 | 推荐 CopyMode |\n"
        "|------|------|----------------|\n"
        "| 个人日常 | 安静输出，行为贴近 `cp` | CopyOnly |\n"
        "| 企业数据迁移 | 结构化日志，进度可观测 | CopyOnly 或 CksumCopy |\n"
        "\n"
        "## 使用例子\n"
        "\n"
        "```bash\n"
        "# 复制文件\n"
        "acp file.txt /backup/file.txt\n"
        "\n"
        "# 复制目录\n"
        "acp mydir/ /backup/\n"
        "\n"
        "# 多源复制\n"
        "acp a.txt b.txt /backup/\n"
        "\n"
        "# 校验两个目录的数据差异\n"
        "acp --mode=CksumOnly /data/ /backup/\n"
        "\n"
        "# 增量复制（仅写入差异数据）\n"
        "acp --mode=CksumCopy /data/ /backup/\n"
        "\n"
        "# 预览当前生效的配置，不执行复制\n"
        "acp --dry-run /data/ /backup/\n"
        "```\n"
        "\n"
        "## 日志\n"
        "\n"
        "- **ProgramLog**：程序运行状态日志，可设为 console 或 file\n"
        "- **FileLog**：文件复制/校验结果日志，NDJSON 格式（每行一个 JSON 对象），可设为 console 或 file。\n"
        "  CksumOnly / CksumCopy 模式自动启用；CopyOnly 模式需手动启用（`--enable-file-log`）。\n"
        "\n"
        "### FileLog 使用方法\n"
        "\n"
        "```bash\n"
        "# CopyOnly 模式启用 FileLog 并输出到文件\n"
        "acp --enable-file-log --file-log-mode=file --file-log-path=/tmp/acp_file.jsonl /data/ /backup/\n"
        "\n"
        "# 实时查看复制进度（file 模式）\n"
        "tail -f /tmp/acp_file.jsonl | jq 'select(.event==\"progress_summary\") | {files_done, bytes_done, speed_mbps_avg, eta_seconds}'\n"
        "\n"
        "# 实时查看复制进度（console 模式）\n"
        "acp --enable-file-log --file-log-mode=console /data/ /backup/ 2>/dev/null | jq 'select(.event==\"progress_summary\")'\n"
        "\n"
        "# 查看校验结果（CksumOnly / CksumCopy 模式）\n"
        "cat /tmp/acp_file.jsonl | jq 'select(.event==\"cksum_result\")'\n"
        "\n"
        "# 检查是否有错误\n"
        "cat /tmp/acp_file.jsonl | jq 'select(.event==\"file_error\")'\n"
        "\n"
        "# 查看最终汇总\n"
        "cat /tmp/acp_file.jsonl | jq 'select(.event==\"copy_complete\")'\n"
        "```\n"
        "\n"
        "### FileLog 事件类型\n"
        "\n"
        "| 事件 | 说明 | 出现时机 |\n"
        "|------|------|----------|\n"
        "| `copy_plan` | 扫描结果汇总 | 复制开始前 |\n"
        "| `file_start` | 文件开始复制 | 每个文件 |\n"
        "| `file_complete` | 文件复制完成 | 每个文件 |\n"
        "| `file_error` | 文件复制错误 | 出错时 |\n"
        "| `file_unsupported` | 不支持的文件类型 | 跳过时 |\n"
        "| `meta_warning` | 元数据保留警告 | 保留失败时 |\n"
        "| `cksum_result` | 校验结果 | CksumOnly / CksumCopy 模式 |\n"
        "| `progress_summary` | 进度摘要 | 每 intervalSec 秒 |\n"
        "| `state_snapshot` | 状态快照 | 每 intervalSec 秒 |\n"
        "| `copy_complete` | 复制完成汇总 | 复制结束时 |\n"
        "| `stuck_detected` | I/O 卡住检测 | I/O 超时时 |\n"
        "\n"
        "各事件的完整字段说明见 `acp --help-all`。\n"
        "\n"
        "## 配置加载顺序（后者覆盖前者）\n"
        "\n"
        "1. 内置默认值\n"
        "2. `/etc/acp_config.json`\n"
        "3. `~/acp_config.json`\n"
        "4. `./acp_config.json`\n"
        "5. 命令行参数\n"
        "\n"
        "## 退出码\n"
        "\n"
        "| 码 | 含义 |\n"
        "|----|------|\n"
        "| 0  | 成功 |\n"
        "| 1  | 配置错误、路径错误或复制失败 |\n";
}

static void PrintHelpAllDesignDoc()
{
    std::cout << R"HELPALL(
# acp 场景说明

## 1. 功能场景

acp 支持三种功能场景，由 `CopyMode` 配置项决定：

| 场景 | CopyMode 值 | 说明 |
|------|-------------|------|
| 全量复制 | `CopyOnly` | 从源读取并写入目标，行为与 `cp` 类似 |
| 数据校验 | `CksumOnly` | 校验源与目标的差异，不写入，始终逐块校验（不做 mtime+size 快速跳过），结果记录到 FileLog（自动启用） |
| 增量复制 | `CksumCopy` | 校验源与目标的差异，仅写入差异数据（mtime+size 一致时快速跳过，仅对不一致文件逐块校验；快速跳过不检测权限/属主/xattr/ACL 等元数据差异），校验结果记录到 FileLog（自动启用） |

### 1.1 全量复制（CopyOnly）

**命令行用法：** 与当前保持不变。

```
acp <src>... <dst>
```

**路径语义：** 与当前保持不变 — `acp <src> <dst>` 将 `<src>` 复制到 `<dst>/<src>`。

**ProgramLog：** 当前行为保持不变。

**FileLog：** 当前行为保持不变。

**EnableInotify：** 支持。

**多源复制：** 支持。

### 1.2 数据校验（CksumOnly）

**命令行用法：**

```
acp <src> <dst>
```

约束：

- 源端和目标端必须同时为目录或同时为文件，且必须都存在。
- 路径语义与全量复制不同：`acp <src> <dst>` 校验的是 `<src>` 与 `<dst>` 的对应关系，**不是** `<src>` 与 `<dst>/<src>` 的对应关系。
- 始终逐块校验文件内容，**不做 mtime+size 快速跳过**。即使源和目标的 size/mtime 完全一致，也会读取文件数据做块级校验。这确保不会因元数据碰巧一致而漏检内容损坏。

**ProgramLog：** 与全量复制相同。

**FileLog：** 自动启用。仅输出文件对比结果事件和校验进度性能事件，其他事件不输出。

**EnableInotify：** 不支持。

**多源数据校验：** 不支持。

### 1.3 增量复制（CksumCopy）

**命令行用法：**

```
acp <src> <dst>
```

约束：

- 源端和目标端必须同时为目录或同时为文件，且必须都存在。
- 路径语义与全量复制不同：`acp <src> <dst>` 校验并复制差异数据的是 `<src>` 与 `<dst>` 的对应关系，**不是** `<src>` 与 `<dst>/<src>` 的对应关系。
- 当源和目标的 size+mtime 一致时，快速跳过不做逐块校验。仅对 size 或 mtime 不一致的文件做逐块校验并写入差异数据。这在大多数场景下能大幅减少 I/O，但若 mtime 被人为恢复或碰巧一致，可能漏检内容差异。
- 快速跳过**仅检查 size+mtime**，不检测权限、属主、xattr、ACL 等其他元数据差异。即使权限或属主不同，只要 size+mtime 一致，CksumCopy 仍会跳过。如需检测元数据差异，请使用 CksumOnly 模式。

**ProgramLog：** 与全量复制相同。

**FileLog：** 保持当前行为。

**EnableInotify：** 不支持。

**多源增量复制：** 不支持。

---

## 2. 日志系统

acp 有两套日志系统：

| 日志 | 用途 | 输出内容 |
|------|------|----------|
| **ProgramLog** | 程序自身运行状态 | 程序启动、配置加载、错误等 |
| **FileLog** | 文件复制/校验状态 | 文件复制结果、校验结果、进度性能 |

两种日志均可设置输出为 `console` 或 `file`（可指定路径），具体设置方法见 `acp --help-all`。

---

## 3. FileLog NDJSON 事件格式

FileLog 输出 NDJSON 格式（每行一个 JSON 对象），每行包含 `"type":"file_info"` 和 `"event"` 字段。
所有事件均包含 `"timestamp"` 字段（ISO 8601 格式）。

### 3.1 copy_plan — 扫描结果汇总

复制开始前发出，报告源路径扫描结果。

```json
{"type":"file_info","event":"copy_plan","scan_state":"completed","files_total":30,"dirs_total":1,"symlinks_total":0,"bytes_total":314572800,"files_regular":30,"files_unsupported":0,"timestamp":"..."}
```

| 字段 | 类型 | 说明 |
|------|------|------|
| scan_state | string | `"completed"` |
| files_total | int | 源端文件总数 |
| dirs_total | int | 源端目录总数 |
| symlinks_total | int | 源端符号链接总数 |
| bytes_total | int | 源端文件总字节数 |
| files_regular | int | 普通文件数 |
| files_unsupported | int | 不支持的文件类型数 |

### 3.2 file_start — 文件开始复制

```json
{"type":"file_info","event":"file_start","src":"/data/a.txt","dst":"/backup/a.txt","size":10485760,"timestamp":"..."}
```

| 字段 | 类型 | 说明 |
|------|------|------|
| src | string | 源文件路径 |
| dst | string | 目标文件路径 |
| size | int | 文件大小（字节） |

### 3.3 file_complete — 文件复制完成

```json
{"type":"file_info","event":"file_complete","src":"/data/a.txt","dst":"/backup/a.txt","size":10485760,"bytes_written":10485760,"duration_ms":3,"speed_mbps":3333.3,"timestamp":"..."}
```

| 字段 | 类型 | 说明 |
|------|------|------|
| src | string | 源文件路径 |
| dst | string | 目标文件路径 |
| size | int | 文件大小（字节） |
| bytes_written | int | 实际写入字节数 |
| duration_ms | int | 复制耗时（毫秒） |
| speed_mbps | float | 单文件平均速度（MB/s） |

### 3.4 file_error — 文件复制错误

```json
{"type":"file_info","event":"file_error","src":"/data/a.txt","error":"Permission denied","errno":13,"action":"open","timestamp":"..."}
```

| 字段 | 类型 | 说明 |
|------|------|------|
| src | string | 源文件路径 |
| error | string | 错误描述 |
| errno | int | errno 值 |
| action | string | 出错操作（`open`/`read`/`write`/`truncate`/...） |

### 3.5 file_unsupported — 不支持的文件类型

```json
{"type":"file_info","event":"file_unsupported","src":"/dev/sda1","type":"block_device","action":"skip","timestamp":"..."}
```

### 3.6 meta_warning — 元数据保留警告

```json
{"type":"file_info","event":"meta_warning","src":"/data/a.txt","meta_type":"xattr","error":"Operation not supported","errno":95,"timestamp":"..."}
```

### 3.7 cksum_result — 校验结果（CksumOnly / CksumCopy）

```json
{"type":"file_info","event":"cksum_result","src":"/data/a.txt","dst":"/backup/a.txt","result":"match","content":"match","meta":"mismatch","meta_details":["mtime","uid"],"timestamp":"..."}
```

| 字段 | 类型 | 说明 |
|------|------|------|
| src | string | 源文件路径 |
| dst | string | 目标文件路径 |
| result | string | 总体结果：`"match"` 或 `"mismatch"` |
| content | string | 内容校验结果：`"match"` / `"mismatch"` / `"skipped"` |
| meta | string | 元数据校验结果：`"match"` / `"mismatch"` |
| meta_details | array | 差异元数据项列表，如 `["mtime","uid","mode"]` |

### 3.8 progress_summary — 进度摘要

每 `FileLogIntervalSec` 秒发出一次。

```json
{"type":"file_info","event":"progress_summary","files_total":30,"files_done":10,"bytes_total":314572800,"bytes_done":104857600,"speed_mbps_avg":1250.0,"eta_seconds":168,"current_file":"/data/medium_11.bin","timestamp":"..."}
```

| 字段 | 类型 | 说明 |
|------|------|------|
| files_total | int | 文件总数 |
| files_done | int | 已完成文件数 |
| bytes_total | int | 总字节数 |
| bytes_done | int | 已完成字节数 |
| speed_mbps_avg | float | 全局平均速度（MB/s） |
| eta_seconds | int | 预计剩余时间（秒），0 表示无法估算 |
| current_file | string | 当前正在复制的文件路径 |

### 3.9 state_snapshot — 状态快照

每 `FileLogIntervalSec` 秒发出一次。file 模式追加到日志文件；console 模式原子覆盖写入状态文件。

```json
{"type":"file_info","event":"state_snapshot","pid":12345,"state":"running","start_time":"2026-04-26T04:20:22.563Z","files_total":30,"files_done":10,"bytes_total":314572800,"bytes_done":104857600,"current_speed_mbps":1250.0,"current_file":"/data/medium_11.bin","last_update":"2026-04-26T04:20:27.563Z"}
```

| 字段 | 类型 | 说明 |
|------|------|------|
| pid | int | 进程 ID |
| state | string | `"running"` 或 `"completed"` |
| start_time | string | 复制开始时间（ISO 8601） |
| files_total | int | 文件总数 |
| files_done | int | 已完成文件数 |
| bytes_total | int | 总字节数 |
| bytes_done | int | 已完成字节数 |
| current_speed_mbps | float | 全局平均速度（MB/s） |
| current_file | string | 当前正在复制的文件路径 |
| last_update | string | 最后更新时间（ISO 8601） |

### 3.10 copy_complete — 复制完成汇总

复制结束时发出。

```json
{"type":"file_info","event":"copy_complete","files_total":30,"files_done":31,"bytes_total":314572800,"bytes_done":314572800,"duration_ms":79,"speed_mbps_avg":3797.5,"timestamp":"..."}
```

| 字段 | 类型 | 说明 |
|------|------|------|
| files_total | int | 文件总数 |
| files_done | int | 已完成文件数（含目录，可能 > files_total） |
| bytes_total | int | 总字节数 |
| bytes_done | int | 已完成字节数 |
| duration_ms | int | 总耗时（毫秒） |
| speed_mbps_avg | float | 全局平均速度（MB/s） |

### 3.11 stuck_detected — I/O 卡住检测

I/O 超过 `IOStuckTimeout` 秒无进展时发出。

```json
{"type":"file_info","event":"stuck_detected","round":5000,"stuck_seconds":30,"files_total":30,"files_done":10,"bytes_total":314572800,"bytes_done":104857600,"current_file":"/data/large.bin","timestamp":"..."}
```

### 3.12 CksumOnly 过滤模式

CksumOnly 模式下，FileLog 仅输出以下事件，其余事件被过滤：
`cksum_result`、`progress_summary`、`copy_complete`、`copy_plan`、`state_snapshot`

---

## 4. 使用场景与配置示例

### 4.1 个人日常使用

```json
{
  "ProgramLogLevel": "error",
  "ProgramLogMode": "console",
  "ProgramLogFilePath": "/tmp/acp_program.log",
  "FileLogEnabled": false,
  "FileLogMode": "console",
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

| 配置项 | 值 | 说明 |
|--------|-----|------|
| ProgramLogLevel | `error` | 一般人对复制过程不感兴趣 |
| ProgramLogMode | `console` | 有报错直接输出，和 `cp` 行为一致 |
| FileLogEnabled | `false` | 无输出即表示全部复制成功，和 `cp` 行为一致；CksumOnly / CksumCopy 模式自动启用；AI agent 运行时设为 `true`，配合 `FileLogMode: "file"` 和 `FileLogPath`，可在复制过程中查询进度 |
| CopyEngine | `liburing` / `libaio` | Linux 选 `liburing` 或 `libaio`，macOS 只能选 `GCD` |
| CksumAlgorithm | `xxhash64` | 默认即可 |
| CopyParallelism | `1` | 单线程异步复制已足够快 |
| CopyChanSize | `100` | 无需占用过多系统资源 |
| EnableInotify | `false` | 和 `cp` 行为一致 |
| PreserveSparseFiles | `true` | 和 `cp` 行为一致 |
| PreserveMeta | `true` | 和 `cp` 行为一致 |
| DirectIO | `false` | 和 `cp` 行为一致 |
| SyncWrites | `true` | 与 `cp` 不同：acp 默认 `fsync()`，降低数据丢失风险 |
| IOSize × QueueDepth | 128KB × 8 | 总缓冲 1MB，按内存大小可适当增大，不超过内存容量 |
| Batch | `8` | 每批 IO 提交数量，对性能影响不大 |
| IOReapWait | `1` | 一般无需调整 |

### 4.2 企业数据迁移

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

| 配置项 | 值 | 说明 |
|--------|-----|------|
| ProgramLogLevel | `error` | 同上 |
| ProgramLogMode | `file` | 保留日志供运维人员排查 |
| FileLogEnabled | `true` | 设置为 `true`，配合 `FileLogMode: "file"` 和 `FileLogPath`，复制前清除文件，复制中可查询进度；CksumOnly / CksumCopy 模式会自动启用 |
| CopyEngine | `liburing` / `libaio` | Linux 选 `liburing` 或 `libaio`，macOS 只能选 `GCD` |
| CksumAlgorithm | `xxhash64` | 默认即可 |
| CopyParallelism | `1`–`8` | 异步复制 1 通常够快；性能峰值一般在 1–8 之间，过大收益递减 |
| CopyChanSize | `1000+` | 建议更大 |
| EnableInotify | 视需求 | 根据是否需要持续复制而定 |
| PreserveSparseFiles | `true` | 和 `cp` 行为一致 |
| PreserveMeta | `true` | 和 `cp` 行为一致；去掉可加快复制 |
| DirectIO | `false` / `true` | 某些介质/文件系统开启 `DirectIO` 性能更好；与 `SyncWrites` 互斥，二选一为 `true` |
| SyncWrites | `false` / `true` | 与 `DirectIO` 互斥，二选一为 `true` |
| IOSize × QueueDepth | 256KB × 32 | 总缓冲 8MB，可按可用内存增大 |
| Batch | `8` | 同上 |
| IOReapWait | `1` | 同上 |
)HELPALL";
}

struct CliOptions {
    std::optional<std::string> optLogLevel, optLogMode, optLogFile;
    std::optional<std::string> optFileLogMode, optFileLogPath;
    std::optional<std::string> optEngine, optMode, optCksumAlgo;
    std::optional<int> optFileLogInterval, optParallelism, optChanSize;
    std::optional<size_t> optIoSize, optQueueDepth;
    std::optional<int> optBatch, optIoReapWait, optIoStuckTimeout;
    bool enableFileLog = false, disableFileLog = false;
    bool enableInotify = false, disableInotify = false;
    bool enableSparse = false, disableSparse = false;
    bool enablePreserveMeta = false, disablePreserveMeta = false;
    bool enableDirectIO = false, disableDirectIO = false;
    bool enableSync = false, disableSync = false;
    bool dryRun = false;
    std::vector<std::string> positional;
};

static void SetupCliOptions(CLI::App& app, CliOptions& opts)
{
    app.add_option("--log-level,-L", opts.optLogLevel, "Program log level (trace/debug/info/warn/error/critical)");
    app.add_option("--log-mode", opts.optLogMode, "Program log mode (console/file)");
    app.add_option("--log-file", opts.optLogFile, "Program log file path");
    app.add_option("--file-log-mode", opts.optFileLogMode, "File log mode (console/file)");
    app.add_option("--file-log-path", opts.optFileLogPath, "File log output path");
    app.add_option("--engine,-e", opts.optEngine, "Copy engine (libaio/liburing)")
        ->check(CLI::IsMember({"libaio", "liburing", "gcd"}));
    app.add_option("--mode,-m", opts.optMode, "Copy mode (CopyOnly/CksumCopy/CksumOnly)")
        ->check(CLI::IsMember({"CopyOnly", "CksumCopy", "CksumOnly"}));
    app.add_option("--cksum-algo,-a", opts.optCksumAlgo, "Checksum algorithm (xxhash64/md5/sha256)")
        ->check(CLI::IsMember({"xxhash64", "md5", "sha256"}));

    app.add_option("--file-log-interval", opts.optFileLogInterval, "File log flush interval (sec)")->check(CLI::PositiveNumber);
    app.add_option("--parallelism,-p", opts.optParallelism, "Number of copy worker threads")->check(CLI::PositiveNumber);
    app.add_option("--chan-size", opts.optChanSize, "Channel capacity for file dispatch")->check(CLI::PositiveNumber);
    app.add_option("--io-size", opts.optIoSize, "I/O unit size in bytes")->check(CLI::PositiveNumber);
    app.add_option("--queue-depth,-q", opts.optQueueDepth, "Max in-flight I/Os per worker")->check(CLI::PositiveNumber);
    app.add_option("--batch,-b", opts.optBatch, "I/Os submitted per batch")->check(CLI::PositiveNumber);
    app.add_option("--io-reap-wait,-w", opts.optIoReapWait, "I/O completion wait timeout (sec)")->check(CLI::PositiveNumber);
    app.add_option("--io-stuck-timeout,-t", opts.optIoStuckTimeout, "Stuck I/O detection timeout (sec, 0=off)")->check(CLI::NonNegativeNumber);

    app.add_flag("--enable-file-log", opts.enableFileLog, "Enable file-level NDJSON logging");
    app.add_flag("--disable-file-log", opts.disableFileLog, "Disable file-level NDJSON logging");
    app.add_flag("--enable-inotify", opts.enableInotify, "Enable source directory monitoring");
    app.add_flag("--disable-inotify", opts.disableInotify, "Disable source directory monitoring");
    app.add_flag("--enable-sparse", opts.enableSparse, "Preserve sparse file holes");
    app.add_flag("--disable-sparse", opts.disableSparse, "Do not preserve sparse file holes");
    app.add_flag("--enable-preserve-meta", opts.enablePreserveMeta, "Preserve file metadata");
    app.add_flag("--disable-preserve-meta", opts.disablePreserveMeta, "Do not preserve file metadata");
    app.add_flag("--enable-direct-io", opts.enableDirectIO, "Use O_DIRECT for unbuffered I/O");
    app.add_flag("--disable-direct-io", opts.disableDirectIO, "Use buffered I/O");
    app.add_flag("--enable-sync", opts.enableSync, "Sync data to disk after each write");
    app.add_flag("--disable-sync", opts.disableSync, "Do not force sync after writes");
    app.add_flag("--dry-run", opts.dryRun, "Show effective config and mode without executing");

    app.add_option("paths", opts.positional, "Source and destination paths")
       ->expected(-1);
}

static void PrintHelpAllCliOptions()
{
    CLI::App app{"acp — async cp, high-performance file copy"};
    CliOptions opts;
    SetupCliOptions(app, opts);
    std::cout << "\n---\n\n# 命令行选项\n\n";
    std::cout << app.help("", CLI::AppFormatMode::All) << std::endl;
}

static void PrintHelpAll()
{
    PrintHelpAllDesignDoc();
    // PrintHelpAllCliOptions();
}

static void PrintDryRun(const RWCombinedCopyOptions& options,
                         const std::vector<std::string>& srcPaths,
                         const std::string& dstPath)
{
    std::string modeDesc;
    if (options.CopyMode == "CopyOnly")
        modeDesc = "全量复制 (CopyOnly)";
    else if (options.CopyMode == "CksumOnly")
        modeDesc = "数据校验 (CksumOnly)";
    else if (options.CopyMode == "CksumCopy")
        modeDesc = "增量复制 (CksumCopy)";
    else
        modeDesc = "Unknown (" + options.CopyMode + ")";

    std::cout <<
        "## acp dry-run\n"
        "\n"
        "### 功能场景\n"
        << modeDesc << "\n"
        "\n"
        "### 生效参数\n";

    std::cout << "| 参数 | 值 |\n|------|----|\n";
    std::cout << "| ProgramLogLevel | `" << options.ProgramLogLevel << "` |\n";
    std::cout << "| ProgramLogMode | `" << options.ProgramLogMode << "` |\n";
    std::cout << "| ProgramLogFilePath | `" << options.ProgramLogFilePath << "` |\n";
    std::cout << "| FileLogEnabled | `" << (options.FileLogEnabled ? "true" : "false") << "` |\n";
    std::cout << "| FileLogMode | `" << options.FileLogMode << "` |\n";
    std::cout << "| FileLogIntervalSec | `" << options.FileLogIntervalSec << "` |\n";
    std::cout << "| FileLogPath | `" << options.FileLogPath << "` |\n";
    std::cout << "| CopyEngine | `" << options.CopyEngine << "` |\n";
    std::cout << "| CopyMode | `" << options.CopyMode << "` |\n";
    std::cout << "| CksumAlgorithm | `" << options.CksumAlgorithm << "` |\n";
    std::cout << "| CopyParallelism | `" << options.CopyParallelism << "` |\n";
    std::cout << "| CopyChanSize | `" << options.CopyChanSize << "` |\n";
    std::cout << "| EnableInotify | `" << (options.EnableInotify ? "true" : "false") << "` |\n";
    std::cout << "| PreserveSparseFiles | `" << (options.PreserveSparseFiles ? "true" : "false") << "` |\n";
    std::cout << "| PreserveMeta | `" << (options.PreserveMeta ? "true" : "false") << "` |\n";
    std::cout << "| DirectIO | `" << (options.DirectIO ? "true" : "false") << "` |\n";
    std::cout << "| SyncWrites | `" << (options.SyncWrites ? "true" : "false") << "` |\n";
    std::cout << "| IOSize | `" << options.IoSize << "` |\n";
    std::cout << "| QueueDepth | `" << options.QueueDepth << "` |\n";
    std::cout << "| Batch | `" << options.Batch << "` |\n";
    std::cout << "| IOReapWait | `" << options.IOReapWait << "` |\n";
    std::cout << "| IOStuckTimeout | `" << options.IOStuckTimeout << "` |\n";

    std::cout << "\n### 源路径\n";
    if (srcPaths.empty())
        std::cout << "- (未指定)\n";
    else
        for (const auto& s : srcPaths)
            std::cout << "- `" << s << "`\n";
    std::cout << "\n### 目标路径\n- `" << (dstPath.empty() ? "(未指定)" : dstPath) << "`\n";
}

int main(int argc, char *argv[])
{
    namespace fs = std::filesystem;

    if (argc == 2 && (std::strcmp(argv[1], "--help") == 0 || std::strcmp(argv[1], "-h") == 0))
    {
        PrintHelpAllCliOptions();
        PrintHelp(argv[0]);
        return 0;
    }
    if (argc == 2 && std::strcmp(argv[1], "--help-all") == 0)
    {
        PrintHelpAll();
        return 0;
    }
    if (argc == 2 && (std::strcmp(argv[1], "--version") == 0 || std::strcmp(argv[1], "-v") == 0))
    {
        PrintVersion();
        return 0;
    }

    // ---------- CLI11 参数定义：所有选项先收集到 optional，后续按优先级覆盖配置 ----------
    CLI::App app{"acp — async cp, high-performance file copy"};
    CliOptions cliOpts;
    SetupCliOptions(app, cliOpts);
    app.set_version_flag("--version,-v", kVersion);

    // ---------- 配置合并优先级：默认值 → /etc/acp_config.json → ~/acp_config.json → ./acp_config.json → CLI 参数 ----------
    RWCombinedCopyOptions options;

    // 分层加载配置文件，后加载的覆盖先加载的
    auto merge = [&](const std::string& path) {
        auto merged = MergeCopyOptions(options, path);
        if (merged) options = *merged;
    };

    merge("/etc/acp_config.json");

    const char* home = std::getenv("HOME");
    if (home) {
        merge(std::string(home) + "/acp_config.json");
    }

    merge("./acp_config.json");

    // ---------- 解析命令行参数（CLI11），其值将在后续覆盖配置文件 ----------
    try {
        app.parse(argc, argv);
    } catch (const CLI::ParseError& e) {
        return app.exit(e);
    }

    // ---------- 命令行参数覆盖配置文件（最高优先级） ----------
    if (cliOpts.optLogLevel)        options.ProgramLogLevel = *cliOpts.optLogLevel;
    if (cliOpts.optLogMode)         options.ProgramLogMode = *cliOpts.optLogMode;
    if (cliOpts.optLogFile)         options.ProgramLogFilePath = *cliOpts.optLogFile;
    if (cliOpts.optFileLogMode)     options.FileLogMode = *cliOpts.optFileLogMode;
    if (cliOpts.optFileLogPath)     options.FileLogPath = *cliOpts.optFileLogPath;
    if (cliOpts.optEngine)          options.CopyEngine = *cliOpts.optEngine;
    if (cliOpts.optMode)            options.CopyMode = *cliOpts.optMode;
    if (cliOpts.optCksumAlgo)       options.CksumAlgorithm = *cliOpts.optCksumAlgo;

    if (cliOpts.optFileLogInterval) options.FileLogIntervalSec = *cliOpts.optFileLogInterval;
    if (cliOpts.optParallelism)     options.CopyParallelism = *cliOpts.optParallelism;
    if (cliOpts.optChanSize)        options.CopyChanSize = *cliOpts.optChanSize;
    if (cliOpts.optIoSize)          options.IoSize = *cliOpts.optIoSize;
    if (cliOpts.optQueueDepth)      options.QueueDepth = *cliOpts.optQueueDepth;
    if (cliOpts.optBatch)           options.Batch = *cliOpts.optBatch;
    if (cliOpts.optIoReapWait)      options.IOReapWait = *cliOpts.optIoReapWait;
    if (cliOpts.optIoStuckTimeout)  options.IOStuckTimeout = *cliOpts.optIoStuckTimeout;

    if (cliOpts.enableFileLog)      options.FileLogEnabled = true;
    if (cliOpts.disableFileLog)     options.FileLogEnabled = false;
    if (cliOpts.enableInotify)      options.EnableInotify = true;

    // CksumOnly / CksumCopy: 校验结果必须可见，自动启用 FileLog
    if ((options.CopyMode == "CksumOnly" || options.CopyMode == "CksumCopy") && !cliOpts.disableFileLog)
        options.FileLogEnabled = true;
    if (cliOpts.disableInotify)     options.EnableInotify = false;
    if (cliOpts.enableSparse)       options.PreserveSparseFiles = true;
    if (cliOpts.disableSparse)      options.PreserveSparseFiles = false;
    if (cliOpts.enablePreserveMeta) options.PreserveMeta = true;
    if (cliOpts.disablePreserveMeta)options.PreserveMeta = false;
    if (cliOpts.enableDirectIO)     options.DirectIO = true;
    if (cliOpts.disableDirectIO)    options.DirectIO = false;
    if (cliOpts.enableSync)         options.SyncWrites = true;
    if (cliOpts.disableSync)        options.SyncWrites = false;

    // ---------- 统一参数校验 ----------
    if (options.IoSize == 0) {
        std::cerr << "Configuration error: IOSize must be > 0\n";
        return 1;
    }
    if (options.QueueDepth == 0) {
        std::cerr << "Configuration error: QueueDepth must be > 0\n";
        return 1;
    }
    if (options.CopyParallelism <= 0) {
        std::cerr << "Configuration error: CopyParallelism must be > 0\n";
        return 1;
    }
    if (options.CopyChanSize <= 0) {
        std::cerr << "Configuration error: CopyChanSize must be > 0\n";
        return 1;
    }
    if (options.IOStuckTimeout < 0) {
        std::cerr << "Configuration error: IOStuckTimeout must be >= 0\n";
        return 1;
    }

    // IOStuckTimeout 与 EnableInotify 互斥：inotify 模式下主循环永不退出，stuck 检测无意义
    if (options.EnableInotify && options.IOStuckTimeout > 0) {
        std::cerr << "Configuration error: IOStuckTimeout and EnableInotify are mutually exclusive.\n";
        return 1;
    }

#ifdef __APPLE__
    // macOS: validate explicit CopyEngine setting
    if (cliOpts.optEngine && *cliOpts.optEngine != "gcd") {
        std::cerr << "Configuration error: On macOS, --engine must be 'gcd'. Got: " << *cliOpts.optEngine << "\n";
        return 1;
    }
    if (options.DirectIO) {
        std::cerr << "DirectIO is not supported on macOS" << std::endl;
        return 1;
    }
#endif

    auto logger = InitLogger(options);

    // ---------- dry-run：输出生效配置后直接退出 ----------
    if (cliOpts.dryRun)
    {
        std::string dstPath = cliOpts.positional.size() >= 2 ? cliOpts.positional.back() : "";
        std::vector<std::string> srcPaths;
        if (cliOpts.positional.size() >= 2)
            srcPaths.assign(cliOpts.positional.begin(), cliOpts.positional.end() - 1);
        PrintDryRun(options, srcPaths, dstPath);
        return 0;
    }

    // ---------- 路径参数校验 ----------
    if (cliOpts.positional.size() < 2)
    {
        std::cerr << "Error: At least one source and one destination path are required.\n"
                  << "Run with --help for more information." << std::endl;
        return 1;
    }

    std::string dstPath = cliOpts.positional.back();
    std::vector<std::string> srcPaths(cliOpts.positional.begin(), cliOpts.positional.end() - 1);
    bool multiSource = srcPaths.size() > 1;

    // ---------- CksumOnly / CksumCopy 模式约束校验 ----------
    bool isCksumMode = (options.CopyMode == "CksumOnly" || options.CopyMode == "CksumCopy");

    if (isCksumMode && multiSource)
    {
        std::cerr << "Error: Multi-source is not supported in "
                  << options.CopyMode << " mode." << std::endl;
        return 1;
    }

    if (isCksumMode && options.EnableInotify)
    {
        logger->warn("Inotify is disabled in {} mode.", options.CopyMode);
        options.EnableInotify = false;
    }

    // ---------- 路径校验层次 1：存在性 — 所有源路径必须存在 ----------
    for (const auto& src : srcPaths)
    {
        if (!fs::exists(src))
        {
            std::cerr << "Source path does not exist: " << src << std::endl;
            return 1;
        }
    }

    // CksumOnly / CksumCopy：目标必须存在
    if (isCksumMode && !fs::exists(dstPath))
    {
        std::cerr << "Error: Destination path must exist in "
                  << options.CopyMode << " mode: " << dstPath << std::endl;
        return 1;
    }

    // 多源模式特殊处理：目标必须是已存在的目录，且禁用 inotify（无法同时监控多个源）
    if (multiSource && !fs::is_directory(dstPath))
    {
        std::cerr << "When copying multiple sources, destination must be an existing directory." << std::endl;
        return 1;
    }

    if (multiSource && options.EnableInotify)
    {
        logger->warn("Inotify is disabled when copying multiple sources.");
        options.EnableInotify = false;
    }

    bool anyFailed = false;
    std::vector<std::pair<fs::path, fs::path>> batchPairs;
    batchPairs.reserve(srcPaths.size());

    for (const auto& src_path : srcPaths)
    {
        fs::path src_p;
        try
        {
            src_p = fs::canonical(src_path);
        }
        catch (const fs::filesystem_error& e)
        {
            std::cerr << "Failed to resolve source path: " << src_path << ", " << e.what() << std::endl;
            anyFailed = true;
            continue;
        }

        // ---------- 路径校验层次 2：多源/单源下目的地构造逻辑 ----------
        fs::path dst_p;
        if (multiSource)
        {
            // 多源时：以各源 basename 作为 dst 子目录/文件名（如 cp a b dir/ → dir/a, dir/b）
            try
            {
                dst_p = fs::canonical(dstPath) / src_p.filename();
            }
            catch (const fs::filesystem_error& e)
            {
                std::cerr << "Failed to resolve destination path: " << dstPath << ", " << e.what() << std::endl;
                anyFailed = true;
                continue;
            }
        }
        else
        {
            if (fs::exists(dstPath))
            {
                try
                {
                    dst_p = fs::canonical(dstPath);
                }
                catch (const fs::filesystem_error& e)
                {
                    std::cerr << "Failed to resolve destination path: " << dstPath << ", " << e.what() << std::endl;
                    anyFailed = true;
                    continue;
                }
            }
            else
            {
                dst_p = fs::absolute(dstPath);
            }
        }

        // ---------- 路径校验层次 3：等价性 — 禁止源与目标是同一文件/目录 ----------
        if (fs::exists(dst_p))
        {
            try
            {
                if (fs::equivalent(src_p, dst_p))
                {
                    std::cerr << "Source path and destination path cannot be the same." << std::endl;
                    anyFailed = true;
                    continue;
                }
            }
            catch (const fs::filesystem_error&)
            {
                // equivalent may fail for some path combinations; ignore
            }
        }

        // ---------- 路径校验层次 4：可写性 — 已存在的目的地必须可写 ----------
        if (fs::exists(dst_p))
        {
            std::error_code ec;
            fs::perms p = fs::status(dst_p, ec).permissions();
            if (!ec &&
                (p & fs::perms::owner_write) == fs::perms::none &&
                (p & fs::perms::group_write) == fs::perms::none &&
                (p & fs::perms::others_write) == fs::perms::none)
            {
                std::cerr << "Destination path is not writable: " << dst_p.string() << std::endl;
                anyFailed = true;
                continue;
            }
        }

        // ---------- 路径校验层次 5：子目录关系 — 禁止目录自复制导致无限递归 ----------
        if (fs::is_directory(src_p) && fs::is_directory(dst_p))
        {
            bool src_is_prefix_of_dst = std::mismatch(src_p.begin(), src_p.end(), dst_p.begin(), dst_p.end()).first == src_p.end();
            bool dst_is_prefix_of_src = std::mismatch(dst_p.begin(), dst_p.end(), src_p.begin(), src_p.end()).first == dst_p.end();

            if (src_is_prefix_of_dst || dst_is_prefix_of_src)
            {
                std::cerr << "Source path and destination path cannot be subdirectory of each other." << std::endl;
                std::cerr << "Source path: " << src_p.string() << std::endl;
                std::cerr << "Destination path: " << dst_p.string() << std::endl;
                anyFailed = true;
                continue;
            }
        }

        // ---------- CksumOnly / CksumCopy：源和目标必须同为文件或同为目录 ----------
        if (isCksumMode && fs::exists(dst_p))
        {
            if (fs::is_directory(src_p) != fs::is_directory(dst_p))
            {
                std::cerr << "Error: In " << options.CopyMode
                          << " mode, source and destination must be the same type (both files or both directories)."
                          << std::endl;
                anyFailed = true;
                continue;
            }
        }

        // ---------- 路由分发：根据源类型和目标类型选择 CopyDir / CopyFile / CopyBatch ----------
        bool dstIsDir = multiSource ? fs::is_directory(dstPath) : (fs::exists(dst_p) && fs::is_directory(dst_p));

        if (fs::is_directory(src_p) && (dstIsDir || !fs::exists(dst_p)))
        {
            // CksumOnly/CksumCopy: dst_p 已是最终目标路径，不再拼接 src filename
            fs::path final_dst = isCksumMode ? dst_p
                                : (multiSource ? dst_p : (dstIsDir ? dst_p / src_p.filename() : dst_p));

            if (multiSource)
            {
                batchPairs.emplace_back(src_p, final_dst);
            }
            else
            {
                logger->warn("begin to copy directory: {} to directory: {}", src_p.string(), final_dst.string());
                int rc = CopyDir(src_p, final_dst, options, logger);
                if (rc != 0)
                    anyFailed = true;
            }
        }
        else if (fs::is_regular_file(src_p) && dstIsDir)
        {
            // CksumOnly/CksumCopy: 文件对文件校验，不应进入 "file into dir" 分支
            if (isCksumMode)
            {
                std::cerr << "Error: In " << options.CopyMode
                          << " mode, source and destination must both be files or both be directories."
                          << std::endl;
                anyFailed = true;
            }
            else
            {
                fs::path final_dst = multiSource ? dst_p : dst_p / src_p.filename();
                if (multiSource)
                {
                    batchPairs.emplace_back(src_p, final_dst);
                }
                else
                {
                    logger->info("begin to copy file: {} to file: {}", src_p.string(), final_dst.string());
                    int rc = CopyFile(src_p, final_dst, options, logger);
                    if (rc != 0)
                        anyFailed = true;
                }
            }
        }
        else if (fs::is_regular_file(src_p))
        {
            if (multiSource)
            {
                batchPairs.emplace_back(src_p, dst_p);
            }
            else
            {
                logger->info("begin to copy file: {} to file: {}", src_p.string(), dst_p.string());
                int rc = CopyFile(src_p, dst_p, options, logger);
                if (rc != 0)
                    anyFailed = true;
            }
        }
        else
        {
            std::cerr << "Unsupported source type: " << src_p.string() << std::endl;
            anyFailed = true;
        }
    }

    // 多源模式下统一通过 CopyBatch 执行，避免为每个源单独创建 CopyEngine 线程组
    if (multiSource && !anyFailed && !batchPairs.empty())
    {
        int rc = CopyBatch(batchPairs, options, logger);
        if (rc != 0)
            anyFailed = true;
    }

    return anyFailed ? 1 : 0;
}
