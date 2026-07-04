#include "../vm.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static Type test_types[] = {
    [0] = {
        .kind = TYPE_INT,
        .name = "int",
        .payload_size = sizeof(num_t),
        .size = sizeof(num_t),
        .align = alignof(num_t),
    },
    [1] = {
        .kind = TYPE_BYTE,
        .name = "byte",
        .payload_size = 1,
        .size = 1,
        .align = 1,
    },
    [2] = {
        .kind = TYPE_ARRAY,
        .name = "int[4]",
        .data.array = {
            .elem = TYPE_INT_ID,
            .capacity = 4,
        },
    },
};

enum {
    TYPE_INT_ARRAY4_ID = 2,
};

static TypeS test_type_slice(void) {
    return (TypeS) {
        .data = test_types,
        .len = sizeof(test_types) / sizeof(test_types[0]),
    };
}

static void vm_init_for_test(VM* vm, size_t storage_cap, size_t param_cap, size_t crash_cap) {
    memset(vm, 0, sizeof(*vm));

    vm->storage.data = malloc(storage_cap);
    vm->storage.len = 0;
    vm->storage.cap = storage_cap;
    assert(vm->storage.data);

    vm->param_stack.data = malloc(param_cap * sizeof(*vm->param_stack.data));
    vm->param_stack.len = 0;
    vm->param_stack.cap = param_cap;
    assert(vm->param_stack.data);

    vm->crash_stack.data = malloc(crash_cap * sizeof(*vm->crash_stack.data));
    vm->crash_stack.len = 0;
    vm->crash_stack.cap = crash_cap;
    assert(vm->crash_stack.data);
}

static void vm_free_for_test(VM* vm) {
    free(vm->storage.data);
    free(vm->param_stack.data);
    free(vm->crash_stack.data);
    memset(vm, 0, sizeof(*vm));
}

static void push_param_or_die(VM* vm, void* ptr) {
    VM_RESULT r = vm_push_param(vm, ptr);
    assert(r == VM_OK);
}

static void run_func_or_die(Func* func, VM* vm) {
    VmCode code = vm_compile_no_defers(func);
    assert(code.data);
    assert(code.len > 0);

    VM_RESULT result = vm_run(vm, code.data);
    assert(result == VM_OK);

    free(code.data);
}

static void run_func_expect(Func* func, VM* vm, VM_RESULT expected) {
    VmCode code = vm_compile_no_defers(func);
    assert(code.data);
    assert(code.len > 0);

    VM_RESULT result = vm_run(vm, code.data);
    assert(result == expected);

    free(code.data);
}

/*
    Assumption used by these tests:

        OP_PUSH_ARG idx pushes the external pointer from param_stack[idx].

    Assignment assumption:

        push destination pointer
        push source pointer
        OP_ASSIGN

    So:

        y = x

    is encoded as:

        PUSH_ARG y
        PUSH_ARG x
        ASSIGN

    If your VM uses source-before-destination, swap the two PUSH_ARG ops in
    each assignment helper.
*/

static void test_basic_external_assign(void) {
    enum {
        ARG_Y,
        ARG_X,
        ARG_COUNT,
    };

    enum {
        BLOCK_ROOT,
        BLOCK_COUNT,
    };

    enum {
        OP_PUSH_Y,
        OP_PUSH_X,
        OP_ASSIGN_Y,
        OP_COUNT,
    };

    static Var ins[] = {
        [ARG_X] = {.tid = TYPE_INT_ID, .name = "x"},
    };

    static Var outs[] = {
        {.tid = TYPE_INT_ID, .name = "y"},
    };

    static Var vars[] = {
        [ARG_X] = {.tid = TYPE_INT_ID, .name = "x"},
        [ARG_Y] = {.tid = TYPE_INT_ID, .name = "y"},
    };

    static OP ops[] = {
        [OP_PUSH_Y]   = {.kind = OP_PUSH_ARG, .extra = ARG_Y},
        [OP_PUSH_X]   = {.kind = OP_PUSH_ARG, .extra = ARG_X},
        [OP_ASSIGN_Y] = {.kind = OP_ASSIGN,   .extra = 0},
    };

    static Block blocks[] = {
        [BLOCK_ROOT] = {
            .kind = BLOCK_BASIC,
            .data.basic = {
                .start = OP_PUSH_Y,
                .len = 3,
            },
        },
    };

    Func func = {
        .name = "test_basic_external_assign",

        .sig = {
            .ins = {
                .data = ins,
                .len = 1,
            },
            .outs = {
                .data = outs,
                .len = 1,
            },
            .can_crash = false,
        },

        .types = test_type_slice(),

        .blocks = {
            .data = blocks,
            .len = BLOCK_COUNT,
        },

        .ops = {
            .data = ops,
            .len = OP_COUNT,
        },

        .vars = {
            .data = vars,
            .len = ARG_COUNT,
        },
    };

    VM vm;
    vm_init_for_test(&vm, 1024, 8, 8);

    num_t x = 123;
    num_t y = 0;

    push_param_or_die(&vm, &y);
    push_param_or_die(&vm, &x);

    run_func_or_die(&func, &vm);

    assert(y == 123);
    assert(vm.param_stack.len == 1);

    vm_free_for_test(&vm);
}

static void test_crash_pad_writes_y1_but_not_y2_after_crash(void) {
    enum {
        ARG_Y1,
        ARG_Y2,
        ARG_ONE,
        ARG_TWO,
        ARG_COUNT,
    };

    enum {
        BLOCK_ROOT,
        BLOCK_BODY_MANY,
        BLOCK_BODY_CRASH,
        BLOCK_BODY_AFTER_CRASH,
        BLOCK_PAD,
        BLOCK_COUNT,
    };

    enum {
        OP_BODY_PUSH_Y2,
        OP_BODY_PUSH_TWO,
        OP_BODY_ASSIGN_Y2,

        OP_PAD_PUSH_Y1,
        OP_PAD_PUSH_ONE,
        OP_PAD_ASSIGN_Y1,

        OP_COUNT,
    };

    static Var ins[] = {
        [ARG_ONE] = {.tid = TYPE_INT_ID, .name = "one"},
        [ARG_TWO] = {.tid = TYPE_INT_ID, .name = "two"},
    };

    static Var outs[] = {
        {.tid = TYPE_INT_ID, .name = "y1"},
        {.tid = TYPE_INT_ID, .name = "y2"},
    };

    static Var vars[] = {
        [ARG_ONE] = {.tid = TYPE_INT_ID, .name = "one"},
        [ARG_TWO] = {.tid = TYPE_INT_ID, .name = "two"},
        [ARG_Y1]  = {.tid = TYPE_INT_ID, .name = "y1"},
        [ARG_Y2]  = {.tid = TYPE_INT_ID, .name = "y2"},
    };

    static OP ops[] = {
        /*
            This is after BLOCK_CRASH. It must never run.
            If it does run, y2 becomes 2 and the test fails.
        */
        [OP_BODY_PUSH_Y2]  = {.kind = OP_PUSH_ARG, .extra = ARG_Y2},
        [OP_BODY_PUSH_TWO] = {.kind = OP_PUSH_ARG, .extra = ARG_TWO},
        [OP_BODY_ASSIGN_Y2] = {.kind = OP_ASSIGN, .extra = 0},

        /*
            Crash pad writes y1 = one.
        */
        [OP_PAD_PUSH_Y1]   = {.kind = OP_PUSH_ARG, .extra = ARG_Y1},
        [OP_PAD_PUSH_ONE]  = {.kind = OP_PUSH_ARG, .extra = ARG_ONE},
        [OP_PAD_ASSIGN_Y1] = {.kind = OP_ASSIGN, .extra = 0},
    };

    static Block blocks[] = {
        [BLOCK_ROOT] = {
            .kind = BLOCK_CRASH_PAD,
            .data.crash_pad = {
                .body = BLOCK_BODY_MANY,
                .pad = BLOCK_PAD,
            },
        },

        [BLOCK_BODY_MANY] = {
            .kind = BLOCK_MANY,
            .data.many = {
                .start = BLOCK_BODY_CRASH,
                .len = 2,
            },
        },

        [BLOCK_BODY_CRASH] = {
            .kind = BLOCK_CRASH,
        },

        [BLOCK_BODY_AFTER_CRASH] = {
            .kind = BLOCK_BASIC,
            .data.basic = {
                .start = OP_BODY_PUSH_Y2,
                .len = 3,
            },
        },

        [BLOCK_PAD] = {
            .kind = BLOCK_BASIC,
            .data.basic = {
                .start = OP_PAD_PUSH_Y1,
                .len = 3,
            },
        },
    };

    Func func = {
        .name = "test_crash_pad_writes_y1_but_not_y2_after_crash",

        .sig = {
            .ins = {
                .data = ins,
                .len = 2,
            },
            .outs = {
                .data = outs,
                .len = 2,
            },
            .can_crash = true,
        },

        .types = test_type_slice(),

        .blocks = {
            .data = blocks,
            .len = BLOCK_COUNT,
        },

        .ops = {
            .data = ops,
            .len = OP_COUNT,
        },

        .vars = {
            .data = vars,
            .len = ARG_COUNT,
        },
    };

    VM vm;
    vm_init_for_test(&vm, 1024, 16, 16);

    num_t one = 1;
    num_t two = 2;
    num_t y1 = 0;
    num_t y2 = 0;

    push_param_or_die(&vm, &y1);
    push_param_or_die(&vm, &y2);
    push_param_or_die(&vm, &one);
    push_param_or_die(&vm, &two);

    run_func_expect(&func, &vm, VM_CRASH);

    assert(y1 == 1);
    assert(y2 == 0);
    assert(y2 != 2);
    assert(vm.param_stack.len == ARG_COUNT);

    vm_free_for_test(&vm);
}

static void test_crash_pad_body_runs_normally_without_crash(void) {
    enum {
        ARG_Y1,
        ARG_Y2,
        ARG_ONE,
        ARG_TWO,
        ARG_COUNT,
    };

    enum {
        BLOCK_ROOT,
        BLOCK_BODY,
        BLOCK_PAD,
        BLOCK_COUNT,
    };

    enum {
        OP_BODY_PUSH_Y2,
        OP_BODY_PUSH_TWO,
        OP_BODY_ASSIGN_Y2,

        OP_PAD_PUSH_Y1,
        OP_PAD_PUSH_ONE,
        OP_PAD_ASSIGN_Y1,

        OP_COUNT,
    };

    static Var ins[] = {
        [ARG_ONE] = {.tid = TYPE_INT_ID, .name = "one"},
        [ARG_TWO] = {.tid = TYPE_INT_ID, .name = "two"},
    };

    static Var outs[] = {
        {.tid = TYPE_INT_ID, .name = "y1"},
        {.tid = TYPE_INT_ID, .name = "y2"},
    };

    static Var vars[] = {
        [ARG_ONE] = {.tid = TYPE_INT_ID, .name = "one"},
        [ARG_TWO] = {.tid = TYPE_INT_ID, .name = "two"},
        [ARG_Y1]  = {.tid = TYPE_INT_ID, .name = "y1"},
        [ARG_Y2]  = {.tid = TYPE_INT_ID, .name = "y2"},
    };

    static OP ops[] = {
        [OP_BODY_PUSH_Y2]   = {.kind = OP_PUSH_ARG, .extra = ARG_Y2},
        [OP_BODY_PUSH_TWO]  = {.kind = OP_PUSH_ARG, .extra = ARG_TWO},
        [OP_BODY_ASSIGN_Y2] = {.kind = OP_ASSIGN,   .extra = 0},

        /*
            This pad must NOT run because the body does not crash.
            If it runs, y1 becomes 1 and the test fails.
        */
        [OP_PAD_PUSH_Y1]    = {.kind = OP_PUSH_ARG, .extra = ARG_Y1},
        [OP_PAD_PUSH_ONE]   = {.kind = OP_PUSH_ARG, .extra = ARG_ONE},
        [OP_PAD_ASSIGN_Y1]  = {.kind = OP_ASSIGN,   .extra = 0},
    };

    static Block blocks[] = {
        [BLOCK_ROOT] = {
            .kind = BLOCK_CRASH_PAD,
            .data.crash_pad = {
                .body = BLOCK_BODY,
                .pad = BLOCK_PAD,
            },
        },

        [BLOCK_BODY] = {
            .kind = BLOCK_BASIC,
            .data.basic = {
                .start = OP_BODY_PUSH_Y2,
                .len = 3,
            },
        },

        [BLOCK_PAD] = {
            .kind = BLOCK_BASIC,
            .data.basic = {
                .start = OP_PAD_PUSH_Y1,
                .len = 3,
            },
        },
    };

    Func func = {
        .name = "test_crash_pad_body_runs_normally_without_crash",

        .sig = {
            .ins = {
                .data = ins,
                .len = 2,
            },
            .outs = {
                .data = outs,
                .len = 2,
            },
            .can_crash = true,
        },

        .types = test_type_slice(),

        .blocks = {
            .data = blocks,
            .len = BLOCK_COUNT,
        },

        .ops = {
            .data = ops,
            .len = OP_COUNT,
        },

        .vars = {
            .data = vars,
            .len = ARG_COUNT,
        },
    };

    VM vm;
    vm_init_for_test(&vm, 1024, 16, 16);

    num_t one = 1;
    num_t two = 2;
    num_t y1 = 0;
    num_t y2 = 0;

    push_param_or_die(&vm, &y1);
    push_param_or_die(&vm, &y2);
    push_param_or_die(&vm, &one);
    push_param_or_die(&vm, &two);

    run_func_or_die(&func, &vm);

    assert(y1 == 0);
    assert(y2 == 2);
    assert(vm.param_stack.len == 2);

    vm_free_for_test(&vm);
}

static void test_uncaught_crash_returns_vm_crash(void) {
    enum {
        BLOCK_ROOT,
        BLOCK_COUNT,
    };

    static Block blocks[] = {
        [BLOCK_ROOT] = {
            .kind = BLOCK_CRASH,
        },
    };

    Func func = {
        .name = "test_uncaught_crash_returns_vm_crash",

        .sig = {
            .ins = {
                .data = NULL,
                .len = 0,
            },
            .outs = {
                .data = NULL,
                .len = 0,
            },
            .can_crash = true,
        },

        .types = test_type_slice(),

        .blocks = {
            .data = blocks,
            .len = BLOCK_COUNT,
        },

        .ops = {
            .data = NULL,
            .len = 0,
        },

        .vars = {
            .data = NULL,
            .len = 0,
        },
    };

    VmCode code = vm_compile_no_defers(&func);
    assert(code.data);
    assert(code.len > 0);

    VM vm;
    vm_init_for_test(&vm, 1024, 8, 8);

    VM_RESULT result = vm_run(&vm, code.data);
    assert(result == VM_CRASH);

    vm_free_for_test(&vm);
    free(code.data);
}

static void test_push_param_oom(void) {
    VM vm;
    vm_init_for_test(&vm, 1024, 1, 8);

    num_t a = 1;
    num_t b = 2;

    assert(vm_push_param(&vm, &a) == VM_OK);
    assert(vm_push_param(&vm, &b) == VM_OOM_PARAM);

    vm_free_for_test(&vm);
}

static void test_array_push_at_and_drop(void) {
    enum {
        ARG_Y0,
        ARG_Y1,
        ARG_ARR,
        ARG_ONE,
        ARG_TWO,
        ARG_IDX0,
        ARG_IDX1,
        ARG_COUNT,
    };

    enum {
        BLOCK_ROOT,
        BLOCK_COUNT,
    };

    enum {
        OP_PUSH_ARR_FOR_ONE,
        OP_PUSH_ONE,
        OP_PUSH_ONE_TO_ARR,

        OP_PUSH_ARR_FOR_TWO,
        OP_PUSH_TWO,
        OP_PUSH_TWO_TO_ARR,

        OP_PUSH_Y0,
        OP_PUSH_ARR_FOR_Y0,
        OP_PUSH_IDX0,
        OP_AT_Y0,
        OP_ASSIGN_Y0,

        OP_PUSH_Y1,
        OP_PUSH_ARR_FOR_Y1,
        OP_PUSH_IDX1,
        OP_AT_Y1,
        OP_ASSIGN_Y1,

        OP_PUSH_ARR_FOR_DROP,
        OP_DROP_LAST,

        OP_COUNT,
    };

    static Var ins[] = {
        [ARG_ARR]  = {.tid = TYPE_INT_ARRAY4_ID, .name = "arr"},
        [ARG_ONE]  = {.tid = TYPE_INT_ID,        .name = "one"},
        [ARG_TWO]  = {.tid = TYPE_INT_ID,        .name = "two"},
        [ARG_IDX0] = {.tid = TYPE_INT_ID,        .name = "idx0"},
        [ARG_IDX1] = {.tid = TYPE_INT_ID,        .name = "idx1"},
    };

    static Var outs[] = {
        {.tid = TYPE_INT_ID, .name = "y0"},
        {.tid = TYPE_INT_ID, .name = "y1"},
    };

    static Var vars[] = {
        [ARG_ARR]  = {.tid = TYPE_INT_ARRAY4_ID, .name = "arr"},
        [ARG_ONE]  = {.tid = TYPE_INT_ID,        .name = "one"},
        [ARG_TWO]  = {.tid = TYPE_INT_ID,        .name = "two"},
        [ARG_IDX0] = {.tid = TYPE_INT_ID,        .name = "idx0"},
        [ARG_IDX1] = {.tid = TYPE_INT_ID,        .name = "idx1"},
        [ARG_Y0]   = {.tid = TYPE_INT_ID,        .name = "y0"},
        [ARG_Y1]   = {.tid = TYPE_INT_ID,        .name = "y1"},
    };

    static OP ops[] = {
        [OP_PUSH_ARR_FOR_ONE]  = {.kind = OP_PUSH_ARG, .extra = ARG_ARR},
        [OP_PUSH_ONE]          = {.kind = OP_PUSH_ARG, .extra = ARG_ONE},
        [OP_PUSH_ONE_TO_ARR]   = {.kind = OP_ARR_PUSH, .extra = 0},

        [OP_PUSH_ARR_FOR_TWO]  = {.kind = OP_PUSH_ARG, .extra = ARG_ARR},
        [OP_PUSH_TWO]          = {.kind = OP_PUSH_ARG, .extra = ARG_TWO},
        [OP_PUSH_TWO_TO_ARR]   = {.kind = OP_ARR_PUSH, .extra = 0},

        [OP_PUSH_Y0]           = {.kind = OP_PUSH_ARG, .extra = ARG_Y0},
        [OP_PUSH_ARR_FOR_Y0]   = {.kind = OP_PUSH_ARG, .extra = ARG_ARR},
        [OP_PUSH_IDX0]         = {.kind = OP_PUSH_ARG, .extra = ARG_IDX0},
        [OP_AT_Y0]             = {.kind = OP_ARR_AT,   .extra = 0},
        [OP_ASSIGN_Y0]         = {.kind = OP_ASSIGN,   .extra = 0},

        [OP_PUSH_Y1]           = {.kind = OP_PUSH_ARG, .extra = ARG_Y1},
        [OP_PUSH_ARR_FOR_Y1]   = {.kind = OP_PUSH_ARG, .extra = ARG_ARR},
        [OP_PUSH_IDX1]         = {.kind = OP_PUSH_ARG, .extra = ARG_IDX1},
        [OP_AT_Y1]             = {.kind = OP_ARR_AT,   .extra = 0},
        [OP_ASSIGN_Y1]         = {.kind = OP_ASSIGN,   .extra = 0},

        [OP_PUSH_ARR_FOR_DROP] = {.kind = OP_PUSH_ARG, .extra = ARG_ARR},
        [OP_DROP_LAST]         = {.kind = OP_ARR_DROP, .extra = 0},
    };

    static Block blocks[] = {
        [BLOCK_ROOT] = {
            .kind = BLOCK_BASIC,
            .data.basic = {
                .start = OP_PUSH_ARR_FOR_ONE,
                .len = OP_COUNT,
            },
        },
    };

    Func func = {
        .name = "test_array_push_at_and_drop",
        .sig = {
            .ins = {.data = ins, .len = 5},
            .outs = {.data = outs, .len = 2},
            .can_crash = false,
        },
        .types = test_type_slice(),
        .blocks = {.data = blocks, .len = BLOCK_COUNT},
        .ops = {.data = ops, .len = OP_COUNT},
        .vars = {.data = vars, .len = ARG_COUNT},
    };

    VM vm;
    vm_init_for_test(&vm, 1024, 32, 8);

    alignas(Cell) unsigned char arr[128] = {0};
    num_t one = 11;
    num_t two = 22;
    num_t idx0 = 0;
    num_t idx1 = 1;
    num_t y0 = 0;
    num_t y1 = 0;

    push_param_or_die(&vm, &y0);
    push_param_or_die(&vm, &y1);
    push_param_or_die(&vm, arr);
    push_param_or_die(&vm, &one);
    push_param_or_die(&vm, &two);
    push_param_or_die(&vm, &idx0);
    push_param_or_die(&vm, &idx1);

    run_func_or_die(&func, &vm);

    count_t len;
    memcpy(&len, arr, sizeof(len));

    num_t first;
    size_t data_offset = test_types[TYPE_INT_ARRAY4_ID].data.array.data_offset;
    memcpy(&first, arr + data_offset, sizeof(first));

    assert(y0 == 11);
    assert(y1 == 22);
    assert(len == 1);
    assert(first == 11);
    assert(vm.param_stack.len == 2);

    vm_free_for_test(&vm);
}

static void test_loop_break_skips_unreachable_body_tail(void) {
    enum {
        ARG_Y,
        ARG_AFTER,
        ARG_ONE,
        ARG_TWO,
        ARG_BAD,
        VAR_COND,
        ARG_COUNT,
    };

    enum {
        BLOCK_ROOT,
        BLOCK_ROOT_MANY,
        BLOCK_INIT_COND,
        BLOCK_LOOP_UNTIL_BREAK,
        BLOCK_ASSIGN_AFTER_TWO,
        BLOCK_LOOP_BODY_MANY,
        BLOCK_ASSIGN_Y_ONE,
        BLOCK_BREAK_LOOP,
        BLOCK_ASSIGN_Y_BAD,
        BLOCK_COUNT,
    };

    enum {
        OP_INIT_PUSH_COND,
        OP_INIT_PUSH_ONE,
        OP_INIT_ASSIGN_COND,

        OP_Y_PUSH_Y,
        OP_Y_PUSH_ONE,
        OP_Y_ASSIGN,

        OP_BAD_PUSH_Y,
        OP_BAD_PUSH_BAD,
        OP_BAD_ASSIGN,

        OP_AFTER_PUSH_AFTER,
        OP_AFTER_PUSH_TWO,
        OP_AFTER_ASSIGN,

        OP_COUNT,
    };

    static Var ins[] = {
        [ARG_ONE] = {.tid = TYPE_INT_ID, .name = "one"},
        [ARG_TWO] = {.tid = TYPE_INT_ID, .name = "two"},
        [ARG_BAD] = {.tid = TYPE_INT_ID, .name = "bad"},
    };

    static Var outs[] = {
        {.tid = TYPE_INT_ID, .name = "y"},
        {.tid = TYPE_INT_ID, .name = "after"},
    };

    static Var vars[] = {
        [ARG_ONE]   = {.tid = TYPE_INT_ID, .name = "one"},
        [ARG_TWO]   = {.tid = TYPE_INT_ID, .name = "two"},
        [ARG_BAD]   = {.tid = TYPE_INT_ID, .name = "bad"},
        [ARG_Y]     = {.tid = TYPE_INT_ID, .name = "y"},
        [ARG_AFTER] = {.tid = TYPE_INT_ID, .name = "after"},
        [VAR_COND]  = {.tid = TYPE_INT_ID, .name = "cond"},
    };

    static OP ops[] = {
        [OP_INIT_PUSH_COND]  = {.kind = OP_PUSH_VAR, .extra = VAR_COND},
        [OP_INIT_PUSH_ONE]   = {.kind = OP_PUSH_ARG, .extra = ARG_ONE},
        [OP_INIT_ASSIGN_COND] = {.kind = OP_ASSIGN,  .extra = 0},

        [OP_Y_PUSH_Y]        = {.kind = OP_PUSH_ARG, .extra = ARG_Y},
        [OP_Y_PUSH_ONE]      = {.kind = OP_PUSH_ARG, .extra = ARG_ONE},
        [OP_Y_ASSIGN]        = {.kind = OP_ASSIGN,   .extra = 0},

        [OP_BAD_PUSH_Y]      = {.kind = OP_PUSH_ARG, .extra = ARG_Y},
        [OP_BAD_PUSH_BAD]    = {.kind = OP_PUSH_ARG, .extra = ARG_BAD},
        [OP_BAD_ASSIGN]      = {.kind = OP_ASSIGN,   .extra = 0},

        [OP_AFTER_PUSH_AFTER] = {.kind = OP_PUSH_ARG, .extra = ARG_AFTER},
        [OP_AFTER_PUSH_TWO]   = {.kind = OP_PUSH_ARG, .extra = ARG_TWO},
        [OP_AFTER_ASSIGN]     = {.kind = OP_ASSIGN,   .extra = 0},
    };

    static Block blocks[] = {
        [BLOCK_ROOT] = {
            .kind = BLOCK_VAR,
            .data.var = {.var = VAR_COND, .body = BLOCK_ROOT_MANY},
        },
        [BLOCK_ROOT_MANY] = {
            .kind = BLOCK_MANY,
            .data.many = {.start = BLOCK_INIT_COND, .len = 3},
        },
        [BLOCK_INIT_COND] = {
            .kind = BLOCK_BASIC,
            .data.basic = {.start = OP_INIT_PUSH_COND, .len = 3},
        },
        [BLOCK_LOOP_UNTIL_BREAK] = {
            .kind = BLOCK_LOOP,
            .data.loop = {.cond = VAR_COND, .body = BLOCK_LOOP_BODY_MANY},
        },
        [BLOCK_LOOP_BODY_MANY] = {
            .kind = BLOCK_MANY,
            .data.many = {.start = BLOCK_ASSIGN_Y_ONE, .len = 3},
        },
        [BLOCK_ASSIGN_Y_ONE] = {
            .kind = BLOCK_BASIC,
            .data.basic = {.start = OP_Y_PUSH_Y, .len = 3},
        },
        [BLOCK_BREAK_LOOP] = {
            .kind = BLOCK_BREAK,
            .data.level = 2,
        },
        [BLOCK_ASSIGN_Y_BAD] = {
            .kind = BLOCK_BASIC,
            .data.basic = {.start = OP_BAD_PUSH_Y, .len = 3},
        },
        [BLOCK_ASSIGN_AFTER_TWO] = {
            .kind = BLOCK_BASIC,
            .data.basic = {.start = OP_AFTER_PUSH_AFTER, .len = 3},
        },
    };

    Func func = {
        .name = "test_loop_break_skips_unreachable_body_tail",
        .sig = {
            .ins = {.data = ins, .len = 3},
            .outs = {.data = outs, .len = 2},
            .can_crash = false,
        },
        .types = test_type_slice(),
        .blocks = {.data = blocks, .len = BLOCK_COUNT},
        .ops = {.data = ops, .len = OP_COUNT},
        .vars = {.data = vars, .len = ARG_COUNT},
    };

    VM vm;
    vm_init_for_test(&vm, 1024, 32, 8);

    num_t one = 1;
    num_t two = 2;
    num_t bad = 99;
    num_t y = 0;
    num_t after = 0;

    push_param_or_die(&vm, &y);
    push_param_or_die(&vm, &after);
    push_param_or_die(&vm, &one);
    push_param_or_die(&vm, &two);
    push_param_or_die(&vm, &bad);

    run_func_or_die(&func, &vm);

    assert(y == 1);
    assert(after == 2);

    vm_free_for_test(&vm);
}

static void test_nested_many_break_skips_outer_tail(void) {
    enum {
        ARG_Y,
        ARG_ONE,
        ARG_TWO,
        ARG_THREE,
        ARG_BAD,
        ARG_COUNT,
    };

    enum {
        BLOCK_ROOT,
        BLOCK_ASSIGN_ONE,
        BLOCK_OUTER_MANY,
        BLOCK_ASSIGN_THREE,
        BLOCK_INNER_MANY,
        BLOCK_ASSIGN_BAD,
        BLOCK_ASSIGN_TWO,
        BLOCK_BREAK_OUTER,
        BLOCK_COUNT,
    };

    enum {
        OP_ONE_PUSH_Y,
        OP_ONE_PUSH_ONE,
        OP_ONE_ASSIGN,

        OP_TWO_PUSH_Y,
        OP_TWO_PUSH_TWO,
        OP_TWO_ASSIGN,

        OP_BAD_PUSH_Y,
        OP_BAD_PUSH_BAD,
        OP_BAD_ASSIGN,

        OP_THREE_PUSH_Y,
        OP_THREE_PUSH_THREE,
        OP_THREE_ASSIGN,

        OP_COUNT,
    };

    static Var ins[] = {
        [ARG_ONE]   = {.tid = TYPE_INT_ID, .name = "one"},
        [ARG_TWO]   = {.tid = TYPE_INT_ID, .name = "two"},
        [ARG_THREE] = {.tid = TYPE_INT_ID, .name = "three"},
        [ARG_BAD]   = {.tid = TYPE_INT_ID, .name = "bad"},
    };

    static Var outs[] = {
        {.tid = TYPE_INT_ID, .name = "y"},
    };

    static Var vars[] = {
        [ARG_ONE]   = {.tid = TYPE_INT_ID, .name = "one"},
        [ARG_TWO]   = {.tid = TYPE_INT_ID, .name = "two"},
        [ARG_THREE] = {.tid = TYPE_INT_ID, .name = "three"},
        [ARG_BAD]   = {.tid = TYPE_INT_ID, .name = "bad"},
        [ARG_Y]     = {.tid = TYPE_INT_ID, .name = "y"},
    };

    static OP ops[] = {
        [OP_ONE_PUSH_Y]      = {.kind = OP_PUSH_ARG, .extra = ARG_Y},
        [OP_ONE_PUSH_ONE]    = {.kind = OP_PUSH_ARG, .extra = ARG_ONE},
        [OP_ONE_ASSIGN]      = {.kind = OP_ASSIGN,   .extra = 0},

        [OP_TWO_PUSH_Y]      = {.kind = OP_PUSH_ARG, .extra = ARG_Y},
        [OP_TWO_PUSH_TWO]    = {.kind = OP_PUSH_ARG, .extra = ARG_TWO},
        [OP_TWO_ASSIGN]      = {.kind = OP_ASSIGN,   .extra = 0},

        [OP_BAD_PUSH_Y]      = {.kind = OP_PUSH_ARG, .extra = ARG_Y},
        [OP_BAD_PUSH_BAD]    = {.kind = OP_PUSH_ARG, .extra = ARG_BAD},
        [OP_BAD_ASSIGN]      = {.kind = OP_ASSIGN,   .extra = 0},

        [OP_THREE_PUSH_Y]    = {.kind = OP_PUSH_ARG, .extra = ARG_Y},
        [OP_THREE_PUSH_THREE] = {.kind = OP_PUSH_ARG, .extra = ARG_THREE},
        [OP_THREE_ASSIGN]    = {.kind = OP_ASSIGN,   .extra = 0},
    };

    static Block blocks[] = {
        [BLOCK_ROOT] = {
            .kind = BLOCK_MANY,
            .data.many = {.start = BLOCK_ASSIGN_ONE, .len = 3},
        },
        [BLOCK_ASSIGN_ONE] = {
            .kind = BLOCK_BASIC,
            .data.basic = {.start = OP_ONE_PUSH_Y, .len = 3},
        },
        [BLOCK_OUTER_MANY] = {
            .kind = BLOCK_MANY,
            .data.many = {.start = BLOCK_INNER_MANY, .len = 2},
        },
        [BLOCK_INNER_MANY] = {
            .kind = BLOCK_MANY,
            .data.many = {.start = BLOCK_ASSIGN_TWO, .len = 2},
        },
        [BLOCK_ASSIGN_TWO] = {
            .kind = BLOCK_BASIC,
            .data.basic = {.start = OP_TWO_PUSH_Y, .len = 3},
        },
        [BLOCK_BREAK_OUTER] = {
            .kind = BLOCK_BREAK,
            .data.level = 2,
        },
        [BLOCK_ASSIGN_BAD] = {
            .kind = BLOCK_BASIC,
            .data.basic = {.start = OP_BAD_PUSH_Y, .len = 3},
        },
        [BLOCK_ASSIGN_THREE] = {
            .kind = BLOCK_BASIC,
            .data.basic = {.start = OP_THREE_PUSH_Y, .len = 3},
        },
    };

    Func func = {
        .name = "test_nested_many_break_skips_outer_tail",
        .sig = {
            .ins = {.data = ins, .len = 4},
            .outs = {.data = outs, .len = 1},
            .can_crash = false,
        },
        .types = test_type_slice(),
        .blocks = {.data = blocks, .len = BLOCK_COUNT},
        .ops = {.data = ops, .len = OP_COUNT},
        .vars = {.data = vars, .len = ARG_COUNT},
    };

    VM vm;
    vm_init_for_test(&vm, 1024, 32, 8);

    num_t one = 1;
    num_t two = 2;
    num_t three = 3;
    num_t bad = 99;
    num_t y = 0;

    push_param_or_die(&vm, &y);
    push_param_or_die(&vm, &one);
    push_param_or_die(&vm, &two);
    push_param_or_die(&vm, &three);
    push_param_or_die(&vm, &bad);

    run_func_or_die(&func, &vm);

    assert(y == 3);

    vm_free_for_test(&vm);
}

int main(void) {
    test_push_param_oom();
    puts("ok: test_push_param_oom");

    test_basic_external_assign();
    puts("ok: test_basic_external_assign");

    test_crash_pad_writes_y1_but_not_y2_after_crash();
    puts("ok: test_crash_pad_writes_y1_but_not_y2_after_crash");

    test_crash_pad_body_runs_normally_without_crash();
    puts("ok: test_crash_pad_body_runs_normally_without_crash");

    test_uncaught_crash_returns_vm_crash();
    puts("ok: test_uncaught_crash_returns_vm_crash");

    test_array_push_at_and_drop();
    puts("ok: test_array_push_at_and_drop");

    test_loop_break_skips_unreachable_body_tail();
    puts("ok: test_loop_break_skips_unreachable_body_tail");

    test_nested_many_break_skips_outer_tail();
    puts("ok: test_nested_many_break_skips_outer_tail");

    puts("all vm/ir tests passed");
    return 0;
}
