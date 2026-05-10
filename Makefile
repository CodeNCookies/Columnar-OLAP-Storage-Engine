CXX      := g++
CXXFLAGS := -std=c++17 -O2 -Wall -Wextra -Iinclude

SRC := src/csv_parser.cpp src/schema.cpp src/column_rw.cpp src/query.cpp src/loader.cpp

.PHONY: all clean test test2

all: colsh test_phase1 test_phase2

colsh: $(SRC) src/main.cpp
	$(CXX) $(CXXFLAGS) -o $@ $^

test_phase1: $(SRC) tests/test_phase1.cpp
	$(CXX) $(CXXFLAGS) -o $@ $^

test_phase2: $(SRC) tests/test_phase2.cpp
	$(CXX) $(CXXFLAGS) -o $@ $^

test: test_phase1
	./test_phase1

test2: test_phase2
	./test_phase2

clean:
	rm -f colsh test_phase1 test_phase2
	rm -rf warehouse /tmp/colap_test_table /tmp/colap_e2e_warehouse
