# acp 用户体验优化设计

> 不改动核心 I/O 复制逻辑，通过调整功能场景、日志输出和命令行交互，提升用户友好性。

---

## 1. 功能场景

acp 支持三种功能场景，由 `CopyMode` 配置项决定：

| 场景 | CopyMode 值 | 说明 |
|------|-------------|------|
| 全量复制 | `CopyOnly` | 从源读取并写入目标，行为与 `cp` 类似 |
| 数据校验 | `CksumOnly` | 校验源与目标的差异，不写入，结果记录到 FileLog |
| 增量复制 | `CksumCopy` | 校验源与目标的差异，仅写入差异数据 |

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

**ProgramLog：** 与全量复制相同。

**FileLog：** 仅输出文件对比结果事件和校验进度性能事件，其他事件不输出。

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

## 3. 使用场景与配置示例

### 3.1 个人日常使用

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

| 配置项 | 值 | 说明 |
|--------|-----|------|
| ProgramLogLevel | `error` | 一般人对复制过程不感兴趣 |
| ProgramLogMode | `console` | 有报错直接输出，和 `cp` 行为一致 |
| FileLogEnabled | `false` | 无输出即表示全部复制成功，和 `cp` 行为一致；AI agent 运行时设为 `true`，配合 `FileLogMode: "file"` 和 `FileLogPath`，可在复制过程中查询进度 |
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

### 3.2 企业数据迁移

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
| FileLogEnabled | `true` | 设置为 `true`，配合 `FileLogMode: "file"` 和 `FileLogPath`，复制前清除文件，复制中可查询进度 |
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

---

## 4. Help 设计

`acp --help` 以 Markdown 格式精简输出，包含以下内容：

1. **三种功能场景的命令行用法说明**
2. **两种使用场景的说明**（个人日常 / 企业数据迁移）
3. **使用例子**
4. **日志说明**

完整参数列表通过 `acp --help-all` 查看。

---

## 5. 命令行参数优化

### `--dry-run`

新增 `--dry-run` 参数。设置后 acp 不执行实际复制或校验，而是以 Markdown 格式输出：

- 当前生效的参数
- 当前处于哪种功能场景（全量复制 / 数据校验 / 增量复制）

用途：帮助用户在执行前确认配置是否正确。
