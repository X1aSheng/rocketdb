# Makefile for RocketDB
# Platform: Cross-platform (Linux, macOS, Windows with Clang/GCC)
# Usage: make [all|test|clean|rebuild|help]
#
# Test files (7 total):
#   test_kvdb_basic   — KVDB set/get/update/delete/write_gran/seq_wrap/mixed/corrupt/init/capacity
#   test_kvdb_stress  — KVDB GC stress, iterator under GC, power-loss, corrupt sector recovery
#   test_tsdb_basic   — TSDB append/query/epoch/recount/write_gran/max_boundaries
#   test_tsdb_stress  — TSDB rotation stress, append fail, CRC corruption, degraded
#   test_integration  — KV+TS combined: cycle stress, mixed workload, power-loss, wear
#   test_example      — Tutorial
#   test_fault_injection — Fault injection demo

CC      = clang
CFLAGS  = -Wall -Wextra -std=c99 -O2 -g -D_CRT_SECURE_NO_WARNINGS
INCLUDES = -Isrc -Itest/sim
LDFLAGS =
# Add -lm on platforms that need it (Linux, macOS; Windows UCRT doesn't)
UNAME_S := $(shell uname -s 2>/dev/null || echo Windows)
ifneq ($(UNAME_S),Windows)
    LDFLAGS += -lm
endif
OUTPUT_DIR = test/out

# Engine sources
ENGINE_SRCS = src/rocketdb_kvdb.c src/rocketdb_tsdb.c

# Sim library sources (shared objects)
SIM_SRCS = test/sim/test_framework.c \
           test/sim/sim_flash.c \
           test/sim/sim_fault.c \
           test/sim/sim_dist.c \
           test/sim/sim_crypto.c

# All test source files
TEST_SRCS = test/sim/test_kvdb_basic.c \
            test/sim/test_kvdb_stress.c \
            test/sim/test_tsdb_basic.c \
            test/sim/test_tsdb_stress.c \
            test/sim/test_integration.c \
            test/sim/test_example.c \
            test/sim/test_fault_injection.c

# Compute object files (flat, no subdirs)
ENGINE_OBJS = $(addprefix $(OUTPUT_DIR)/,$(notdir $(ENGINE_SRCS:.c=.o)))
SIM_OBJS    = $(addprefix $(OUTPUT_DIR)/,$(notdir $(SIM_SRCS:.c=.o)))
TEST_OBJS   = $(addprefix $(OUTPUT_DIR)/,$(notdir $(TEST_SRCS:.c=.o)))

# Test executables
TEST_EXES = $(TEST_OBJS:.o=.exe)

# Common link deps (every test needs these)
COMMON_OBJS = $(SIM_OBJS)

# Vpath so make can find sources in subdirs
VPATH = src:test/sim

# Default target
all: $(OUTPUT_DIR) $(TEST_EXES)

# Create output directory
$(OUTPUT_DIR):
	@mkdir -p "$(OUTPUT_DIR)"

# ── Shared object compile rule ──────────────────────────────────────────

$(OUTPUT_DIR)/%.o: %.c | $(OUTPUT_DIR)
	@echo Compiling $<...
	@$(CC) $(CFLAGS) $(INCLUDES) -c $< -o $@

# ── Test executable link rules ──────────────────────────────────────────

# KVDB basic: needs kvdb + sim modules (no fault)
$(OUTPUT_DIR)/test_kvdb_basic.exe: \
	$(OUTPUT_DIR)/test_kvdb_basic.o \
	$(OUTPUT_DIR)/test_framework.o \
	$(OUTPUT_DIR)/sim_flash.o \
	$(OUTPUT_DIR)/sim_fault.o \
	$(OUTPUT_DIR)/sim_dist.o \
	$(OUTPUT_DIR)/sim_crypto.o \
	$(OUTPUT_DIR)/rocketdb_kvdb.o
	@echo Linking $@...
	@$(CC) $(LDFLAGS) -o $@ $^

# KVDB stress: needs kvdb + fault
$(OUTPUT_DIR)/test_kvdb_stress.exe: \
	$(OUTPUT_DIR)/test_kvdb_stress.o \
	$(OUTPUT_DIR)/test_framework.o \
	$(OUTPUT_DIR)/sim_flash.o \
	$(OUTPUT_DIR)/sim_fault.o \
	$(OUTPUT_DIR)/sim_crypto.o \
	$(OUTPUT_DIR)/rocketdb_kvdb.o
	@echo Linking $@...
	@$(CC) $(LDFLAGS) -o $@ $^

# TSDB basic: needs tsdb
$(OUTPUT_DIR)/test_tsdb_basic.exe: \
	$(OUTPUT_DIR)/test_tsdb_basic.o \
	$(OUTPUT_DIR)/test_framework.o \
	$(OUTPUT_DIR)/sim_flash.o \
	$(OUTPUT_DIR)/sim_fault.o \
	$(OUTPUT_DIR)/sim_crypto.o \
	$(OUTPUT_DIR)/rocketdb_tsdb.o
	@echo Linking $@...
	@$(CC) $(LDFLAGS) -o $@ $^

# TSDB stress: needs tsdb + fault
$(OUTPUT_DIR)/test_tsdb_stress.exe: \
	$(OUTPUT_DIR)/test_tsdb_stress.o \
	$(OUTPUT_DIR)/test_framework.o \
	$(OUTPUT_DIR)/sim_flash.o \
	$(OUTPUT_DIR)/sim_fault.o \
	$(OUTPUT_DIR)/sim_crypto.o \
	$(OUTPUT_DIR)/rocketdb_tsdb.o
	@echo Linking $@...
	@$(CC) $(LDFLAGS) -o $@ $^

# Integration: needs kvdb + tsdb + fault + dist
$(OUTPUT_DIR)/test_integration.exe: \
	$(OUTPUT_DIR)/test_integration.o \
	$(OUTPUT_DIR)/test_framework.o \
	$(OUTPUT_DIR)/sim_flash.o \
	$(OUTPUT_DIR)/sim_fault.o \
	$(OUTPUT_DIR)/sim_dist.o \
	$(OUTPUT_DIR)/sim_crypto.o \
	$(OUTPUT_DIR)/rocketdb_kvdb.o \
	$(OUTPUT_DIR)/rocketdb_tsdb.o
	@echo Linking $@...
	@$(CC) $(LDFLAGS) -o $@ $^

# Example: needs kvdb
$(OUTPUT_DIR)/test_example.exe: \
	$(OUTPUT_DIR)/test_example.o \
	$(OUTPUT_DIR)/test_framework.o \
	$(OUTPUT_DIR)/sim_flash.o \
	$(OUTPUT_DIR)/sim_fault.o \
	$(OUTPUT_DIR)/sim_crypto.o \
	$(OUTPUT_DIR)/rocketdb_kvdb.o
	@echo Linking $@...
	@$(CC) $(LDFLAGS) -o $@ $^

# Fault injection demo: needs kvdb + fault
$(OUTPUT_DIR)/test_fault_injection.exe: \
	$(OUTPUT_DIR)/test_fault_injection.o \
	$(OUTPUT_DIR)/test_framework.o \
	$(OUTPUT_DIR)/sim_flash.o \
	$(OUTPUT_DIR)/sim_fault.o \
	$(OUTPUT_DIR)/sim_crypto.o \
	$(OUTPUT_DIR)/rocketdb_kvdb.o
	@echo Linking $@...
	@$(CC) $(LDFLAGS) -o $@ $^

# ── Run all tests ───────────────────────────────────────────────────────

TEST_BINS = test_kvdb_basic test_kvdb_stress test_tsdb_basic \
            test_tsdb_stress test_integration test_example test_fault_injection

test: $(TEST_EXES)
	@echo
	@echo ========================================
	@echo   Running RocketDB Test Suites
	@echo ========================================
	@echo
	@for t in $(TEST_BINS); do \
		echo "--- $$t ---"; \
		$(OUTPUT_DIR)/$$t.exe; \
		echo; \
	done

# ── Clean ───────────────────────────────────────────────────────────────

clean:
	@echo Cleaning output directory...
	@rm -rf "$(OUTPUT_DIR)"
	@echo Clean completed!

# ── Rebuild ─────────────────────────────────────────────────────────────

rebuild: clean all

# ── Help ────────────────────────────────────────────────────────────────

help:
	@echo Targets:
	@echo   all      Build all test executables (default)
	@echo   test     Build and run all tests
	@echo   clean    Remove test/out/
	@echo   rebuild  Clean then build
	@echo   help     Show this help
	@echo.
	@echo Test suites:
	@echo   test_kvdb_basic      KVDB basic functionality (8 cases)
	@echo   test_kvdb_stress     KVDB stress tests (4 cases)
	@echo   test_tsdb_basic      TSDB basic functionality (3 cases)
	@echo   test_tsdb_stress     TSDB stress tests (4 cases)
	@echo   test_integration     KVDB+TSDB integration (6 cases)
	@echo   test_example         Tutorial example
	@echo   test_fault_injection Fault injection demo

.PHONY: all test clean rebuild help
