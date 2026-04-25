#pragma once

#include "lib/copy/copy.hpp"
#include "lib/thirdparty/json.hpp"
#include "base/base.hpp"
#include "base/chan.hpp"
#ifdef __APPLE__
#include "base/fsevents.hpp"
#else
#include "base/inotify.hpp"
#endif
#include "base/logger.hpp"

// 初始化日志系统（文件或控制台），返回 ILogger 接口实例
std::shared_ptr<ILogger> InitLogger(const RWCombinedCopyOptions &options);

// 从 JSON 配置文件加载配置并合并到 base 中；文件不存在时返回 nullopt，解析失败时输出错误并返回 nullopt
std::optional<RWCombinedCopyOptions> MergeCopyOptions(
    const RWCombinedCopyOptions &base,
    const std::string &config_path);

// 复制目录（含子树），支持 inotify/FSEvents 持续监控；stopFlag 用于外部终止信号
int CopyDir(const fs::path src_p, const fs::path dst_p, const RWCombinedCopyOptions &options, std::shared_ptr<ILogger> logger, std::atomic<bool> *stopFlag = nullptr);

// 复制单个文件或符号链接；不处理目录递归
int CopyFile(const fs::path src_file, const fs::path dst_file, const RWCombinedCopyOptions &options, std::shared_ptr<ILogger> logger);

// 多源批量复制：将多个源目录/文件统一调度到单个 CopyEngine；多源模式下禁用 inotify
int CopyBatch(const std::vector<std::pair<fs::path, fs::path>> &srcDstPairs,
              const RWCombinedCopyOptions &options,
              std::shared_ptr<ILogger> logger);