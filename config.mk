# Auto-generated configuration makefile
# DO NOT EDIT MANUALLY - run configure script instead

CXX ?= g++
CXXFLAGS = -g -std=c++20 -Wall -Wextra -I. -O2
LDFLAGS = -laio -lcrypto -lssl -lxxhash
SOURCES = main.cpp lib/acp/acp.cpp base/base.cpp lib/combined/combined.cpp lib/mainlib.cpp
LDFLAGS += -luring
SOURCES += lib/ucp/ucp.cpp

# Feature flags
CXXFLAGS += -DENABLE_LIBURING
