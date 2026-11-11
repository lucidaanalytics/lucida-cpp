# `make test`: builds and runs the C++ core tests and the C API test (clang/gcc, no other deps).
CXX ?= clang++
CC ?= clang
OUT ?= build
WARN = -Wall -Wextra -Wshadow -Werror
# -fno-exceptions -fno-rtti: the core must build the way Unreal compiles plugin code.
CXXFLAGS = -std=c++17 -O1 $(WARN) -fno-exceptions -fno-rtti -Iinclude

test: $(OUT)/test_lucida $(OUT)/test_flags $(OUT)/test_c
	$(OUT)/test_lucida
	$(OUT)/test_flags
	$(OUT)/test_c

HEADERS = include/lucida/lucida.hpp include/lucida/json.hpp include/lucida/flags.hpp

$(OUT)/test_flags: tests/test_flags.cpp $(HEADERS)
	mkdir -p $(OUT)
	$(CXX) $(CXXFLAGS) -o $@ tests/test_flags.cpp

$(OUT)/test_lucida: tests/test_lucida.cpp include/lucida/lucida.hpp
	mkdir -p $(OUT)
	$(CXX) $(CXXFLAGS) -o $@ tests/test_lucida.cpp

$(OUT)/test_c: tests/test_c.c src/lucida_c.cpp include/lucida/lucida.h $(HEADERS)
	mkdir -p $(OUT)
	$(CC) -std=c99 $(WARN) -Iinclude -c -o $(OUT)/test_c.o tests/test_c.c
	$(CXX) $(CXXFLAGS) -c -o $(OUT)/lucida_c.o src/lucida_c.cpp
	$(CXX) -o $@ $(OUT)/test_c.o $(OUT)/lucida_c.o

clean:
	rm -rf $(OUT)

.PHONY: test clean
