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
        ARG_X,
        ARG_Y,
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

    push_param_or_die(&vm, &x);
    push_param_or_die(&vm, &y);

    run_func_or_die(&func, &vm);

    assert(y == 123);

    vm_free_for_test(&vm);
}

static void test_crash_pad_writes_y1_but_not_y2_after_crash(void) {
    enum {
        ARG_ONE,
        ARG_TWO,
        ARG_Y1,
        ARG_Y2,
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

    push_param_or_die(&vm, &one);
    push_param_or_die(&vm, &two);
    push_param_or_die(&vm, &y1);
    push_param_or_die(&vm, &y2);

    run_func_or_die(&func, &vm);

    assert(y1 == 1);
    assert(y2 == 0);
    assert(y2 != 2);

    vm_free_for_test(&vm);
}

static void test_crash_pad_body_runs_normally_without_crash(void) {
    enum {
        ARG_ONE,
        ARG_TWO,
        ARG_Y1,
        ARG_Y2,
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

    push_param_or_die(&vm, &one);
    push_param_or_die(&vm, &two);
    push_param_or_die(&vm, &y1);
    push_param_or_die(&vm, &y2);

    run_func_or_die(&func, &vm);

    assert(y1 == 0);
    assert(y2 == 2);

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

    puts("all vm crash pad tests passed");
    return 0;
}