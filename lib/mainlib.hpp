#pragma once

#include "lib/copy/copy.hpp"
#include "lib/thirdparty/json.hpp"
#include "base/base.hpp"
#include "base/chan.hpp"
#include "base/inotify.hpp"
#include "base/logger.hpp"

std::shared_ptr<ILogger> InitLogger(const RWCombinedCopyOptions &options);
int CopyDir(const fs::path src_p, const fs::path dst_p, const RWCombinedCopyOptions &options, std::shared_ptr<ILogger> logger);
int CopyFile(const fs::path src_file, const fs::path dst_file, const RWCombinedCopyOptions &options, std::shared_ptr<ILogger> logger);