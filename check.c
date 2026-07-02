#include "check.h"
#include <stddef.h>
#include <stdint.h>
#include <limits.h>
#include <string.h>

typedef struct StackCalc {
    size_t need; // caller-provided values needed below local stack
    size_t have; // values currently available above that caller base
} StackCalc;

static int checked_add_size(size_t* out, size_t a, size_t b) {
    if (SIZE_MAX - a < b) {
        return 1;
    }

    *out = a + b;
    return 0;
}

static int stack_calc_apply(StackCalc* calc, size_t pop, size_t push) {
    if (!calc) {
        return 1;
    }

    if (calc->have < pop) {
        size_t missing = pop - calc->have;

        if (checked_add_size(&calc->need, calc->need, missing)) {
            return 1;
        }

        calc->have = 0;
    } else {
        calc->have -= pop;
    }

    if (checked_add_size(&calc->have, calc->have, push)) {
        return 1;
    }

    return 0;
}

static int op_stack_effect(OP_KIND kind, size_t* pop, size_t* push) {
    if (!pop || !push) {
        return 1;
    }

    *pop = 0;
    *push = 0;

    switch (kind) {
    case OP_CALL:
        /*
            Cannot infer without function signature info.
            Reject in this local-only pass.
        */
        return 1;

    case OP_NEST_BLOCK:
        /*
            Cannot infer without the nested block's BlockSig.
            Reject in this local-only pass.
        */
        return 1;

    case OP_ASSIGN:
    case OP_ADD_ASSIGN:
    case OP_SUB_ASSIGN:
    case OP_MUL_ASSIGN:
    case OP_DIV_ASSIGN:
    case OP_AND_ASSIGN:
    case OP_OR_ASSIGN:
    case OP_XOR_ASSIGN:
        *pop = 2;
        *push = 0;
        return 0;

    case OP_BIT_NOT_ASSIGN:
        *pop = 1;
        *push = 0;
        return 0;

    case OP_DROP:
        *pop = 1;
        *push = 0;
        return 0;

    case OP_PUSH_VAR:
    case OP_PUSH_ARG:
    case OP_PUSH_GLOBAL:
        *pop = 0;
        *push = 1;
        return 0;

    case OP_ARR_PUSH:
        /*
            Assumption: arr_push consumes arr + value and mutates arr.
            Change if your semantics differ.
        */
        *pop = 2;
        *push = 0;
        return 0;

    case OP_ARR_AT:
        /*
            Assumption: arr_at consumes arr + index and pushes result/ref.
        */
        *pop = 2;
        *push = 1;
        return 0;

    case OP_ARR_DROP:
        /*
            Assumption: arr_drop consumes arr + index/count.
            Change if it consumes only one value.
        */
        *pop = 2;
        *push = 0;
        return 0;
    }

    return 1;
}

static int term_stack_effect(TERM_KIND kind, size_t* pop, size_t* push) {
    if (!pop || !push) {
        return 1;
    }

    *pop = 0;
    *push = 0;

    switch (kind) {
    case TERM_GOTO:
        *pop = 0;
        *push = 0;
        return 0;

    case TERM_BRANCH:
        /*
            Branch exits the block by popping one condition value.
        */
        *pop = 1;
        *push = 0;
        return 0;
    }

    return 1;
}

static int put_call_sizes(const Block* block, BlockSig* sig) {
    if (!block || !sig) {
        return 1;
    }

    StackCalc calc = {0};

    for (size_t i = 0; i < block->ops.len; i++) {
        const OP op = block->ops.data[i];

        size_t pop = 0;
        size_t push = 0;

        if (op_stack_effect(op.kind, &pop, &push)) {
            return 1;
        }

        if (stack_calc_apply(&calc, pop, push)) {
            return 1;
        }
    }

    {
        size_t pop = 0;
        size_t push = 0;

        if (term_stack_effect(block->term.kind, &pop, &push)) {
            return 1;
        }

        if (stack_calc_apply(&calc, pop, push)) {
            return 1;
        }
    }

    sig->will_pop = calc.need;

    /*
        Size-only pass. Param data is filled by a later type/alias pass.
    */
    sig->args.data = NULL;
    sig->args.len = calc.need;
    sig->args.cap = 0;

    sig->will_push.data = NULL;
    sig->will_push.len = calc.have;
    sig->will_push.cap = 0;

    return 0;
}

int verify_func(const Func* func){
	BlockSig* sigs = calloc(func->blocks.len,sizeof(BlockSig));
	assert(sigs);
    if(put_call_sizes(func,sigs)) return 1;

}