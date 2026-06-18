# ==========================================================
# Lab 6
# MVCC Transaction Processing Engine
# Strict Two-Phase Locking + Deadlock Detection
# ==========================================================

CXX := g++
CXXFLAGS := -std=c++17 -Wall -Wextra -Wpedantic -pthread -O2

TARGET := transaction_engine
SRC := main.cpp

.PHONY: all build run clean

all: build

build: $(TARGET)

$(TARGET): $(SRC)
	$(CXX) $(CXXFLAGS) -o $@ $<

run: build
	./$(TARGET)

clean:
	rm -f $(TARGET)