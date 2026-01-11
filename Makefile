# Makefile for AIO File Copy Tool
# Auto-generated configuration is included below
-include config.mk

# If config.mk doesn't exist, provide defaults
CXX ?= g++
CXXFLAGS ?= -g -std=c++20 -Wall -Wextra -I. -O2
# Linker flags: async IO + crypto/hash libs
LDFLAGS ?= -laio -luring -lcrypto -lssl -lxxhash

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

# Test target (requires Catch2 amalgamated available under lib/thirdparty/catch2)
TEST_SRC := $(wildcard tests/*.cpp)
CATCH_SRC := lib/thirdparty/catch2/catch_amalgamated.cpp
TEST_OBJS := $(patsubst %.cpp,%.o,$(TEST_SRC))
CATCH_OBJ := $(patsubst %.cpp,%.o,$(CATCH_SRC))
TEST_BIN := tests/test_all
.PHONY: test
test: $(TEST_BIN)

# Implementation objects required by tests (provide symbols used by test code)
IMPLEMENTATION_OBJS := lib/combined/combined.o base/base.o lib/acp/acp.o lib/mainlib.o lib/ucp/ucp.o

$(TEST_BIN): $(TEST_OBJS) $(CATCH_OBJ) $(IMPLEMENTATION_OBJS)
	$(CXX) $(CXXFLAGS) -I. -Ilib/thirdparty/catch2 -o $@ $^ -pthread $(LDFLAGS)

# Compile rules: tests and vendored Catch2
tests/%.o: tests/%.cpp
	$(CXX) $(CXXFLAGS) -I. -Ilib/thirdparty/catch2 -c $< -o $@

lib/thirdparty/catch2/%.o: lib/thirdparty/catch2/%.cpp
	$(CXX) $(CXXFLAGS) -I. -Ilib/thirdparty/catch2 -c $< -o $@

# Default rule for project sources (depend on config.h)
%.o: %.cpp config.h
	$(CXX) $(CXXFLAGS) -c $< -o $@
