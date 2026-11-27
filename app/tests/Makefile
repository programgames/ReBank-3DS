# Host-side tests for the commit pipeline and the cross-game rules. Needs only g++ or clang++,
# no devkitARM.
#   make test             build and run
#   make test SEED=1234   rerun the randomized drafts with another seed

CXXFLAGS ?= -std=gnu++20 -O1 -g -Wall -Wextra
PKSM_CORE := ../vendor/PKSM-Core
INCLUDES := -I../include -I$(PKSM_CORE)/include -I$(PKSM_CORE)/include/enums
SOURCES := CommitTests.cpp ../source/bank/commit/CommitPlanner.cpp ../source/bank/commit/CommitExecutor.cpp
HEADERS := $(wildcard *.hpp ../include/bank/*.hpp ../include/bank/commit/*.hpp ../include/save/pokemon/*.hpp)
BINARY := build/commit_tests

# compat_tests links all of PKSM-Core; pksm-host/ points it at the data in ../romfs.
PKSM_FLAGS := -std=gnu++20 -O1 -w
PKSM_INCLUDES := -Ipksm-host -I../include $(addprefix -isystem $(PKSM_CORE)/,include include/enums include/personal memecrypto source/utils pcg-cpp/include)
PKSM_OBJECTS := $(patsubst $(PKSM_CORE)/%,build/pksm/%.o,$(wildcard $(PKSM_CORE)/source/*/*.cpp $(PKSM_CORE)/memecrypto/*.c))
COMPAT_SOURCES := CompatTests.cpp ../source/save/pokemon/PokemonTransfer.cpp
COMPAT_BINARY := build/compat_tests

.PHONY: test clean

test: $(BINARY) $(COMPAT_BINARY)
	./$(BINARY) $(if $(SEED),--seed $(SEED))
	./$(COMPAT_BINARY)

$(BINARY): $(SOURCES) $(HEADERS)
	@mkdir -p build
	$(CXX) $(CXXFLAGS) $(INCLUDES) $(SOURCES) -o $@

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
