CXX ?= g++
CXXFLAGS ?= -std=c++17 -O2 -Wall -Wextra -Wpedantic -Werror

.PHONY: all test clean

all: max_min_te_b4_controller

max_min_te_b4_controller: max_min_te_b4_controller.cpp
	$(CXX) $(CXXFLAGS) $< -o $@

test: max_min_te_b4_controller
	./max_min_te_b4_controller --self-test

clean:
	rm -f max_min_te_b4_controller
