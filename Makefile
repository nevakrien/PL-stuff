CC ?= cc
CFLAGS ?= -std=c2x -Wall -Wextra -g -fsanitize=address

HEADERS = vm.h ir.h front_end.h
VM_IR_TEST_BIN := tests/vm_ir_tests
VM_IR_TEST_SRCS := tests/vm_ir_tests.c vm.c ir.c
FRONT_END_TEST_BIN := tests/front_end_tests
FRONT_END_TEST_SRCS := tests/front_end_tests.c front_end.c ir.c
TEST_BINS := $(VM_IR_TEST_BIN) $(FRONT_END_TEST_BIN)

.PHONY: all test clean

all: $(TEST_BINS)

$(VM_IR_TEST_BIN): $(VM_IR_TEST_SRCS) $(HEADERS)
	$(CC) $(CFLAGS) $(VM_IR_TEST_SRCS) -o $@

$(FRONT_END_TEST_BIN): $(FRONT_END_TEST_SRCS) $(HEADERS)
	$(CC) $(CFLAGS) $(FRONT_END_TEST_SRCS) -o $@

test: $(TEST_BINS)
	./$(VM_IR_TEST_BIN)
	./$(FRONT_END_TEST_BIN)

clean:
	rm -f $(TEST_BINS)
