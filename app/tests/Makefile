# Host-side tests for the commit pipeline and the save storage rules. Needs only g++ or clang++,
# no devkitARM.
#   make test             build and run
#   make test SEED=1234   rerun the randomized drafts with another seed

CXXFLAGS ?= -std=gnu++20 -O1 -g -Wall -Wextra
PKSM_CORE := ../vendor/PKSM-Core
INCLUDES := -I../include -I$(PKSM_CORE)/include -I$(PKSM_CORE)/include/enums
SOURCES := CommitTests.cpp ../source/bank/commit/CommitPlanner.cpp ../source/bank/commit/CommitExecutor.cpp
HEADERS := $(wildcard *.hpp ../include/bank/*.hpp ../include/bank/commit/*.hpp ../include/save/pokemon/*.hpp)
BINARY := build/commit_tests

# host/ stands in for <3ds.h> and POSIX on MinGW; HostStubs.cpp replaces the libctru lock and logger.
SAVE_SOURCES := SaveTests.cpp HostStubs.cpp ../source/save/adapter/DsSave.cpp ../source/save/storage/SaveFiles.cpp \
	../source/save/storage/SaveBackups.cpp ../source/platform/FileSystem.cpp ../source/platform/AppPaths.cpp \
	$(PKSM_CORE)/source/utils/crypto_checksums.cpp
SAVE_HEADERS := $(wildcard host/*.h ../include/save/adapter/DsSave.hpp ../include/save/storage/*.hpp ../include/platform/*.hpp)
SAVE_BINARY := build/save_tests

.PHONY: test clean

test: $(BINARY) $(SAVE_BINARY)
	./$(BINARY) $(if $(SEED),--seed $(SEED))
	./$(SAVE_BINARY)

$(BINARY): $(SOURCES) $(HEADERS)
	@mkdir -p build
	$(CXX) $(CXXFLAGS) $(INCLUDES) $(SOURCES) -o $@

$(SAVE_BINARY): $(SAVE_SOURCES) $(SAVE_HEADERS)
	@mkdir -p build
	$(CXX) $(CXXFLAGS) -Ihost -include host/compat.h $(INCLUDES) $(SAVE_SOURCES) -o $@

clean:
	rm -rf build
