CC ?= gcc
AR ?= ar
VALGRIND ?= valgrind
CPPFLAGS ?=
CFLAGS ?= -std=c11 -O2 -Wall -Wextra -Werror -pedantic
LDFLAGS ?=
LDLIBS ?= -pthread

BUILD_DIR := build
INCLUDE_DIR := include
LIB := $(BUILD_DIR)/libbenchmark_core.a
OBJ := $(BUILD_DIR)/benchmark_core.o
TEST_BIN := $(BUILD_DIR)/benchmark_core_smoke
DIST_DIR := dist

.PHONY: all clean test lint test_sanitize test_helgrind dist install

all: $(LIB)

$(BUILD_DIR):
	@mkdir -p $@

$(OBJ): src/benchmark_core.c include/benchmark_core.h | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -I$(INCLUDE_DIR) -c $< -o $@

$(LIB): $(OBJ)
	$(AR) rcs $@ $^

$(TEST_BIN): tests/benchmark_core_smoke.c $(LIB) | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -I$(INCLUDE_DIR) $< $(LIB) $(LDFLAGS) $(LDLIBS) -o $@

test: $(TEST_BIN)
	@$(TEST_BIN) --iterations 101 --warmup 5 --data-count 16 --input-kind nonzero --operation-kind xor --measure-mode kernel-only --size-profile medium --capacity-profile normal
	@$(TEST_BIN) --threads 2 --total-iterations 200 --warmup 5 --data-count 16 --input-kind mixed --operation-kind rotate --measure-mode end-to-end --size-profile near-capacity --capacity-profile near-capacity

lint:
	@cppcheck --std=c11 --enable=warning,style,performance,portability --error-exitcode=1 --suppress=missingIncludeSystem src tests

test_sanitize:
	@$(MAKE) --no-print-directory clean
	@$(MAKE) --no-print-directory \
		CFLAGS='-std=c11 -O1 -g -Wall -Wextra -Werror -pedantic -fsanitize=address,undefined -fno-omit-frame-pointer' \
		LDFLAGS='-fsanitize=address,undefined' LDLIBS='-pthread -fsanitize=address,undefined' test
	@echo "AddressSanitizer and UBSan: OK"

test_helgrind:
	@$(MAKE) --no-print-directory clean
	@$(MAKE) --no-print-directory $(TEST_BIN)
	@$(VALGRIND) --tool=helgrind --error-exitcode=42 $(TEST_BIN) \
		--threads 2 --total-iterations 200 --warmup 5 --data-count 16 \
		--input-kind mixed --operation-kind rotate --measure-mode end-to-end \
		--size-profile near-capacity --capacity-profile near-capacity
	@echo "Helgrind: OK"

dist install: all
	@rm -rf $(DIST_DIR)
	@mkdir -p $(DIST_DIR)/include $(DIST_DIR)/lib
	@cp $(INCLUDE_DIR)/benchmark_core.h $(DIST_DIR)/include/
	@cp $(LIB) $(DIST_DIR)/lib/
	@cp README.md LICENSE $(DIST_DIR)/
	@echo "Distribution created in $(DIST_DIR)/"

clean:
	@rm -rf $(BUILD_DIR) $(DIST_DIR)
