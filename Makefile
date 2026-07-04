CC ?= cc
CFLAGS ?= -std=c2x -Wall -Wextra -g

HEADERS = vm.h ir.h
TEST_BIN := tests/vm_ir_tests
TEST_SRCS := tests/vm_ir_tests.c vm.c ir.c

.PHONY: all test clean

all: $(TEST_BIN)

$(TEST_BIN): $(TEST_SRCS) $(HEADERS)
	$(CC) $(CFLAGS) $(TEST_SRCS) -o $@

test: $(TEST_BIN)
	./$(TEST_BIN)

clean:
	rm -f $(TEST_BIN)
