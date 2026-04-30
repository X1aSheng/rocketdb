# Makefile for RocketDB Performance Benchmarks

CC := gcc
CFLAGS := -O2 -Wall -Wextra -std=c99
LDFLAGS :=

# Include paths
INC := -I../../ -I../sim

# Source files
SRCS := scenarios.c ../sim/sim_flash.c
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
	./$(TARGET) | tee results_`date +%Y%m%d_%H%M%S`.txt

# Clean build artifacts
clean:
	rm -f $(OBJS) $(TARGET)
	@echo "✓ Cleaned build artifacts"

# Clean all including results
distclean: clean
	rm -f results_*.txt
	@echo "✓ Cleaned all artifacts and results"

.PHONY: all run clean distclean
