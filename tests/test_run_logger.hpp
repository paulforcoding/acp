#pragma once

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <string>
#include <thread>
#include <unistd.h>

// Write to a persistent, unbuffered log file that survives crashes and reboots
inline void RawLog(const char *msg)
{
    std::FILE *f = std::fopen("test_run.log", "a");
    if (!f)
        return;
    std::setbuf(f, nullptr); // unbuffered
    std::time_t now = std::time(nullptr);
    char buf[64];
    std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%S", std::localtime(&now));
    std::fprintf(f, "[%s] [pid=%d] %s\n", buf, static_cast<int>(getpid()), msg);
    std::fflush(f);
    int fd = fileno(f);
    if (fd >= 0)
        fsync(fd);
    std::fclose(f);
}

inline void LogTestEvent(const char *event, const char *testName)
{
    char msg[512];
    std::snprintf(msg, sizeof(msg), "%s: %s", event, testName);
    RawLog(msg);
}

struct TestBodyLogger
{
    explicit TestBodyLogger(const char *name) : mName(name)
    {
        LogTestEvent("BODY_START", mName);
    }
    ~TestBodyLogger()
    {
        LogTestEvent("BODY_END", mName);
    }
    const char *mName;
};

#define LOG_TEST_SCOPE(name) TestBodyLogger _testBodyLogger(name)

// TestWatchdog: detects hangs and dumps diagnostics before aborting.
// Usage: create at the start of a test, destroy when done.
// If the test doesn't complete within timeoutSeconds, the watchdog
// dumps /proc/self/status and the acp program log, then aborts.
class TestWatchdog
{
public:
    TestWatchdog(int timeoutSeconds, const char *testName)
        : mTimeoutSeconds(timeoutSeconds), mTestName(testName), mDone(false)
    {
        char msg[256];
        std::snprintf(msg, sizeof(msg), "Watchdog: starting for '%s', timeout=%ds", mTestName, mTimeoutSeconds);
        RawLog(msg);
        mThread = std::thread(&TestWatchdog::Run, this);
    }

    ~TestWatchdog()
    {
        mDone.store(true);
        if (mThread.joinable())
            mThread.join();
    }

private:
    void Run()
    {
        for (int i = 0; i < mTimeoutSeconds; ++i)
        {
            if (mDone.load())
                return;
            std::this_thread::sleep_for(std::chrono::seconds(1));
        }
        if (!mDone.load())
        {
            char msg[256];
            std::snprintf(msg, sizeof(msg),
                          "Watchdog: TIMEOUT after %ds for test '%s' — dumping diagnostics and aborting",
                          mTimeoutSeconds, mTestName);
            RawLog(msg);
            DumpDiagnostics();
            std::abort();
        }
    }

    void DumpDiagnostics()
    {
        RawLog("=== Watchdog diagnostic dump begin ===");

        // Dump /proc/self/status (Linux only)
        std::FILE *proc = std::fopen("/proc/self/status", "r");
        if (proc)
        {
            char line[256];
            RawLog("--- /proc/self/status ---");
            while (std::fgets(line, sizeof(line), proc))
            {
                line[std::strcspn(line, "\n")] = '\0';
                RawLog(line);
            }
            std::fclose(proc);
        }

        // Dump /proc/self/wchan (what the main thread is waiting on)
        std::FILE *wchan = std::fopen("/proc/self/wchan", "r");
        if (wchan)
        {
            char buf[256];
            if (std::fgets(buf, sizeof(buf), wchan))
            {
                buf[std::strcspn(buf, "\n")] = '\0';
                char msg[512];
                std::snprintf(msg, sizeof(msg), "main wchan: %s", buf);
                RawLog(msg);
            }
            std::fclose(wchan);
        }

        // Dump /proc/self/stack if available
        std::FILE *stack = std::fopen("/proc/self/stack", "r");
        if (stack)
        {
            char line[256];
            RawLog("--- /proc/self/stack ---");
            while (std::fgets(line, sizeof(line), stack))
            {
                line[std::strcspn(line, "\n")] = '\0';
                RawLog(line);
            }
            std::fclose(stack);
        }

        // Dump /proc/self/task/*/wchan for all threads
        // (simplified - just list thread IDs)
        RawLog("--- Thread wchan summary ---");
        std::string cmd = "ls /proc/" + std::to_string(getpid()) + "/task/ 2>/dev/null";
        std::FILE *taskDir = popen(cmd.c_str(), "r");
        if (taskDir)
        {
            char tid[64];
            while (std::fgets(tid, sizeof(tid), taskDir))
            {
                tid[std::strcspn(tid, "\n")] = '\0';
                std::string wchanPath = "/proc/" + std::to_string(getpid()) + "/task/" + tid + "/wchan";
                std::FILE *tw = std::fopen(wchanPath.c_str(), "r");
                if (tw)
                {
                    char wbuf[256];
                    if (std::fgets(wbuf, sizeof(wbuf), tw))
                    {
                        wbuf[std::strcspn(wbuf, "\n")] = '\0';
                        char msg[512];
                        std::snprintf(msg, sizeof(msg), "  tid %s: %s", tid, wbuf);
                        RawLog(msg);
                    }
                    std::fclose(tw);
                }
            }
            pclose(taskDir);
        }

        // Dump the acp program log if it exists
        std::FILE *acpLog = std::fopen("/tmp/acp_program.log", "r");
        if (acpLog)
        {
            char line[512];
            RawLog("--- /tmp/acp_program.log (last 50 lines) ---");
            // Collect last 50 lines using a circular buffer
            std::string ring[50];
            int idx = 0, total = 0;
            while (std::fgets(line, sizeof(line), acpLog))
            {
                line[std::strcspn(line, "\n")] = '\0';
                ring[idx % 50] = line;
                idx++;
                total++;
            }
            std::fclose(acpLog);
            int start = total > 50 ? total - 50 : 0;
            for (int i = start; i < total; ++i)
            {
                char msg[600];
                std::snprintf(msg, sizeof(msg), "  %s", ring[i % 50].c_str());
                RawLog(msg);
            }
        }

        RawLog("=== Watchdog diagnostic dump end ===");
    }

    int mTimeoutSeconds;
    const char *mTestName;
    std::atomic<bool> mDone;
    std::thread mThread;
};
