#pragma once

#include <cstdio>
#include <ctime>
#include <string>
#include <unistd.h>

inline void LogTestEvent(const char* event, const char* testName) {
    std::FILE* f = std::fopen("test_run.log", "a");
    if (!f) {
        return;
    }
    std::setbuf(f, nullptr);
    std::time_t now = std::time(nullptr);
    char buf[64];
    std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%S", std::localtime(&now));
    std::fprintf(f, "[%s] %s: %s\n", buf, event, testName);
    std::fflush(f);
    int fd = fileno(f);
    if (fd >= 0) {
        fsync(fd);
    }
    std::fclose(f);
}

struct TestBodyLogger {
    explicit TestBodyLogger(const char* name) : mName(name) {
        LogTestEvent("BODY_START", mName);
    }
    ~TestBodyLogger() {
        LogTestEvent("BODY_END", mName);
    }
    const char* mName;
};

#define LOG_TEST_SCOPE(name) TestBodyLogger _testBodyLogger(name)
