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

# Test target (requires Catch2 amalgamated available under lib/thirdparty/catch2)
TEST_SRC := $(wildcard tests/*.cpp)
CATCH_SRC := lib/thirdparty/catch2/catch_amalgamated.cpp
TEST_OBJS := $(patsubst %.cpp,%.o,$(TEST_SRC))
CATCH_OBJ := $(patsubst %.cpp,%.o,$(CATCH_SRC))
TEST_BIN := tests/test_all
.PHONY: test
test: $(TEST_BIN)

$(TEST_BIN): $(TEST_OBJS) $(CATCH_OBJ)
	$(CXX) $(CXXFLAGS) -I. -Ilib/thirdparty/catch2 -o $(TEST_BIN) $(TEST_OBJS) $(CATCH_OBJ) -pthread

%.o: %.cpp
	$(CXX) $(CXXFLAGS) -I. -Ilib/thirdparty/catch2 -c $< -o $@

$(CATCH_OBJ): $(CATCH_SRC)
	$(CXX) $(CXXFLAGS) -I. -Ilib/thirdparty/catch2 -c $< -o $@
