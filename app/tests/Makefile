# Host-side tests for the commit pipeline. Needs only g++ or clang++, no devkitARM.
#   make test             build and run
#   make test SEED=1234   rerun the randomized drafts with another seed

CXXFLAGS ?= -std=gnu++20 -O1 -g -Wall -Wextra
PKSM_CORE := ../vendor/PKSM-Core
INCLUDES := -I../include -I$(PKSM_CORE)/include -I$(PKSM_CORE)/include/enums
SOURCES := CommitTests.cpp ../source/bank/commit/CommitPlanner.cpp ../source/bank/commit/CommitExecutor.cpp
HEADERS := $(wildcard *.hpp ../include/bank/*.hpp ../include/bank/commit/*.hpp ../include/save/pokemon/*.hpp)
BINARY := build/commit_tests

.PHONY: test clean

test: $(BINARY)
	./$(BINARY) $(if $(SEED),--seed $(SEED))

$(BINARY): $(SOURCES) $(HEADERS)
	@mkdir -p build
	$(CXX) $(CXXFLAGS) $(INCLUDES) $(SOURCES) -o $@

clean:
	rm -rf build
