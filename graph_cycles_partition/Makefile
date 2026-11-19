CXX ?= g++
CXXFLAGS ?= -std=c++17 -O2 -Wall -Wextra -pedantic -Iinclude
AR ?= ar
ARFLAGS ?= rcs

LIB_OBJS = build/graph.o build/graph_cycles.o build/graph_partitioner.o
DRIVER_OBJS = build/graph_driver.o

.PHONY: all clean run

all: build/libutil_graph.a build/graph_driver

build:
	mkdir -p build

build/graph.o: src/graph.cc include/util_graph/graph.h | build
	$(CXX) $(CXXFLAGS) -c $< -o $@

build/graph_cycles.o: src/graph_cycles.cc include/util_graph/graph.h include/util_graph/graph_cycles.h | build
	$(CXX) $(CXXFLAGS) -c $< -o $@

build/graph_partitioner.o: src/graph_partitioner.cc include/util_graph/graph.h include/util_graph/graph_cycles.h include/util_graph/graph_partitioner.h | build
	$(CXX) $(CXXFLAGS) -c $< -o $@

build/graph_driver.o: tests/graph_driver.cc include/util_graph/graph.h include/util_graph/graph_cycles.h include/util_graph/graph_partitioner.h | build
	$(CXX) $(CXXFLAGS) -c $< -o $@

build/libutil_graph.a: $(LIB_OBJS)
	$(AR) $(ARFLAGS) $@ $^

build/graph_driver: $(DRIVER_OBJS) build/libutil_graph.a
	$(CXX) $(CXXFLAGS) $^ -o $@

run: build/graph_driver
	./build/graph_driver

clean:
	rm -rf build
