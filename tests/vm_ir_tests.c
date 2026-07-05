#include "../vm.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static TypeField test_pair_fields[] = {
    [0] = {.name = "first",  .tid = TYPE_INT_ID},
    [1] = {.name = "second", .tid = TYPE_INT_ID},
};

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
    [3] = {
        .kind = TYPE_NATIVE_FUNC_POINTER,
        .name = "native_fn",
        .payload_size = sizeof(VmNativeFunc),
        .size = sizeof(VmNativeFunc),
        .align = alignof(VmNativeFunc),
    },
    [4] = {
        .kind = TYPE_SLICE,
        .name = "Slice[int]",
        .data.ref = {.elem = TYPE_INT_ID},
    },
    [5] = {
        .kind = TYPE_VIEW,
        .name = "View[int]",
        .data.ref = {.elem = TYPE_INT_ID},
    },
    [6] = {
        .kind = TYPE_STRUCT,
        .name = "Pair",
        .data.fields = {.data = test_pair_fields, .len = 2},
    },
};

enum {
    TYPE_INT_ARRAY4_ID = 2,
    TYPE_NATIVE_FUNC_POINTER_ID = 3,
    TYPE_INT_SLICE_ID = 4,
    TYPE_INT_VIEW_ID = 5,
    TYPE_PAIR_ID = 6,
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
    VmCode code = vm_compile_no_defers(func, NULL);
    assert(code.data);
    assert(code.len > 0);

    VM_RESULT result = vm_run(vm, code.data);
    assert(result == VM_OK);

    free(code.data);
}

static void run_func_expect(Func* func, VM* vm, VM_RESULT expected) {
    VmCode code = vm_compile_no_defers(func, NULL);
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

    static SigInput ins[] = {
        [ARG_X] = {.var = {.tid = TYPE_INT_ID, .name = "x"}, .mut = true},
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

    assert(func.sig.ins.data[ARG_X].mut);

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

    static SigInput ins[] = {
        [ARG_ONE] = {.var = {.tid = TYPE_INT_ID, .name = "one"}},
        [ARG_TWO] = {.var = {.tid = TYPE_INT_ID, .name = "two"}},
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

    static SigInput ins[] = {
        [ARG_ONE] = {.var = {.tid = TYPE_INT_ID, .name = "one"}},
        [ARG_TWO] = {.var = {.tid = TYPE_INT_ID, .name = "two"}},
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

    VmCode code = vm_compile_no_defers(&func, NULL);
    assert(code.data);
    assert(code.len > 0);

    VM vm;
    vm_init_for_test(&vm, 1024, 8, 8);

    VM_RESULT result = vm_run(&vm, code.data);
    assert(result == VM_CRASH);

    vm_free_for_test(&vm);
    free(code.data);
}

static void test_hard_crash_returns_vm_hard_crash(void) {
    enum {
        BLOCK_ROOT,
        BLOCK_COUNT,
    };

    static Block blocks[] = {
        [BLOCK_ROOT] = {
            .kind = BLOCK_HARD_CRASH,
        },
    };

    Func func = {
        .name = "test_hard_crash_returns_vm_hard_crash",
        .sig = {
            .ins = {.data = NULL, .len = 0},
            .outs = {.data = NULL, .len = 0},
            
        },
        .types = test_type_slice(),
        .blocks = {.data = blocks, .len = BLOCK_COUNT},
        .ops = {.data = NULL, .len = 0},
        .vars = {.data = NULL, .len = 0},
    };

    VM vm;
    vm_init_for_test(&vm, 1024, 8, 8);

    run_func_expect(&func, &vm, VM_HARD_CRASH);

    vm_free_for_test(&vm);
}

static void test_array_crashes_unwind_stack(void) {
    enum {
        ARG_Y,
        ARG_ARR,
        ARG_IDX,
        ARG_MARK,
        ARG_COUNT,
    };

    enum {
        BOUNDS_BLOCK_ROOT,
        BOUNDS_BLOCK_BODY,
        BOUNDS_BLOCK_PAD,
        BOUNDS_BLOCK_COUNT,
    };

    enum {
        BOUNDS_OP_PUSH_ARR,
        BOUNDS_OP_PUSH_IDX,
        BOUNDS_OP_AT,
        BOUNDS_OP_PUSH_Y,
        BOUNDS_OP_PUSH_MARK,
        BOUNDS_OP_ASSIGN_Y,
        BOUNDS_OP_COUNT,
    };

    static SigInput ins[] = {
        [ARG_ARR]  = {.var = {.tid = TYPE_INT_ARRAY4_ID, .name = "arr"}},
        [ARG_IDX]  = {.var = {.tid = TYPE_INT_ID,        .name = "idx"}},
        [ARG_MARK] = {.var = {.tid = TYPE_INT_ID,        .name = "mark"}},
    };

    static Var outs[] = {
        {.tid = TYPE_INT_ID, .name = "y"},
    };

    static Var vars[] = {
        [ARG_Y]    = {.tid = TYPE_INT_ID,        .name = "y"},
        [ARG_ARR]  = {.tid = TYPE_INT_ARRAY4_ID, .name = "arr"},
        [ARG_IDX]  = {.tid = TYPE_INT_ID,        .name = "idx"},
        [ARG_MARK] = {.tid = TYPE_INT_ID,        .name = "mark"},
    };

    static OP bounds_ops[] = {
        [BOUNDS_OP_PUSH_ARR]  = {.kind = OP_PUSH_ARG, .extra = ARG_ARR},
        [BOUNDS_OP_PUSH_IDX]  = {.kind = OP_PUSH_ARG, .extra = ARG_IDX},
        [BOUNDS_OP_AT]        = {.kind = OP_ARR_AT,   .extra = 0},
        [BOUNDS_OP_PUSH_Y]    = {.kind = OP_PUSH_ARG, .extra = ARG_Y},
        [BOUNDS_OP_PUSH_MARK] = {.kind = OP_PUSH_ARG, .extra = ARG_MARK},
        [BOUNDS_OP_ASSIGN_Y]  = {.kind = OP_ASSIGN,   .extra = 0},
    };

    static Block bounds_blocks[] = {
        [BOUNDS_BLOCK_ROOT] = {
            .kind = BLOCK_CRASH_PAD,
            .data.crash_pad = {.body = BOUNDS_BLOCK_BODY, .pad = BOUNDS_BLOCK_PAD},
        },
        [BOUNDS_BLOCK_BODY] = {
            .kind = BLOCK_BASIC,
            .data.basic = {.start = BOUNDS_OP_PUSH_ARR, .len = 3},
        },
        [BOUNDS_BLOCK_PAD] = {
            .kind = BLOCK_BASIC,
            .data.basic = {.start = BOUNDS_OP_PUSH_Y, .len = 3},
        },
    };

    Func bounds_func = {
        .name = "test_array_bad_bounds_unwinds_stack",
        .sig = {
            .ins = {.data = ins, .len = 3},
            .outs = {.data = outs, .len = 1},
            
        },
        .types = test_type_slice(),
        .blocks = {.data = bounds_blocks, .len = BOUNDS_BLOCK_COUNT},
        .ops = {.data = bounds_ops, .len = BOUNDS_OP_COUNT},
        .vars = {.data = vars, .len = ARG_COUNT},
    };

    VM bounds_vm;
    vm_init_for_test(&bounds_vm, 1024, 16, 8);

    alignas(Cell) unsigned char bounds_arr[128] = {0};
    num_t bounds_y = 0;
    num_t bad_idx = 0;
    num_t mark = 77;

    push_param_or_die(&bounds_vm, &bounds_y);
    push_param_or_die(&bounds_vm, bounds_arr);
    push_param_or_die(&bounds_vm, &bad_idx);
    push_param_or_die(&bounds_vm, &mark);

    run_func_expect(&bounds_func, &bounds_vm, VM_ARRAY_BOUNDS);

    assert(bounds_y == 77);
    assert(bounds_vm.param_stack.len == ARG_COUNT);

    vm_free_for_test(&bounds_vm);

    enum {
        CAP_BLOCK_ROOT,
        CAP_BLOCK_BODY,
        CAP_BLOCK_PAD,
        CAP_BLOCK_COUNT,
    };

    enum {
        CAP_OP_PUSH_ARR,
        CAP_OP_PUSH_MARK_FOR_ARR,
        CAP_OP_ARR_PUSH,
        CAP_OP_PUSH_Y,
        CAP_OP_PUSH_MARK,
        CAP_OP_ASSIGN_Y,
        CAP_OP_COUNT,
    };

    static OP cap_ops[] = {
        [CAP_OP_PUSH_ARR]          = {.kind = OP_PUSH_ARG, .extra = ARG_ARR},
        [CAP_OP_PUSH_MARK_FOR_ARR] = {.kind = OP_PUSH_ARG, .extra = ARG_MARK},
        [CAP_OP_ARR_PUSH]          = {.kind = OP_ARR_PUSH, .extra = 0},
        [CAP_OP_PUSH_Y]            = {.kind = OP_PUSH_ARG, .extra = ARG_Y},
        [CAP_OP_PUSH_MARK]         = {.kind = OP_PUSH_ARG, .extra = ARG_MARK},
        [CAP_OP_ASSIGN_Y]          = {.kind = OP_ASSIGN,   .extra = 0},
    };

    static Block cap_blocks[] = {
        [CAP_BLOCK_ROOT] = {
            .kind = BLOCK_CRASH_PAD,
            .data.crash_pad = {.body = CAP_BLOCK_BODY, .pad = CAP_BLOCK_PAD},
        },
        [CAP_BLOCK_BODY] = {
            .kind = BLOCK_BASIC,
            .data.basic = {.start = CAP_OP_PUSH_ARR, .len = 3},
        },
        [CAP_BLOCK_PAD] = {
            .kind = BLOCK_BASIC,
            .data.basic = {.start = CAP_OP_PUSH_Y, .len = 3},
        },
    };

    Func cap_func = {
        .name = "test_array_out_of_capacity_unwinds_stack",
        .sig = {
            .ins = {.data = ins, .len = 3},
            .outs = {.data = outs, .len = 1},
            
        },
        .types = test_type_slice(),
        .blocks = {.data = cap_blocks, .len = CAP_BLOCK_COUNT},
        .ops = {.data = cap_ops, .len = CAP_OP_COUNT},
        .vars = {.data = vars, .len = ARG_COUNT},
    };

    VM cap_vm;
    vm_init_for_test(&cap_vm, 1024, 16, 8);

    alignas(Cell) unsigned char cap_arr[128] = {0};
    count_t len = test_types[TYPE_INT_ARRAY4_ID].data.array.capacity;
    memcpy(cap_arr, &len, sizeof(len));

    num_t cap_y = 0;

    push_param_or_die(&cap_vm, &cap_y);
    push_param_or_die(&cap_vm, cap_arr);
    push_param_or_die(&cap_vm, &bad_idx);
    push_param_or_die(&cap_vm, &mark);

    run_func_expect(&cap_func, &cap_vm, VM_ARRAY_CAPACITY);

    count_t actual_len;
    memcpy(&actual_len, cap_arr, sizeof(actual_len));
    assert(cap_y == 77);
    assert(actual_len == test_types[TYPE_INT_ARRAY4_ID].data.array.capacity);
    assert(cap_vm.param_stack.len == ARG_COUNT);

    vm_free_for_test(&cap_vm);

    enum {
        DROP_BLOCK_ROOT,
        DROP_BLOCK_BODY,
        DROP_BLOCK_PAD,
        DROP_BLOCK_COUNT,
    };

    enum {
        DROP_OP_PUSH_ARR,
        DROP_OP_PUSH_COUNT,
        DROP_OP_ARR_DROP,
        DROP_OP_PUSH_Y,
        DROP_OP_PUSH_MARK,
        DROP_OP_ASSIGN_Y,
        DROP_OP_COUNT,
    };

    static OP drop_ops[] = {
        [DROP_OP_PUSH_ARR]   = {.kind = OP_PUSH_ARG, .extra = ARG_ARR},
        [DROP_OP_PUSH_COUNT] = {.kind = OP_PUSH_ARG, .extra = ARG_IDX},
        [DROP_OP_ARR_DROP]   = {.kind = OP_ARR_DROP, .extra = 0},
        [DROP_OP_PUSH_Y]     = {.kind = OP_PUSH_ARG, .extra = ARG_Y},
        [DROP_OP_PUSH_MARK]  = {.kind = OP_PUSH_ARG, .extra = ARG_MARK},
        [DROP_OP_ASSIGN_Y]   = {.kind = OP_ASSIGN,   .extra = 0},
    };

    static Block drop_blocks[] = {
        [DROP_BLOCK_ROOT] = {
            .kind = BLOCK_CRASH_PAD,
            .data.crash_pad = {.body = DROP_BLOCK_BODY, .pad = DROP_BLOCK_PAD},
        },
        [DROP_BLOCK_BODY] = {
            .kind = BLOCK_BASIC,
            .data.basic = {.start = DROP_OP_PUSH_ARR, .len = 3},
        },
        [DROP_BLOCK_PAD] = {
            .kind = BLOCK_BASIC,
            .data.basic = {.start = DROP_OP_PUSH_Y, .len = 3},
        },
    };

    Func drop_func = {
        .name = "test_array_drop_underflow_unwinds_stack",
        .sig = {
            .ins = {.data = ins, .len = 3},
            .outs = {.data = outs, .len = 1},
            
        },
        .types = test_type_slice(),
        .blocks = {.data = drop_blocks, .len = DROP_BLOCK_COUNT},
        .ops = {.data = drop_ops, .len = DROP_OP_COUNT},
        .vars = {.data = vars, .len = ARG_COUNT},
    };

    VM drop_vm;
    vm_init_for_test(&drop_vm, 1024, 16, 8);

    alignas(Cell) unsigned char drop_arr[128] = {0};
    count_t drop_len = 1;
    memcpy(drop_arr, &drop_len, sizeof(drop_len));

    num_t drop_y = 0;
    num_t too_many = 2;

    push_param_or_die(&drop_vm, &drop_y);
    push_param_or_die(&drop_vm, drop_arr);
    push_param_or_die(&drop_vm, &too_many);
    push_param_or_die(&drop_vm, &mark);

    run_func_expect(&drop_func, &drop_vm, VM_ARRAY_UNDERFLOW);

    count_t actual_drop_len;
    memcpy(&actual_drop_len, drop_arr, sizeof(actual_drop_len));
    assert(drop_y == 77);
    assert(actual_drop_len == 1);
    assert(drop_vm.param_stack.len == ARG_COUNT);

    vm_free_for_test(&drop_vm);
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
        ARG_DROP_COUNT,
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
        OP_PUSH_DROP_COUNT,
        OP_DROP_FROM_ARR,

        OP_COUNT,
    };

    static SigInput ins[] = {
        [ARG_ARR]  = {.var = {.tid = TYPE_INT_ARRAY4_ID, .name = "arr"}},
        [ARG_ONE]  = {.var = {.tid = TYPE_INT_ID,        .name = "one"}},
        [ARG_TWO]  = {.var = {.tid = TYPE_INT_ID,        .name = "two"}},
        [ARG_IDX0] = {.var = {.tid = TYPE_INT_ID,        .name = "idx0"}},
        [ARG_IDX1] = {.var = {.tid = TYPE_INT_ID,        .name = "idx1"}},
        [ARG_DROP_COUNT] = {.var = {.tid = TYPE_INT_ID,   .name = "drop_count"}},
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
        [ARG_DROP_COUNT] = {.tid = TYPE_INT_ID,   .name = "drop_count"},
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
        [OP_PUSH_DROP_COUNT]   = {.kind = OP_PUSH_ARG, .extra = ARG_DROP_COUNT},
        [OP_DROP_FROM_ARR]     = {.kind = OP_ARR_DROP, .extra = 0},
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
            .ins = {.data = ins, .len = 6},
            .outs = {.data = outs, .len = 2},
            
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
    num_t drop_count = 2;
    num_t y0 = 0;
    num_t y1 = 0;

    push_param_or_die(&vm, &y0);
    push_param_or_die(&vm, &y1);
    push_param_or_die(&vm, arr);
    push_param_or_die(&vm, &one);
    push_param_or_die(&vm, &two);
    push_param_or_die(&vm, &idx0);
    push_param_or_die(&vm, &idx1);
    push_param_or_die(&vm, &drop_count);

    run_func_or_die(&func, &vm);

    count_t len;
    memcpy(&len, arr, sizeof(len));

    assert(y0 == 11);
    assert(y1 == 22);
    assert(len == 0);
    assert(vm.param_stack.len == 2);

    vm_free_for_test(&vm);
}

static void test_slice_from_array_at_inc_and_dec(void) {
    enum {
        ARG_Y,
        ARG_SLICE,
        ARG_ARR,
        ARG_ZERO,
        ARG_ONE,
        ARG_COUNT,
    };

    enum {
        BLOCK_ROOT,
        BLOCK_COUNT,
    };

    enum {
        OP_PUSH_SLICE_FOR_FROM,
        OP_PUSH_ARR,
        OP_DO_SLICE_FROM_ARR,

        OP_PUSH_SLICE_FOR_INC,
        OP_PUSH_ONE_FOR_INC,
        OP_DO_SLICE_INC,

        OP_PUSH_SLICE_FOR_DEC,
        OP_PUSH_ONE_FOR_DEC,
        OP_DO_SLICE_DEC,

        OP_PUSH_Y,
        OP_PUSH_SLICE_FOR_AT,
        OP_PUSH_ZERO,
        OP_DO_SLICE_AT,
        OP_ASSIGN_Y,

        OP_COUNT,
    };

    static SigInput ins[] = {
        [0] = {.var = {.tid = TYPE_INT_ARRAY4_ID, .name = "arr"}},
        [1] = {.var = {.tid = TYPE_INT_ID,        .name = "zero"}},
        [2] = {.var = {.tid = TYPE_INT_ID,        .name = "one"}},
    };

    static Var outs[] = {
        {.tid = TYPE_INT_ID,       .name = "y"},
        {.tid = TYPE_INT_SLICE_ID, .name = "slice"},
    };

    static Var vars[] = {
        [ARG_Y]     = {.tid = TYPE_INT_ID,        .name = "y"},
        [ARG_SLICE] = {.tid = TYPE_INT_SLICE_ID,  .name = "slice"},
        [ARG_ARR]   = {.tid = TYPE_INT_ARRAY4_ID, .name = "arr"},
        [ARG_ZERO]  = {.tid = TYPE_INT_ID,        .name = "zero"},
        [ARG_ONE]   = {.tid = TYPE_INT_ID,        .name = "one"},
    };

    static OP ops[] = {
        [OP_PUSH_SLICE_FOR_FROM] = {.kind = OP_PUSH_ARG,      .extra = ARG_SLICE},
        [OP_PUSH_ARR]            = {.kind = OP_PUSH_ARG,      .extra = ARG_ARR},
        [OP_DO_SLICE_FROM_ARR]   = {.kind = OP_SLICE_FROM_AR, .extra = 0},

        [OP_PUSH_SLICE_FOR_INC]  = {.kind = OP_PUSH_ARG,      .extra = ARG_SLICE},
        [OP_PUSH_ONE_FOR_INC]    = {.kind = OP_PUSH_ARG,      .extra = ARG_ONE},
        [OP_DO_SLICE_INC]        = {.kind = OP_SLICE_INC,     .extra = 0},

        [OP_PUSH_SLICE_FOR_DEC]  = {.kind = OP_PUSH_ARG,      .extra = ARG_SLICE},
        [OP_PUSH_ONE_FOR_DEC]    = {.kind = OP_PUSH_ARG,      .extra = ARG_ONE},
        [OP_DO_SLICE_DEC]        = {.kind = OP_SLICE_DEC,     .extra = 0},

        [OP_PUSH_Y]              = {.kind = OP_PUSH_ARG,      .extra = ARG_Y},
        [OP_PUSH_SLICE_FOR_AT]   = {.kind = OP_PUSH_ARG,      .extra = ARG_SLICE},
        [OP_PUSH_ZERO]           = {.kind = OP_PUSH_ARG,      .extra = ARG_ZERO},
        [OP_DO_SLICE_AT]         = {.kind = OP_SLICE_AT,      .extra = 0},
        [OP_ASSIGN_Y]            = {.kind = OP_ASSIGN,        .extra = 0},
    };

    static Block blocks[] = {
        [BLOCK_ROOT] = {
            .kind = BLOCK_BASIC,
            .data.basic = {.start = OP_PUSH_SLICE_FOR_FROM, .len = OP_COUNT},
        },
    };

    Func func = {
        .name = "test_slice_from_array_at_inc_and_dec",
        .sig = {
            .ins = {.data = ins, .len = 3},
            .outs = {.data = outs, .len = 2},
            
        },
        .types = test_type_slice(),
        .blocks = {.data = blocks, .len = BLOCK_COUNT},
        .ops = {.data = ops, .len = OP_COUNT},
        .vars = {.data = vars, .len = ARG_COUNT},
    };

    VM vm;
    vm_init_for_test(&vm, 1024, 32, 8);

    alignas(Cell) unsigned char arr[128] = {0};
    count_t len = 2;
    num_t first = 11;
    num_t second = 22;
    assert(type_layout_all(test_type_slice()));
    size_t data_offset = test_types[TYPE_INT_ARRAY4_ID].data.array.data_offset;
    memcpy(arr, &len, sizeof(len));
    memcpy(arr + data_offset, &first, sizeof(first));
    memcpy(arr + data_offset + sizeof(second), &second, sizeof(second));

    alignas(Cell) unsigned char slice[type_slice_payload_size()];
    memset(slice, 0, sizeof(slice));
    num_t y = 0;
    num_t zero = 0;
    num_t one = 1;

    push_param_or_die(&vm, &y);
    push_param_or_die(&vm, slice);
    push_param_or_die(&vm, arr);
    push_param_or_die(&vm, &zero);
    push_param_or_die(&vm, &one);

    run_func_or_die(&func, &vm);

    void* slice_data;
    count_t slice_len;
    memcpy(&slice_data, slice, sizeof(slice_data));
    memcpy(&slice_len, slice + type_slice_len_offset(), sizeof(slice_len));

    assert(y == 22);
    assert(slice_data == arr + data_offset + sizeof(num_t));
    assert(slice_len == 1);

    vm_free_for_test(&vm);
}

static void test_struct_at_assigns_field(void) {
    enum {
        ARG_Y,
        ARG_PAIR,
        ARG_COUNT,
    };

    enum {
        BLOCK_ROOT,
        BLOCK_COUNT,
    };

    enum {
        OP_PUSH_Y,
        OP_PUSH_PAIR,
        OP_STRUCT_AT_SECOND,
        OP_ASSIGN_Y,
        OP_COUNT,
    };

    static SigInput ins[] = {
        [0] = {.var = {.tid = TYPE_PAIR_ID, .name = "pair"}},
    };

    static Var outs[] = {
        {.tid = TYPE_INT_ID, .name = "y"},
    };

    static Var vars[] = {
        [ARG_Y]    = {.tid = TYPE_INT_ID,  .name = "y"},
        [ARG_PAIR] = {.tid = TYPE_PAIR_ID, .name = "pair"},
    };

    static OP ops[] = {
        [OP_PUSH_Y]           = {.kind = OP_PUSH_ARG,  .extra = ARG_Y},
        [OP_PUSH_PAIR]        = {.kind = OP_PUSH_ARG,  .extra = ARG_PAIR},
        [OP_STRUCT_AT_SECOND] = {.kind = OP_STRUCT_AT, .extra = 1},
        [OP_ASSIGN_Y]         = {.kind = OP_ASSIGN,    .extra = 0},
    };

    static Block blocks[] = {
        [BLOCK_ROOT] = {
            .kind = BLOCK_BASIC,
            .data.basic = {.start = OP_PUSH_Y, .len = OP_COUNT},
        },
    };

    Func func = {
        .name = "test_struct_at_assigns_field",
        .sig = {
            .ins = {.data = ins, .len = 1},
            .outs = {.data = outs, .len = 1},
        },
        .types = test_type_slice(),
        .blocks = {.data = blocks, .len = BLOCK_COUNT},
        .ops = {.data = ops, .len = OP_COUNT},
        .vars = {.data = vars, .len = ARG_COUNT},
    };

    VM vm;
    vm_init_for_test(&vm, 1024, 8, 8);

    assert(type_layout_all(test_type_slice()));
    alignas(Cell) unsigned char pair[64] = {0};
    num_t first = 11;
    num_t second = 22;
    memcpy(pair + test_pair_fields[0].offset, &first, sizeof(first));
    memcpy(pair + test_pair_fields[1].offset, &second, sizeof(second));

    num_t y = 0;
    push_param_or_die(&vm, &y);
    push_param_or_die(&vm, pair);

    run_func_or_die(&func, &vm);

    assert(y == second);

    vm_free_for_test(&vm);
}

static void test_view_rejects_slice_mutation(void) {
    enum {
        ARG_VIEW,
        ARG_ONE,
        ARG_COUNT,
    };

    enum {
        BLOCK_ROOT,
        BLOCK_COUNT,
    };

    enum {
        OP_PUSH_VIEW,
        OP_PUSH_ONE,
        OP_INC_VIEW,
        OP_COUNT,
    };

    static SigInput ins[] = {
        [0] = {.var = {.tid = TYPE_INT_ID, .name = "one"}},
    };

    static Var outs[] = {
        {.tid = TYPE_INT_VIEW_ID, .name = "view"},
    };

    static Var vars[] = {
        [ARG_VIEW] = {.tid = TYPE_INT_VIEW_ID, .name = "view"},
        [ARG_ONE]  = {.tid = TYPE_INT_ID,      .name = "one"},
    };

    static OP ops[] = {
        [OP_PUSH_VIEW] = {.kind = OP_PUSH_ARG,  .extra = ARG_VIEW},
        [OP_PUSH_ONE]  = {.kind = OP_PUSH_ARG,  .extra = ARG_ONE},
        [OP_INC_VIEW]  = {.kind = OP_SLICE_INC, .extra = 0},
    };

    static Block blocks[] = {
        [BLOCK_ROOT] = {
            .kind = BLOCK_BASIC,
            .data.basic = {.start = OP_PUSH_VIEW, .len = OP_COUNT},
        },
    };

    Func func = {
        .name = "test_view_rejects_slice_mutation",
        .sig = {
            .ins = {.data = ins, .len = 1},
            .outs = {.data = outs, .len = 1},
            
        },
        .types = test_type_slice(),
        .blocks = {.data = blocks, .len = BLOCK_COUNT},
        .ops = {.data = ops, .len = OP_COUNT},
        .vars = {.data = vars, .len = ARG_COUNT},
    };

    VmCode code = vm_compile_no_defers(&func, NULL);
    assert(!code.data);
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

    static SigInput ins[] = {
        [ARG_ONE] = {.var = {.tid = TYPE_INT_ID, .name = "one"}},
        [ARG_TWO] = {.var = {.tid = TYPE_INT_ID, .name = "two"}},
        [ARG_BAD] = {.var = {.tid = TYPE_INT_ID, .name = "bad"}},
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

    static SigInput ins[] = {
        [ARG_ONE]   = {.var = {.tid = TYPE_INT_ID, .name = "one"}},
        [ARG_TWO]   = {.var = {.tid = TYPE_INT_ID, .name = "two"}},
        [ARG_THREE] = {.var = {.tid = TYPE_INT_ID, .name = "three"}},
        [ARG_BAD]   = {.var = {.tid = TYPE_INT_ID, .name = "bad"}},
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

static void test_push_global_assigns_value(void) {
    enum {
        ARG_Y,
        ARG_COUNT,
    };

    enum {
        GLOBAL_X,
        GLOBAL_COUNT,
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

    static Var outs[] = {
        {.tid = TYPE_INT_ID, .name = "y"},
    };

    static Var vars[] = {
        [ARG_Y] = {.tid = TYPE_INT_ID, .name = "y"},
    };

    static OP ops[] = {
        [OP_PUSH_Y]   = {.kind = OP_PUSH_ARG,    .extra = ARG_Y},
        [OP_PUSH_X]   = {.kind = OP_PUSH_GLOBAL, .extra = GLOBAL_X},
        [OP_ASSIGN_Y] = {.kind = OP_ASSIGN,      .extra = 0},
    };

    static Block blocks[] = {
        [BLOCK_ROOT] = {
            .kind = BLOCK_BASIC,
            .data.basic = {.start = OP_PUSH_Y, .len = 3},
        },
    };

    num_t x = 42;
    Global globals[] = {
        [GLOBAL_X] = {.var = {.tid = TYPE_INT_ID, .name = "x"}, .mem = &x},
    };

    CompileContext ctx = {
        .globals = {.data = globals, .len = GLOBAL_COUNT, .cap = GLOBAL_COUNT},
    };

    Func func = {
        .name = "test_push_global_assigns_value",
        .sig = {
            .ins = {.data = NULL, .len = 0},
            .outs = {.data = outs, .len = 1},
            
        },
        .types = test_type_slice(),
        .blocks = {.data = blocks, .len = BLOCK_COUNT},
        .ops = {.data = ops, .len = OP_COUNT},
        .vars = {.data = vars, .len = ARG_COUNT},
    };

    VM vm;
    vm_init_for_test(&vm, 1024, 8, 8);

    num_t y = 0;
    push_param_or_die(&vm, &y);

    VmCode code = vm_compile_no_defers(&func, &ctx);
    assert(code.data);
    assert(vm_run(&vm, code.data) == VM_OK);

    assert(y == x);

    free(code.data);
    vm_free_for_test(&vm);
}

static void test_compiled_function_call(void) {
    enum {
        ARG_Y,
        ARG_X,
        ARG_COUNT,
    };

    enum {
        FUNC_CALLEE,
        FUNC_COUNT,
    };

    enum {
        BLOCK_ROOT,
        BLOCK_COUNT,
    };

    enum {
        OP_PUSH_Y,
        OP_PUSH_X,
        OP_CALL_CALLEE,
        OP_COUNT,
    };

    static SigInput ins[] = {
        [0] = {.var = {.tid = TYPE_INT_ID, .name = "x"}},
    };

    static Var outs[] = {
        [0] = {.tid = TYPE_INT_ID, .name = "y"},
    };

    static Var vars[] = {
        [ARG_Y] = {.tid = TYPE_INT_ID, .name = "y"},
        [ARG_X] = {.tid = TYPE_INT_ID, .name = "x"},
    };

    static OP callee_ops[] = {
        [OP_PUSH_Y] = {.kind = OP_PUSH_ARG, .extra = ARG_Y},
        [OP_PUSH_X] = {.kind = OP_PUSH_ARG, .extra = ARG_X},
        [OP_CALL_CALLEE] = {.kind = OP_ASSIGN, .extra = 0},
    };

    static OP caller_ops[] = {
        [OP_PUSH_Y] = {.kind = OP_PUSH_ARG, .extra = ARG_Y},
        [OP_PUSH_X] = {.kind = OP_PUSH_ARG, .extra = ARG_X},
        [OP_CALL_CALLEE] = {.kind = OP_CALL, .extra = FUNC_CALLEE},
    };

    static Block blocks[] = {
        [BLOCK_ROOT] = {
            .kind = BLOCK_BASIC,
            .data.basic = {.start = OP_PUSH_Y, .len = 3},
        },
    };

    Func funcs[] = {
        [FUNC_CALLEE] = {
            .name = "callee_assign",
            .sig = {
                .ins = {.data = ins, .len = 1},
                .outs = {.data = outs, .len = 1},
                
            },
            .types = test_type_slice(),
            .blocks = {.data = blocks, .len = BLOCK_COUNT},
            .ops = {.data = callee_ops, .len = OP_COUNT},
            .vars = {.data = vars, .len = ARG_COUNT},
        },
    };

    Func caller = {
        .name = "caller",
        .sig = {
            .ins = {.data = ins, .len = 1},
            .outs = {.data = outs, .len = 1},
            
        },
        .types = test_type_slice(),
        .blocks = {.data = blocks, .len = BLOCK_COUNT},
        .ops = {.data = caller_ops, .len = OP_COUNT},
        .vars = {.data = vars, .len = ARG_COUNT},
    };

    CompileContext ctx = {
        .funcs = {.data = funcs, .len = FUNC_COUNT, .cap = FUNC_COUNT},
    };

    VM vm;
    vm_init_for_test(&vm, 1024, 16, 8);

    num_t y = 0;
    num_t x = 17;
    push_param_or_die(&vm, &y);
    push_param_or_die(&vm, &x);

    VmCode code = vm_compile_no_defers(&caller, &ctx);
    assert(code.data);
    assert(ctx.code.len == FUNC_COUNT);
    assert(ctx.code.data[FUNC_CALLEE].data);
    assert(vm_run(&vm, code.data) == VM_OK);

    assert(y == x);

    free(code.data);
    for(size_t i=0;i<ctx.code.len;i++) free(ctx.code.data[i].data);
    free(ctx.code.data);
    vm_free_for_test(&vm);
}

static void test_callee_crash_unwinds_to_caller_pad(void) {
    enum {
        ARG_Y,
        ARG_X,
        ARG_MARK,
        ARG_COUNT,
    };

    enum {
        FUNC_CALLEE,
        FUNC_COUNT,
    };

    enum {
        CALLER_BLOCK_ROOT,
        CALLER_BLOCK_BODY,
        CALLER_BLOCK_PAD,
        CALLER_BLOCK_COUNT,
    };

    enum {
        CALLER_OP_PUSH_X,
        CALLER_OP_CALL_CALLEE,
        CALLER_OP_PUSH_Y,
        CALLER_OP_PUSH_MARK,
        CALLER_OP_ASSIGN_Y,
        CALLER_OP_COUNT,
    };

    enum {
        CALLEE_BLOCK_ROOT,
        CALLEE_BLOCK_COUNT,
    };

    static SigInput callee_ins[] = {
        [0] = {.var = {.tid = TYPE_INT_ID, .name = "x"}},
    };

    static SigInput caller_ins[] = {
        [0] = {.var = {.tid = TYPE_INT_ID, .name = "x"}},
        [1] = {.var = {.tid = TYPE_INT_ID, .name = "mark"}},
    };

    static Var callee_vars[] = {
        [0] = {.tid = TYPE_INT_ID, .name = "x"},
    };

    static Var caller_outs[] = {
        [0] = {.tid = TYPE_INT_ID, .name = "y"},
    };

    static Var caller_vars[] = {
        [ARG_Y] = {.tid = TYPE_INT_ID, .name = "y"},
        [ARG_X] = {.tid = TYPE_INT_ID, .name = "x"},
        [ARG_MARK] = {.tid = TYPE_INT_ID, .name = "mark"},
    };

    static Block callee_blocks[] = {
        [CALLEE_BLOCK_ROOT] = {.kind = BLOCK_CRASH},
    };

    static OP caller_ops[] = {
        [CALLER_OP_PUSH_X] = {.kind = OP_PUSH_ARG, .extra = ARG_X},
        [CALLER_OP_CALL_CALLEE] = {.kind = OP_CALL, .extra = FUNC_CALLEE},
        [CALLER_OP_PUSH_Y] = {.kind = OP_PUSH_ARG, .extra = ARG_Y},
        [CALLER_OP_PUSH_MARK] = {.kind = OP_PUSH_ARG, .extra = ARG_MARK},
        [CALLER_OP_ASSIGN_Y] = {.kind = OP_ASSIGN, .extra = 0},
    };

    static Block caller_blocks[] = {
        [CALLER_BLOCK_ROOT] = {
            .kind = BLOCK_CRASH_PAD,
            .data.crash_pad = {.body = CALLER_BLOCK_BODY, .pad = CALLER_BLOCK_PAD},
        },
        [CALLER_BLOCK_BODY] = {
            .kind = BLOCK_BASIC,
            .data.basic = {.start = CALLER_OP_PUSH_X, .len = 2},
        },
        [CALLER_BLOCK_PAD] = {
            .kind = BLOCK_BASIC,
            .data.basic = {.start = CALLER_OP_PUSH_Y, .len = 3},
        },
    };

    Func funcs[] = {
        [FUNC_CALLEE] = {
            .name = "callee_crash",
            .sig = {
                .ins = {.data = callee_ins, .len = 1},
                .outs = {.data = NULL, .len = 0},
                
            },
            .types = test_type_slice(),
            .blocks = {.data = callee_blocks, .len = CALLEE_BLOCK_COUNT},
            .ops = {.data = NULL, .len = 0},
            .vars = {.data = callee_vars, .len = 1},
        },
    };

    Func caller = {
        .name = "caller_pad_catches_callee_crash",
        .sig = {
            .ins = {.data = caller_ins, .len = 2},
            .outs = {.data = caller_outs, .len = 1},
            
        },
        .types = test_type_slice(),
        .blocks = {.data = caller_blocks, .len = CALLER_BLOCK_COUNT},
        .ops = {.data = caller_ops, .len = CALLER_OP_COUNT},
        .vars = {.data = caller_vars, .len = ARG_COUNT},
    };

    CompileContext ctx = {
        .funcs = {.data = funcs, .len = FUNC_COUNT, .cap = FUNC_COUNT},
    };

    VM vm;
    vm_init_for_test(&vm, 1024, 16, 8);

    num_t y = 0;
    num_t x = 17;
    num_t mark = 88;
    push_param_or_die(&vm, &y);
    push_param_or_die(&vm, &x);
    push_param_or_die(&vm, &mark);

    VmCode code = vm_compile_no_defers(&caller, &ctx);
    assert(code.data);
    assert(ctx.code.len == FUNC_COUNT);
    assert(ctx.code.data[FUNC_CALLEE].data);
    assert(vm_run(&vm, code.data) == VM_CRASH);

    assert(y == mark);
    assert(vm.param_stack.len == ARG_COUNT);

    free(code.data);
    for(size_t i=0;i<ctx.code.len;i++) free(ctx.code.data[i].data);
    free(ctx.code.data);
    vm_free_for_test(&vm);
}

static VM_RESULT native_assign_99(VM* vm) {
    if(vm->param_stack.len < 1) return VM_PARAM_UNDERFLOW;
    num_t x = 99;
    memcpy(TOP(vm->param_stack), &x, sizeof(x));
    return VM_OK;
}

static void test_native_call_from_global(void) {
    enum {
        ARG_Y,
        ARG_COUNT,
    };

    enum {
        GLOBAL_NATIVE,
        GLOBAL_COUNT,
    };

    enum {
        BLOCK_ROOT,
        BLOCK_COUNT,
    };

    enum {
        OP_PUSH_Y,
        OP_PUSH_NATIVE,
        OP_CALL_NATIVE,
        OP_COUNT,
    };

    static Var outs[] = {
        {.tid = TYPE_INT_ID, .name = "y"},
    };

    static Var vars[] = {
        [ARG_Y] = {.tid = TYPE_INT_ID, .name = "y"},
    };

    static OP ops[] = {
        [OP_PUSH_Y]      = {.kind = OP_PUSH_ARG,             .extra = ARG_Y},
        [OP_PUSH_NATIVE] = {.kind = OP_PUSH_GLOBAL,          .extra = GLOBAL_NATIVE},
        [OP_CALL_NATIVE] = {.kind = OP_CALL_NATIVE_ON_STACK, .extra = 0},
    };

    static Block blocks[] = {
        [BLOCK_ROOT] = {
            .kind = BLOCK_BASIC,
            .data.basic = {.start = OP_PUSH_Y, .len = 3},
        },
    };

    VmNativeFunc native = native_assign_99;
    Global globals[] = {
        [GLOBAL_NATIVE] = {
            .var = {.tid = TYPE_NATIVE_FUNC_POINTER_ID, .name = "native"},
            .mem = &native,
            .is_mut = false,
        },
    };

    CompileContext ctx = {
        .globals = {.data = globals, .len = GLOBAL_COUNT, .cap = GLOBAL_COUNT},
    };

    Func func = {
        .name = "test_native_call_from_global",
        .sig = {
            .ins = {.data = NULL, .len = 0},
            .outs = {.data = outs, .len = 1},
            
        },
        .types = test_type_slice(),
        .blocks = {.data = blocks, .len = BLOCK_COUNT},
        .ops = {.data = ops, .len = OP_COUNT},
        .vars = {.data = vars, .len = ARG_COUNT},
    };

    VM vm;
    vm_init_for_test(&vm, 1024, 8, 8);

    num_t y = 0;
    push_param_or_die(&vm, &y);

    VmCode code = vm_compile_no_defers(&func, &ctx);
    assert(code.data);
    assert(vm_run(&vm, code.data) == VM_OK);

    assert(y == 99);

    free(code.data);
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

    test_hard_crash_returns_vm_hard_crash();
    puts("ok: test_hard_crash_returns_vm_hard_crash");

    test_array_crashes_unwind_stack();
    puts("ok: test_array_crashes_unwind_stack");

    test_array_push_at_and_drop();
    puts("ok: test_array_push_at_and_drop");

    test_slice_from_array_at_inc_and_dec();
    puts("ok: test_slice_from_array_at_inc_and_dec");

    test_struct_at_assigns_field();
    puts("ok: test_struct_at_assigns_field");

    test_view_rejects_slice_mutation();
    puts("ok: test_view_rejects_slice_mutation");

    test_loop_break_skips_unreachable_body_tail();
    puts("ok: test_loop_break_skips_unreachable_body_tail");

    test_nested_many_break_skips_outer_tail();
    puts("ok: test_nested_many_break_skips_outer_tail");

    test_push_global_assigns_value();
    puts("ok: test_push_global_assigns_value");

    test_compiled_function_call();
    puts("ok: test_compiled_function_call");

    test_callee_crash_unwinds_to_caller_pad();
    puts("ok: test_callee_crash_unwinds_to_caller_pad");

    test_native_call_from_global();
    puts("ok: test_native_call_from_global");

    puts("all vm/ir tests passed");
    return 0;
}
