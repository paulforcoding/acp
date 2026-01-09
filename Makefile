# Makefile for AIO File Copy Tool
# Auto-generated configuration is included below
-include config.mk

# If config.mk doesn't exist, provide defaults
CXX ?= g++
CXXFLAGS ?= -g -std=c++20 -Wall -Wextra -I. -O2
LDFLAGS ?= -laio

SOURCES ?= main.cpp lib/acp/acp.cpp base/base.cpp lib/combined/combined.cpp
OBJECTS = $(SOURCES:.cpp=.o)
TARGET = acp

all: check-config $(TARGET)

check-config:
	@if [ ! -f config.h ] || [ ! -f config.mk ]; then \
		echo "Error: Configuration files not found!"; \
		echo "Please run: ./configure"; \
		exit 1; \
	fi

$(TARGET): $(OBJECTS)
	$(CXX) -o $@ $^ $(LDFLAGS)

%.o: %.cpp config.h
	$(CXX) $(CXXFLAGS) -c $< -o $@

clean:
	rm -f $(OBJECTS) $(TARGET) ${TEST_BIN}

distclean: clean
	rm -f config.h config.mk

.PHONY: all clean distclean check-config

# Test target (requires Catch2 available under lib/thirdparty)
TEST_SRC := $(wildcard tests/*.cpp)
TEST_BIN := tests/test_all
.PHONY: test
test:
	$(CXX) $(CXXFLAGS) -I. -o $(TEST_BIN) $(TEST_SRC) -pthread
