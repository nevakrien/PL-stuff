CC ?= cc
CFLAGS ?= -std=c2x -Wall -Wextra -g -fsanitize=address

HEADERS = vm.h ir.h verification.h frontend.h
VM_IR_TEST_BIN := tests/vm_ir_tests
VM_IR_TEST_SRCS := tests/vm_ir_tests.c vm.c ir.c
VERIFICATION_TEST_BIN := tests/verification_tests
VERIFICATION_TEST_SRCS := tests/verification_tests.c verification.c ir.c
FRONTEND_TEST_BIN := tests/frontend_tests
FRONTEND_TEST_SRCS := tests/frontend_tests.c frontend.c vm.c verification.c ir.c
TEST_BINS := $(VM_IR_TEST_BIN) $(VERIFICATION_TEST_BIN) $(FRONTEND_TEST_BIN)

.PHONY: all test clean

all: $(TEST_BINS)

$(VM_IR_TEST_BIN): $(VM_IR_TEST_SRCS) $(HEADERS)
	$(CC) $(CFLAGS) $(VM_IR_TEST_SRCS) -o $@

$(VERIFICATION_TEST_BIN): $(VERIFICATION_TEST_SRCS) $(HEADERS)
	$(CC) $(CFLAGS) $(VERIFICATION_TEST_SRCS) -o $@

$(FRONTEND_TEST_BIN): $(FRONTEND_TEST_SRCS) $(HEADERS)
	$(CC) $(CFLAGS) $(FRONTEND_TEST_SRCS) -o $@

test: $(TEST_BINS)
	./$(VM_IR_TEST_BIN)
	./$(VERIFICATION_TEST_BIN)
	./$(FRONTEND_TEST_BIN)

clean:
	rm -f $(TEST_BINS)
