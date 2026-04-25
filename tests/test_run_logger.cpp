#include "lib/thirdparty/catch2/catch_amalgamated.hpp"
#include <cstdio>
#include <ctime>
#include <string>
#include <unistd.h>

class TestRunListener : public Catch::EventListenerBase {
public:
    using Catch::EventListenerBase::EventListenerBase;

    void testRunStarting(Catch::TestRunInfo const&) override {
        mLogFile = std::fopen("test_run.log", "a");
        if (mLogFile) {
            std::setbuf(mLogFile, nullptr);
            WriteLog("=== TEST RUN START ===");
        }
    }

    void testCaseStarting(Catch::TestCaseInfo const& testInfo) override {
        WriteLog("START: " + std::string(testInfo.name));
    }

    void testCaseEnded(Catch::TestCaseStats const& stats) override {
        std::string msg = "END: " + std::string(stats.testInfo->name);
        msg += " | result=" + std::string(stats.totals.assertions.allPassed() ? "PASS" : "FAIL");
        WriteLog(msg);
    }

    void testRunEnded(Catch::TestRunStats const&) override {
        WriteLog("=== TEST RUN END ===");
        if (mLogFile) {
            std::fflush(mLogFile);
            std::fclose(mLogFile);
            mLogFile = nullptr;
        }
    }

private:
    std::FILE* mLogFile = nullptr;

    void WriteLog(const std::string& msg) {
        if (!mLogFile) {
            return;
        }
        std::time_t now = std::time(nullptr);
        char buf[64];
        std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%S", std::localtime(&now));
        std::fprintf(mLogFile, "[%s] %s\n", buf, msg.c_str());
        std::fflush(mLogFile);
        int fd = fileno(mLogFile);
        if (fd >= 0) {
            fsync(fd);
        }
    }
};

CATCH_REGISTER_LISTENER(TestRunListener)
