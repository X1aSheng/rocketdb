# Makefile for RocketDB Performance Benchmarks

CC := gcc
CFLAGS := -O2 -Wall -Wextra -std=c99 -D_POSIX_C_SOURCE=200809L
LDFLAGS :=

ROOT := ../..
OUTPUT_DIR := ../out
TARGET := $(OUTPUT_DIR)/benchmark
INC := -I$(ROOT)/src -I$(ROOT)/tests/sim
VPATH := $(ROOT)/src:$(ROOT)/tests/sim:.

OBJS := $(OUTPUT_DIR)/rocketdb_kvdb.o \
	$(OUTPUT_DIR)/rocketdb_tsdb.o \
	$(OUTPUT_DIR)/sim_flash.o \
	$(OUTPUT_DIR)/sim_fault.o \
	$(OUTPUT_DIR)/sim_trace.o \
	$(OUTPUT_DIR)/sim_crypto.o \
	$(OUTPUT_DIR)/scenarios.o

all: $(OUTPUT_DIR) $(TARGET)

$(OUTPUT_DIR):
	mkdir -p "$(OUTPUT_DIR)"

$(TARGET): $(OBJS) | $(OUTPUT_DIR)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)
	@echo "Built $(TARGET)"

$(OUTPUT_DIR)/%.o: %.c | $(OUTPUT_DIR)
	$(CC) $(CFLAGS) $(INC) -c -o $@ $<

run: $(TARGET)
	@echo "========================================="
	@echo " Running RocketDB Performance Benchmark"
	@echo "========================================="
	./$(TARGET) | tee $(OUTPUT_DIR)/results_`date +%Y%m%d_%H%M%S`.csv

clean:
	rm -f $(OBJS) $(TARGET)
	@echo "Cleaned build artifacts"

distclean: clean
	rm -f $(OUTPUT_DIR)/results_*.txt $(OUTPUT_DIR)/results_*.csv
	@echo "Cleaned all artifacts and results"

.PHONY: all run clean distclean
