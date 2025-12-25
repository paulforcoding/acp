# Makefile for AIO File Copy Tool
CXX = g++
CXXFLAGS = -g -std=c++20 -Wall -Wextra -I. -O2
LDFLAGS = -laio -luring
SOURCES = main.cpp lib/acp/acp.cpp base/base.cpp lib/combined/combined.cpp 
OBJECTS = $(SOURCES:.cpp=.o)
TARGET = acp

all: $(TARGET)
$(TARGET): $(OBJECTS)
	$(CXX) $(LDFLAGS) -o $@ $^

%.o: %.cpp
	$(CXX) $(CXXFLAGS) -c $< -o $@

clean:
	rm -f $(OBJECTS) $(TARGET)
.PHONY: all clean
