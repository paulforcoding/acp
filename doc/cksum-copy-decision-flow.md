# CksumCopy 增量复制决策流程

CksumCopy 模式通过校验源与目标的差异，仅写入差异数据，避免不必要的 I/O。决策分为**文件级快速路径**和**块级校验路径**两层。

## 文件级快速路径：size + mtime 比对

打开目标文件后，如果 dst 存在，首先执行一次 size + mtime 比对：

- **size 相同且 mtime 完全一致**（秒 + 纳秒）→ 跳过整个文件的块级校验，视为 match，不执行任何 I/O。
- **size 或 mtime 不一致** → 进入块级校验路径。

这条快速路径类似 rsync 的 `--size-only` + `--modify-window=0` 优化，在大多数场景下能大幅减少 I/O。

### 快速路径的局限

**CksumCopy 仅以 size + mtime 作为快速跳过的判断条件，不检测其他元数据（权限、属主、时间戳精度、xattr、ACL 等）。** 这意味着：

- 如果文件内容相同但权限或属主不同，CksumCopy 仍会跳过（因为 size + mtime 一致）。
- 如果 mtime 被人为恢复（如 `touch -r`）使源和目标 mtime 一致，但实际内容不同，CksumCopy 会错误地跳过该文件。

如需检测除内容以外的元数据差异，应使用 **CksumOnly** 模式（始终逐块校验内容 + 比对全部元数据）。

## 块级校验路径

未命中快速路径的文件进入逐块校验。CksumCopy 为每个文件分配两类 slot：

- **RW slot**：读取源文件数据块
- **Cksum slot**：读取目标文件相同偏移的数据块

当同一个偏移的 RW slot 和 Cksum slot 都完成读取后，比较两者的校验和：

- **校验和匹配** → 跳过该块的写入，直接更新已写入字节数（相当于"假装已写入"）。
- **校验和不匹配** → 将 RW slot 中的源数据写入目标文件的对应偏移。如果该块是全零且开启了稀疏文件保持，则走 `SkipWriteAsHole` 逻辑。
- **Cksum slot 读取失败**（如 EOF）→ 视为不匹配，执行写入。

## 写入完成后的处理

所有块处理完毕后：

- **非快速路径文件**：执行 `TruncateDstToSrcSize`（确保 dst 大小正确）、可选 fsync、`PreserveMetadata`（覆盖元数据）。
- **快速路径跳过的文件**：不执行 truncate、fsync、PreserveMetadata，因为文件未被修改。

## 目的端文件不存在时的行为

CksumCopy 以 `O_CREAT | O_RDWR` 打开目标文件。如果目的端文件不存在，会自动创建空文件，然后走块级校验路径：

1. `open()` 创建空的目标文件
2. 进入块级校验：RW slot 读 src，Cksum slot 读 dst（空文件 → read 返回 EOF）
3. Cksum slot EOF → 视为 mismatch → 所有源数据块写入 dst

效果等同于全量复制，只是多了一次对空 dst 的无意义 read（有一点冗余 I/O）。如果目的端父目录也不存在，会通过 `fs::create_directories` 自动创建。

> 注意：CksumOnly 模式下目的端文件不存在不会创建，而是记录 `dst_missing` 后跳过。

## 决策流程图

```
OpenDstFile
├─ dst 不存在 → 自动创建空文件，走块级校验（等效全量复制，有少量冗余 read）
├─ dst 存在，size + mtime 匹配 → 跳过整个文件（不做 I/O）
└─ dst 存在，size 或 mtime 不匹配 → 走块级校验
    └─ 逐块: cksum(src_block, dst_block)
        ├─ match   → 跳过该块写入
        └─ mismatch → 写入该块 (src → dst)
```

## 与 CksumOnly 的对比

| 行为 | CksumCopy | CksumOnly |
|------|-----------|-----------|
| 文件级 size+mtime 快速跳过 | 支持 | 不支持（始终逐块校验） |
| 块级校验 | 仅对不一致文件执行 | 始终执行 |
| 写入差异数据 | 是 | 否 |
| 元数据比对 | 写入后覆盖（PreserveMetadata） | 只读比对并报告差异 |
| 快速跳过时检测元数据差异 | 不检测 | 不适用（不快速跳过） |
