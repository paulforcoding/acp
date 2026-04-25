#pragma once

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <mutex>
#include <string>
#include <unistd.h>

#include "base/base.hpp"

// FileLogReporter: 双角色设计——既输出逐文件 NDJSON 事件流，也输出聚合状态快照。
// NDJSON 格式选择原因：每行独立可解析，便于 Agent 流式消费和 grep 过滤。
// console 模式：事件走 stdout（供外部管道消费），状态快照原子覆盖写入 stateFilePath（供监控探针读取）。
// file 模式：事件与快照追加到同一文件，便于事后审计。
// 所有统计使用原子变量，避免与 I/O 引擎竞争锁；事件输出使用独立 mutex 保证行级原子性。
class FileLogReporter
{
public:
    FileLogReporter(bool enabled, const std::string& fileLogMode, int intervalSec,
                    const std::string& fileLogPath, const std::string& stateFilePath)
        : mEnabled(enabled),
          mFileLogMode(fileLogMode),
          mIntervalSec(intervalSec),
          mFileLogPath(fileLogPath),
          mStateFilePath(stateFilePath),
          mStartTime(std::chrono::steady_clock::now())
    {
        if (mEnabled && mFileLogMode == "file" && !mFileLogPath.empty())
        {
            // 以追加模式打开，确保文件存在
            std::ofstream ofs(mFileLogPath, std::ios::app);
        }
    }

    // --- 事件发射 ---

    void FileStart(const std::string &src, const std::string &dst, size_t size)
    {
        if (!mEnabled)
            return;
        EmitEvent(fmt::format("{{\"type\":\"file_info\",\"event\":\"file_start\",\"src\":\"{}\",\"dst\":\"{}\",\"size\":{},\"timestamp\":\"{}\"}}",
                              EscapeJsonString(src), EscapeJsonString(dst), size, CurrentIsoTimestamp()));
    }

    void FileComplete(const std::string &src, const std::string &dst, size_t size, int64_t durationMs)
    {
        if (!mEnabled)
            return;
        double speedMbps = durationMs > 0 ? static_cast<double>(size) / 1024.0 / 1024.0 / (static_cast<double>(durationMs) / 1000.0) : 0.0;
        EmitEvent(fmt::format("{{\"type\":\"file_info\",\"event\":\"file_complete\",\"src\":\"{}\",\"dst\":\"{}\",\"size\":{},\"bytes_written\":{},\"duration_ms\":{},\"speed_mbps\":{:.1f},\"timestamp\":\"{}\"}}",
                              EscapeJsonString(src), EscapeJsonString(dst), size, size, durationMs, speedMbps, CurrentIsoTimestamp()));
    }

    void FileError(const std::string &src, const std::string &error, int errnoCode, const std::string &action)
    {
        if (!mEnabled)
            return;
        EmitEvent(fmt::format("{{\"type\":\"file_info\",\"event\":\"file_error\",\"src\":\"{}\",\"error\":\"{}\",\"errno\":{},\"action\":\"{}\",\"timestamp\":\"{}\"}}",
                              EscapeJsonString(src), EscapeJsonString(error), errnoCode, action, CurrentIsoTimestamp()));
    }

    void FileUnsupported(const std::string &src, const std::string &fileType, const std::string &action)
    {
        if (!mEnabled)
            return;
        EmitEvent(fmt::format("{{\"type\":\"file_info\",\"event\":\"file_unsupported\",\"src\":\"{}\",\"type\":\"{}\",\"action\":\"{}\",\"timestamp\":\"{}\"}}",
                              EscapeJsonString(src), EscapeJsonString(fileType), action, CurrentIsoTimestamp()));
    }

    void FileMetaWarning(const std::string &src,
                         const std::string &metaType,
                         const std::string &error,
                         int errnoCode)
    {
        if (!mEnabled)
            return;
        EmitEvent(fmt::format(
            "{{\"type\":\"file_info\",\"event\":\"meta_warning\","
            "\"src\":\"{}\",\"meta_type\":\"{}\",\"error\":\"{}\","
            "\"errno\":{},\"timestamp\":\"{}\"}}",
            EscapeJsonString(src), metaType, EscapeJsonString(error),
            errnoCode, CurrentIsoTimestamp()));
    }

    void FileCksumResult(const std::string &src,
                         const std::string &dst,
                         const std::string &result,
                         const std::string &reason,
                         size_t offset = 0,
                         const std::string &detail = "")
    {
        if (!mEnabled)
            return;
        EmitEvent(fmt::format(
            "{{\"type\":\"file_info\",\"event\":\"cksum_result\","
            "\"src\":\"{}\",\"dst\":\"{}\",\"result\":\"{}\",\"reason\":\"{}\","
            "\"offset\":{},\"detail\":\"{}\",\"timestamp\":\"{}\"}}",
            EscapeJsonString(src), EscapeJsonString(dst),
            result, EscapeJsonString(reason),
            offset, EscapeJsonString(detail),
            CurrentIsoTimestamp()));
    }

    void CopyPlan(const std::string &scanState,
                  size_t filesTotal, size_t dirsTotal, size_t symlinksTotal,
                  size_t bytesTotal, size_t filesRegular, size_t filesUnsupported)
    {
        if (!mEnabled)
            return;
        EmitEvent(fmt::format(
            "{{\"type\":\"file_info\",\"event\":\"copy_plan\",\"scan_state\":\"{}\",\"files_total\":{},\"dirs_total\":{},\"symlinks_total\":{},\"bytes_total\":{},\"files_regular\":{},\"files_unsupported\":{},\"timestamp\":\"{}\"}}",
            scanState, filesTotal, dirsTotal, symlinksTotal, bytesTotal, filesRegular, filesUnsupported, CurrentIsoTimestamp()));
    }

    void CopyComplete(int64_t durationMs)
    {
        if (!mEnabled)
            return;
        size_t total = mFilesTotal.load();
        size_t done = mFilesDone.load();
        size_t bytesTotal = mBytesTotal.load();
        size_t bytesDone = mBytesDone.load();
        double speedMbps = durationMs > 0 ? static_cast<double>(bytesDone) / 1024.0 / 1024.0 / (static_cast<double>(durationMs) / 1000.0) : 0.0;
        EmitEvent(fmt::format(
            "{{\"type\":\"file_info\",\"event\":\"copy_complete\",\"files_total\":{},\"files_done\":{},\"bytes_total\":{},\"bytes_done\":{},\"duration_ms\":{},\"speed_mbps_avg\":{:.1f},\"timestamp\":\"{}\"}}",
            total, done, bytesTotal, bytesDone, durationMs, speedMbps, CurrentIsoTimestamp()));
    }

    void StuckDetected(long round, int stuckSeconds)
    {
        if (!mEnabled)
            return;
        size_t total = mFilesTotal.load();
        size_t done = mFilesDone.load();
        size_t bytesTotal = mBytesTotal.load();
        size_t bytesDone = mBytesDone.load();
        std::string currentFile;
        {
            std::lock_guard<std::mutex> lock(mCurrentFileMutex);
            currentFile = mCurrentFile;
        }
        EmitEvent(fmt::format(
            "{{\"type\":\"file_info\",\"event\":\"stuck_detected\","
            "\"round\":{},\"stuck_seconds\":{},"
            "\"files_total\":{},\"files_done\":{},"
            "\"bytes_total\":{},\"bytes_done\":{},"
            "\"current_file\":\"{}\",\"timestamp\":\"{}\"}}",
            round, stuckSeconds,
            total, done, bytesTotal, bytesDone,
            EscapeJsonString(currentFile),
            CurrentIsoTimestamp()));
    }

    void MaybeEmitProgressSummary()
    {
        if (!mEnabled)
            return;
        auto now = std::chrono::steady_clock::now();
        auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(now - mLastSummaryTime).count();
        if (elapsed < mIntervalSec)
            return;
        mLastSummaryTime = now;

        size_t total = mFilesTotal.load();
        size_t done = mFilesDone.load();
        size_t bytesTotal = mBytesTotal.load();
        size_t bytesDone = mBytesDone.load();

        auto wallMs = std::chrono::duration_cast<std::chrono::milliseconds>(now - mStartTime).count();
        double speedMbps = wallMs > 0 ? static_cast<double>(bytesDone) / 1024.0 / 1024.0 / (static_cast<double>(wallMs) / 1000.0) : 0.0;
        int64_t etaSeconds = 0;
        if (speedMbps > 0.001 && bytesTotal > bytesDone)
        {
            double remainingBytes = static_cast<double>(bytesTotal - bytesDone);
            etaSeconds = static_cast<int64_t>(remainingBytes / 1024.0 / 1024.0 / speedMbps);
        }

        std::string currentFile;
        {
            std::lock_guard<std::mutex> lock(mCurrentFileMutex);
            currentFile = mCurrentFile;
        }

        EmitEvent(fmt::format(
            "{{\"type\":\"file_info\",\"event\":\"progress_summary\",\"files_total\":{},\"files_done\":{},\"bytes_total\":{},\"bytes_done\":{},\"speed_mbps_avg\":{:.1f},\"eta_seconds\":{},\"current_file\":\"{}\",\"timestamp\":\"{}\"}}",
            total, done, bytesTotal, bytesDone, speedMbps, etaSeconds, EscapeJsonString(currentFile), CurrentIsoTimestamp()));
    }

    // --- 统计更新 ---

    void IncrementFilesTotal(size_t n = 1) { mFilesTotal.fetch_add(n, std::memory_order_relaxed); }
    void IncrementFilesDone(size_t n = 1) { mFilesDone.fetch_add(n, std::memory_order_relaxed); }
    void AddBytesTotal(size_t n) { mBytesTotal.fetch_add(n, std::memory_order_relaxed); }
    // 原子累加：多个工作线程并发完成 I/O 时无锁更新进度，避免成为扩展瓶颈
    void AddBytesDone(size_t n) { mBytesDone.fetch_add(n, std::memory_order_relaxed); }

    void SetCurrentFile(const std::string &path)
    {
        std::lock_guard<std::mutex> lock(mCurrentFileMutex);
        mCurrentFile = path;
    }

    void SetFilesTotal(size_t n) { mFilesTotal.store(n, std::memory_order_relaxed); }
    void SetBytesTotal(size_t n) { mBytesTotal.store(n, std::memory_order_relaxed); }

    size_t GetFilesTotal() const { return mFilesTotal.load(std::memory_order_relaxed); }
    size_t GetFilesDone() const { return mFilesDone.load(std::memory_order_relaxed); }
    size_t GetBytesTotal() const { return mBytesTotal.load(std::memory_order_relaxed); }
    size_t GetBytesDone() const { return mBytesDone.load(std::memory_order_relaxed); }
    std::string GetCurrentFile() const
    {
        std::lock_guard<std::mutex> lock(mCurrentFileMutex);
        return mCurrentFile;
    }

    // --- 状态文件 ---

    void UpdateStateFile()
    {
        if (!mEnabled)
            return;

        size_t total = mFilesTotal.load(std::memory_order_relaxed);
        size_t done = mFilesDone.load(std::memory_order_relaxed);
        size_t bytesTotal = mBytesTotal.load(std::memory_order_relaxed);
        size_t bytesDone = mBytesDone.load(std::memory_order_relaxed);

        auto now = std::chrono::steady_clock::now();
        auto wallMs = std::chrono::duration_cast<std::chrono::milliseconds>(now - mStartTime).count();
        double speedMbps = wallMs > 0 ? static_cast<double>(bytesDone) / 1024.0 / 1024.0 / (static_cast<double>(wallMs) / 1000.0) : 0.0;

        std::string currentFile;
        {
            std::lock_guard<std::mutex> lock(mCurrentFileMutex);
            currentFile = mCurrentFile;
        }

        if (mFileLogMode == "file" && !mFileLogPath.empty())
        {
            // file mode: 追加 NDJSON 行到同一个日志文件
            std::string content = fmt::format(
                "{{\"type\":\"file_info\",\"event\":\"state_snapshot\",\"pid\":{},\"state\":\"running\",\"start_time\":\"{}\",\"files_total\":{},\"files_done\":{},\"bytes_total\":{},\"bytes_done\":{},\"current_speed_mbps\":{:.1f},\"current_file\":\"{}\",\"last_update\":\"{}\"}}\n",
                static_cast<int>(getpid()),
                mStartIsoTime.empty() ? CurrentIsoTimestamp() : mStartIsoTime,
                total, done, bytesTotal, bytesDone,
                speedMbps,
                EscapeJsonString(currentFile),
                CurrentIsoTimestamp());
            AppendToLogFile(content);
        }
        else if (!mStateFilePath.empty())
        {
            // console mode: 原子覆盖写入状态文件
            std::string content = fmt::format(
                "{{\"pid\":{},\"state\":\"running\",\"start_time\":\"{}\",\"files_total\":{},\"files_done\":{},\"bytes_total\":{},\"bytes_done\":{},\"current_speed_mbps\":{:.1f},\"current_file\":\"{}\",\"last_update\":\"{}\"}}\n",
                static_cast<int>(getpid()),
                mStartIsoTime.empty() ? CurrentIsoTimestamp() : mStartIsoTime,
                total, done, bytesTotal, bytesDone,
                speedMbps,
                EscapeJsonString(currentFile),
                CurrentIsoTimestamp());
            WriteStateFileAtomic(content);
        }
    }

    void FinalizeStateFile()
    {
        if (!mEnabled)
            return;

        size_t total = mFilesTotal.load(std::memory_order_relaxed);
        size_t done = mFilesDone.load(std::memory_order_relaxed);
        size_t bytesTotal = mBytesTotal.load(std::memory_order_relaxed);
        size_t bytesDone = mBytesDone.load(std::memory_order_relaxed);

        if (mFileLogMode == "file" && !mFileLogPath.empty())
        {
            std::string content = fmt::format(
                "{{\"type\":\"file_info\",\"event\":\"state_snapshot\",\"pid\":{},\"state\":\"completed\",\"start_time\":\"{}\",\"files_total\":{},\"files_done\":{},\"bytes_total\":{},\"bytes_done\":{},\"current_speed_mbps\":0.0,\"current_file\":\"\",\"last_update\":\"{}\"}}\n",
                static_cast<int>(getpid()),
                mStartIsoTime.empty() ? CurrentIsoTimestamp() : mStartIsoTime,
                total, done, bytesTotal, bytesDone,
                CurrentIsoTimestamp());
            AppendToLogFile(content);
        }
        else if (!mStateFilePath.empty())
        {
            std::string content = fmt::format(
                "{{\"pid\":{},\"state\":\"completed\",\"start_time\":\"{}\",\"files_total\":{},\"files_done\":{},\"bytes_total\":{},\"bytes_done\":{},\"current_speed_mbps\":0.0,\"current_file\":\"\",\"last_update\":\"{}\"}}\n",
                static_cast<int>(getpid()),
                mStartIsoTime.empty() ? CurrentIsoTimestamp() : mStartIsoTime,
                total, done, bytesTotal, bytesDone,
                CurrentIsoTimestamp());
            WriteStateFileAtomic(content);
        }
    }

private:
    void EmitEvent(const std::string &jsonLine)
    {
        std::lock_guard<std::mutex> lock(mEmitMutex);
        if (mFileLogMode == "file" && !mFileLogPath.empty())
        {
            AppendToLogFile(jsonLine + "\n");
        }
        else
        {
            std::cout << jsonLine << '\n';
        }
    }

    void AppendToLogFile(const std::string &content)
    {
        std::lock_guard<std::mutex> lock(mFileMutex);
        std::ofstream ofs(mFileLogPath, std::ios::app);
        if (ofs)
        {
            ofs << content;
            ofs.flush();
        }
    }

    void WriteStateFileAtomic(const std::string &content)
    {
        std::string tmpPath = mStateFilePath + ".tmp";
        {
            std::ofstream ofs(tmpPath, std::ios::out | std::ios::trunc);
            if (!ofs)
                return;
            ofs << content;
        }
        std::filesystem::rename(tmpPath, mStateFilePath);
    }

    bool mEnabled;
    std::string mFileLogMode;
    int mIntervalSec;
    std::string mFileLogPath;
    std::string mStateFilePath;
    std::chrono::steady_clock::time_point mStartTime;
    std::chrono::steady_clock::time_point mLastSummaryTime;
    std::string mStartIsoTime{CurrentIsoTimestamp()};

    std::atomic<size_t> mFilesTotal{0};
    std::atomic<size_t> mFilesDone{0};
    std::atomic<size_t> mBytesTotal{0};
    std::atomic<size_t> mBytesDone{0};

    mutable std::mutex mCurrentFileMutex;
    std::string mCurrentFile;

    std::mutex mEmitMutex;
    std::mutex mFileMutex;
};
