# Host-side tests for the commit pipeline, the save storage rules and the cross-game rules. Needs only
# g++ or clang++, no devkitARM.
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

# compat_tests links all of PKSM-Core; pksm-host/ points it at the data in ../romfs.
PKSM_FLAGS := -std=gnu++20 -O1 -w
PKSM_INCLUDES := -Ipksm-host -I../include $(addprefix -isystem $(PKSM_CORE)/,include include/enums include/personal memecrypto source/utils pcg-cpp/include)
PKSM_OBJECTS := $(patsubst $(PKSM_CORE)/%,build/pksm/%.o,$(wildcard $(PKSM_CORE)/source/*/*.cpp $(PKSM_CORE)/memecrypto/*.c))
COMPAT_SOURCES := CompatTests.cpp ../source/save/pokemon/PokemonTransfer.cpp
COMPAT_BINARY := build/compat_tests

.PHONY: test clean

test: $(BINARY) $(SAVE_BINARY) $(COMPAT_BINARY)
	./$(BINARY) $(if $(SEED),--seed $(SEED))
	./$(SAVE_BINARY)
	./$(COMPAT_BINARY)

$(BINARY): $(SOURCES) $(HEADERS)
	@mkdir -p build
	$(CXX) $(CXXFLAGS) $(INCLUDES) $(SOURCES) -o $@

$(SAVE_BINARY): $(SAVE_SOURCES) $(SAVE_HEADERS)
	@mkdir -p build
	$(CXX) $(CXXFLAGS) -Ihost -include host/compat.h $(INCLUDES) $(SAVE_SOURCES) -o $@

$(COMPAT_BINARY): $(COMPAT_SOURCES) $(HEADERS) pksm-host/PKSMCORE_CONFIG.h $(PKSM_OBJECTS)
	@mkdir -p build
	$(CXX) $(CXXFLAGS) $(PKSM_INCLUDES) $(COMPAT_SOURCES) $(PKSM_OBJECTS) -o $@

build/pksm/%.cpp.o: $(PKSM_CORE)/%.cpp pksm-host/PKSMCORE_CONFIG.h
	@mkdir -p $(dir $@)
	$(CXX) $(PKSM_FLAGS) $(PKSM_INCLUDES) -c $< -o $@

build/pksm/%.c.o: $(PKSM_CORE)/%.c
	@mkdir -p $(dir $@)
	$(CC) -O1 -w -I$(PKSM_CORE)/memecrypto -c $< -o $@

clean:
	rm -rf build
