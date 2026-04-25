# acp 的目录自复制检测设计说明

> 本文档说明 acp 在递归复制目录时，如何检测并防止「把目录复制到自身子树中」导致的无限递归或数据膨胀问题。

---

## 1. 什么是目录自复制

目录自复制指复制操作的源目录与目标目录存在包含关系，导致复制过程陷入无限循环或指数级膨胀。典型场景：

```bash
# 场景 A：直接把 src 复制到 src 内部
acp src src/subdir
# → 如果允许，会创建 src/subdir/src、src/subdir/src/subdir/src ...

# 场景 B：bind mount 导致路径不同但文件系统对象相同
mount --bind /a /mnt/bind_a
acp /a /mnt/bind_a/sub
# → /a 和 /mnt/bind_a 是同一个目录，路径前缀比较无法识别

# 场景 C：目录硬链接形成循环
mkdir -p /a/b
ln /a /a/b/loop   # root 权限下的目录硬链接
acp /a /dst
# → 扫描 /a/b/loop 时实际回到 /a，形成无限循环
```

---

## 2. acp 当前提供的保护

acp 在**复制启动前**对命令行传入的源路径和目标路径进行一次性检测：

| 检测项 | 实现方式 | 覆盖场景 |
|--------|---------|---------|
| **源路径 = 目标路径** | `fs::equivalent()` | `acp src src` |
| **源路径是目标路径的前缀** | `fs::path` 分量级前缀比较 | `acp /a /a/b`、`acp /a/b /a` |
| **目标路径是源路径的前缀** | 同上 | `acp /a/b /a` |

当检测到上述情况时，acp 会立即报错退出，不会启动复制：

```
Source path and destination path cannot be subdirectory of each other.
```

**这覆盖了日常使用中 99% 以上的误操作**，例如 Agent 在拼接路径时不小心把输出目录拼到了输入目录内部。

---

## 3. acp 未覆盖的场景

以下场景**不会被 acp 检测或阻止**：

### 3.1 Bind Mount 绕回

当源目录和目标目录通过 bind mount 指向同一文件系统对象，但路径完全不同：

```bash
mount --bind /data /mnt/data_mirror
acp /data /mnt/data_mirror/sub
```

acp 的路径前缀比较无法识别此关系，复制会正常执行。

### 3.2 目录硬链接循环

Linux 下 root 用户可以创建目录硬链接。如果源目录内部存在目录硬链接指回祖先目录：

```bash
mkdir -p /a/b
ln /a /a/b/loop   # 目录硬链接
acp /a /dst
```

acp 在递归扫描 `/a` 时会进入 `/a/b/loop`（实际回到 `/a`），导致**无限循环**。

### 3.3 符号链接指向祖先

如果源目录内部有符号链接指回祖先：

```bash
mkdir -p src/subdir
ln -s .. src/subdir/loop
acp src dst
```

acp 使用 `lstat`（不跟随符号链接），因此不会递归进入 `loop`。复制本身不会无限循环，但复制到目标端的 `dst/src/subdir/loop` 仍然指向 `..`（即 `dst/` 的父目录），其语义与源端不同。

---

## 4. 为什么不覆盖这些场景

### 4.1 场景罕见

- **Bind mount 绕回**：需要系统级 mount 配置，普通文件复制任务中几乎不会遇到。
- **目录硬链接**：Linux 禁止普通用户创建目录硬链接，仅 root 可以操作。现代文件系统和工具链中，目录硬链接已被视为废弃特性。

### 4.2 内存开销与定位冲突

GNU cp 通过维护**递归过程中所有已访问目录的 `(st_dev, st_ino)` 集合**来检测循环。对于 acp 定位的「海量小文件」场景（数百万目录），持续维护这一集合的内存开销显著：

- 每个目录条目需存储 `dev_t + ino_t`（通常 16–24 字节）
- 100 万个目录 ≈ 16–24 MB 常驻内存
- `std::set`/`std::unordered_set` 的额外桶/节点开销通常再乘 2–3 倍

acp 的核心竞争力是**高性能异步复制**，在海量小文件场景下每一分内存都应服务于 I/O 吞吐，而非为极端边缘场景支付防御性开销。

### 4.3 ROI 不显著

| 场景 | 发生概率 | 检测成本 | 决策 |
|------|---------|---------|------|
| 路径前缀自复制 | 高（用户 typo） | 零（启动前一次字符串比较） | 已覆盖 ✅ |
| Bind mount 绕回 | 极低 | 中（维护 inode 集合） | 不覆盖 |
| 目录硬链接循环 | 极低 | 高（递归中每目录查集合） | 不覆盖 |

---

## 5. 如果你确实需要完整保护

如果你有以下需求：
- 在不可信/复杂的文件系统环境中运行（如备份 `/`、复制包含 bind mount 的目录树）
- 必须防御目录硬链接循环（如处理 legacy 系统或第三方提供的目录树）

建议**fork acp 代码库**，在 `lib/mainlib.cpp` 的 `CopyDir` 函数中增加递归级 inode 跟踪：

```cpp
// 在 DFS 入口维护一个已访问目录的 (dev, ino) 集合
std::set<std::pair<dev_t, ino_t>> seenDirs;

// 每次 opendir 前
struct stat st;
if (lstat(srcChild.c_str(), &st) == 0 && S_ISDIR(st.st_mode)) {
    auto key = std::make_pair(st.st_dev, st.st_ino);
    if (!seenDirs.insert(key).second) {
        logger->warn("Directory cycle detected at {}, skipping", srcChild);
        continue;
    }
    // 继续递归 ...
}
```

---

## 6. 与 GNU cp 的对比总结

| 检测能力 | acp | GNU cp |
|---------|-----|--------|
| 启动前路径前缀检测 | ✅ | ✅ |
| 源 = 目标（同一路径） | ✅ | ✅ |
| 递归中目录硬链接循环 | ❌ | ✅（祖先链表 + 已复制哈希表） |
| Bind mount 绕回 | ❌ | ✅（`(st_dev, st_ino)` 比较） |
| 内存开销（递归保护） | 无 | 与目录数量成正比 |

---

*本文档适用于 acp v0.5.0 及后续版本。*
