.PHONY: all func perf rfunc rperf clean help

MAKEFLAGS += -j32

PROJECT_ROOT := ../..
BUILD_DIR := $(PROJECT_ROOT)/build
LIB_DIR := $(BUILD_DIR)
INCLUDE_DIR := $(PROJECT_ROOT)/include
COMMON_INCLUDE_DIR := $(PROJECT_ROOT)/test/common/include

FUNCTEST_SRC_DIR := $(PROJECT_ROOT)/test/functest
PERFTEST_SRC_DIR := $(PROJECT_ROOT)/test/perftest

FUNCTEST_BIN := ./functest/functest
PERFTEST_BIN := ./perftest/perftest

GTEST_DIR := /usr/local/include/gtest/
GTEST_LIBS := -lgtest -lgtest_main -lpthread

CXX := g++
CXXFLAGS := -std=c++17 -Wall -Wextra -fopenmp -O3 -DPERF_ARMv8_ASM -fPIC
LDFLAGS := -fopenmp -lpthread

ifeq ($(DEBUG),1)
    CXXFLAGS += -g -O0
else
    CXXFLAGS += -O3 -D_FORTIFY_SOURCE=2 -fstack-protector-all
endif

CXXFLAGS += -march=armv8-a+crc
CXXFLAGS += -I$(INCLUDE_DIR) -I$(COMMON_INCLUDE_DIR) -I$(GTEST_DIR)

FUNCTEST_SOURCES := $(wildcard $(FUNCTEST_SRC_DIR)/*.cpp)
PERFTEST_SOURCES := $(wildcard $(PERFTEST_SRC_DIR)/*.cpp)

FUNCTEST_OBJECTS := $(patsubst $(FUNCTEST_SRC_DIR)/%.cpp,./functest/%.o,$(FUNCTEST_SOURCES))
PERFTEST_OBJECTS := $(patsubst $(PERFTEST_SRC_DIR)/%.cpp,./perftest/%.o,$(PERFTEST_SOURCES))

all: func perf

func: $(FUNCTEST_BIN)

perf: $(PERFTEST_BIN)

./functest/%.o: $(FUNCTEST_SRC_DIR)/%.cpp
	@mkdir -p ./functest
	@echo "Compiling $<..."
	@$(CXX) $(CXXFLAGS) -c $< -o $@

$(FUNCTEST_BIN): $(FUNCTEST_OBJECTS)
	@mkdir -p ./functest
	@echo "Linking functest..."
	@$(CXX) $(FUNCTEST_OBJECTS) -L$(LIB_DIR) -lreplaceAll_acc $(GTEST_LIBS) $(LDFLAGS) -o $@
	@echo "Built functest: $@"

./perftest/%.o: $(PERFTEST_SRC_DIR)/%.cpp
	@mkdir -p ./perftest
	@echo "Compiling $<..."
	@$(CXX) $(CXXFLAGS) -c $< -o $@

$(PERFTEST_BIN): $(PERFTEST_OBJECTS)
	@mkdir -p ./perftest
	@echo "Linking perftest..."
	@$(CXX) $(PERFTEST_OBJECTS) -L$(LIB_DIR) -lreplaceAll_acc $(GTEST_LIBS) $(LDFLAGS) -o $@
	@echo "Built perftest: $@"

rfunc:
	@if [ -z "$(func)" ]; then \
		echo "Running all functional tests..."; \
		$(FUNCTEST_BIN); \
	else \
		echo "Running functional test: $(func)"; \
		$(FUNCTEST_BIN) --gtest_filter=$(func); \
	fi

rperf:
	@if [ -z "$(perf)" ]; then \
		echo "Running all performance tests..."; \
		$(PERFTEST_BIN); \
	else \
		echo "Running performance test: $(perf)"; \
		$(PERFTEST_BIN) --gtest_filter=$(perf); \
	fi

clean:
	@echo "Cleaning..."
	@rm -rf ./functest ./perftest
	@echo "Clean complete"

help:
	@echo "Available targets:"
	@echo "  make              - Build all tests (func and perf)"
	@echo "  make func         - Build functest"
	@echo "  make perf         - Build perftest"
	@echo "  make rfunc        - Run all functional tests"
	@echo "  make rperf        - Run all performance tests"
	@echo "  make rfunc func=TEST_NAME - Run specific functional test"
	@echo "  make rperf perf=TEST_NAME - Run specific performance test"
	@echo "  make clean        - Clean build artifacts"
	@echo "  make DEBUG=1      - Build with debug symbols"
	@echo ""
	@echo "Examples:"
	@echo "  make rfunc func=ReplaceAllAcc.ExecMatch1"
	@echo "  make rperf perf=ReplaceAllAcc.BASIC_PERF001"

.DEFAULT_GOAL := all
