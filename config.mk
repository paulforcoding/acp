# Auto-generated configuration makefile
# DO NOT EDIT MANUALLY - run configure script instead

CXX ?= g++
CXXFLAGS = -g -std=c++20 -Wall -Wextra -I. -O2 -DSPDLOG_FMT_EXTERNAL
LDFLAGS = -laio -lcrypto -lssl -lxxhash
SOURCES = main.cpp lib/mainlib.cpp lib/acp/acp.cpp base/base.cpp lib/combined/combined.cpp
LDFLAGS += -luring
SOURCES += lib/ucp/ucp.cpp

# Feature flags
CXXFLAGS += -DENABLE_LIBURING
