CXX      := g++
CXXFLAGS := -std=c++17 -O2 -Wall -Wextra -Iinclude

SRCS     := src/graph.cpp src/main.cpp
TEST_SRC := tests/test_bgp.cpp src/graph.cpp

.PHONY: all clean test

all: bgp_sim

bgp_sim: $(SRCS) include/bgp_sim.h
	$(CXX) $(CXXFLAGS) $(SRCS) -o bgp_sim

test: $(TEST_SRC) include/bgp_sim.h
	$(CXX) $(CXXFLAGS) $(TEST_SRC) -o run_tests
	./run_tests

clean:
	rm -f bgp_sim run_tests
