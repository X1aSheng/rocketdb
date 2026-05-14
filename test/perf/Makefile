# Makefile for RocketDB Performance Benchmarks

CC := gcc
CFLAGS := -O2 -Wall -Wextra -std=c99 -D_POSIX_C_SOURCE=200809L
LDFLAGS :=

# Include paths
INC := -I../../src -I../sim

# Source files
SRCS := ../../src/rocketdb_kvdb.c ../../src/rocketdb_tsdb.c \
	../sim/sim_flash.c ../sim/sim_fault.c ../sim/sim_trace.c \
	../sim/sim_crypto.c scenarios.c
OBJS := $(SRCS:.c=.o)
TARGET := benchmark

# Default target
all: $(TARGET)

# Build executable
$(TARGET): $(OBJS)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)
	@echo "✓ Built $(TARGET)"

# Compile C files
%.o: %.c
	$(CC) $(CFLAGS) $(INC) -c -o $@ $<

# Run benchmark
run: $(TARGET)
	@echo "========================================="
	@echo " Running RocketDB Performance Benchmark"
	@echo "========================================="
	./$(TARGET) | tee results_`date +%Y%m%d_%H%M%S`.csv

# Clean build artifacts
clean:
	rm -f $(OBJS) $(TARGET)
	@echo "✓ Cleaned build artifacts"

# Clean all including results
distclean: clean
	rm -f results_*.txt results_*.csv
	@echo "✓ Cleaned all artifacts and results"

.PHONY: all run clean distclean
