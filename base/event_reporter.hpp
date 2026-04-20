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

// FileLogReporter: 输出 file_info 事件到 stdout（NDJSON），并维护状态文件。
// 所有统计使用原子变量，事件输出使用独立 mutex（不与 I/O 队列竞争）。
class FileLogReporter
{
public:
    FileLogReporter(bool enabled, int intervalSec, std::string stateFilePath)
        : mEnabled(enabled),
          mIntervalSec(intervalSec),
          mStateFilePath(std::move(stateFilePath)),
          mStartTime(std::chrono::steady_clock::now())
    {
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
    void AddBytesDone(size_t n) { mBytesDone.fetch_add(n, std::memory_order_relaxed); }

    void SetCurrentFile(const std::string &path)
    {
        std::lock_guard<std::mutex> lock(mCurrentFileMutex);
        mCurrentFile = path;
    }

    void SetFilesTotal(size_t n) { mFilesTotal.store(n, std::memory_order_relaxed); }
    void SetBytesTotal(size_t n) { mBytesTotal.store(n, std::memory_order_relaxed); }

    // --- 状态文件 ---

    void UpdateStateFile()
    {
        if (!mEnabled || mStateFilePath.empty())
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

    void FinalizeStateFile()
    {
        if (!mEnabled || mStateFilePath.empty())
            return;

        size_t total = mFilesTotal.load(std::memory_order_relaxed);
        size_t done = mFilesDone.load(std::memory_order_relaxed);
        size_t bytesTotal = mBytesTotal.load(std::memory_order_relaxed);
        size_t bytesDone = mBytesDone.load(std::memory_order_relaxed);

        std::string content = fmt::format(
            "{{\"pid\":{},\"state\":\"completed\",\"start_time\":\"{}\",\"files_total\":{},\"files_done\":{},\"bytes_total\":{},\"bytes_done\":{},\"current_speed_mbps\":0.0,\"current_file\":\"\",\"last_update\":\"{}\"}}\n",
            static_cast<int>(getpid()),
            mStartIsoTime.empty() ? CurrentIsoTimestamp() : mStartIsoTime,
            total, done, bytesTotal, bytesDone,
            CurrentIsoTimestamp());

        WriteStateFileAtomic(content);
    }

private:
    void EmitEvent(const std::string &jsonLine)
    {
        std::lock_guard<std::mutex> lock(mEmitMutex);
        std::cout << jsonLine << '\n';
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
    int mIntervalSec;
    std::string mStateFilePath;
    std::chrono::steady_clock::time_point mStartTime;
    std::chrono::steady_clock::time_point mLastSummaryTime;
    std::string mStartIsoTime{CurrentIsoTimestamp()};

    std::atomic<size_t> mFilesTotal{0};
    std::atomic<size_t> mFilesDone{0};
    std::atomic<size_t> mBytesTotal{0};
    std::atomic<size_t> mBytesDone{0};

    std::mutex mCurrentFileMutex;
    std::string mCurrentFile;

    std::mutex mEmitMutex;
};
