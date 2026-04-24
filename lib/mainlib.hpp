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

std::shared_ptr<ILogger> InitLogger(const RWCombinedCopyOptions &options);
std::optional<RWCombinedCopyOptions> MergeCopyOptions(
    const RWCombinedCopyOptions &base,
    const std::string &config_path);
int CopyDir(const fs::path src_p, const fs::path dst_p, const RWCombinedCopyOptions &options, std::shared_ptr<ILogger> logger, std::atomic<bool> *stopFlag = nullptr);
int CopyFile(const fs::path src_file, const fs::path dst_file, const RWCombinedCopyOptions &options, std::shared_ptr<ILogger> logger);