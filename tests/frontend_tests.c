#include "../frontend.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static Type test_types[] = {
	[0] = {.kind = TYPE_INT,.name = "int",.payload_size = sizeof(num_t),.size = sizeof(num_t),.align = alignof(num_t)},
	[1] = {.kind = TYPE_BYTE,.name = "byte",.payload_size = 1,.size = 1,.align = 1},
};

static TypeS test_type_slice(void){
	return (TypeS){.data = test_types,.len = sizeof(test_types) / sizeof(test_types[0])};
}

static void vm_init_for_test(VM* vm){
	memset(vm,0,sizeof(*vm));
	vm->storage.data = malloc(1024);
	vm->storage.cap = 1024;
	vm->param_stack.data = malloc(16 * sizeof(*vm->param_stack.data));
	vm->param_stack.cap = 16;
	vm->crash_stack.data = malloc(8 * sizeof(*vm->crash_stack.data));
	vm->crash_stack.cap = 8;
	assert(vm->storage.data);
	assert(vm->param_stack.data);
	assert(vm->crash_stack.data);
}

static void test_basic_frontend_assignment(void){
	static Var vars[] = {
		[0] = {.tid = TYPE_INT_ID,.name = "Y"},
		[1] = {.tid = TYPE_INT_ID,.name = "X"},
	};
	static SigInput ins[] = {{.var = {.tid = TYPE_INT_ID,.name = "X"}}};
	static Var outs[] = {{.tid = TYPE_INT_ID,.name = "Y"}};

	Func func = {
		.name = "assign_from_source",
		.sig = {.ins = {.data = ins,.len = 1},.outs = {.data = outs,.len = 1}},
		.types = test_type_slice(),
		.vars = {.data = vars,.len = 2},
	};
	Frontend fe;
	frontend_init(&fe,NULL,&func);
	assert(frontend_add_core_words(&fe));
	assert(frontend_compile_source(&fe,"( Y X Assign )"));
	assert(func.ops.len == 3);
	assert(func.ops.data[0].kind == OP_PUSH_ARG && func.ops.data[0].extra == 0);
	assert(func.ops.data[1].kind == OP_PUSH_ARG && func.ops.data[1].extra == 1);
	assert(func.ops.data[2].kind == OP_ASSIGN);

	VM vm;
	vm_init_for_test(&vm);
	num_t y = 0;
	num_t x = 42;
	assert(vm_push_param(&vm,&y) == VM_OK);
	assert(vm_push_param(&vm,&x) == VM_OK);
	VmCode code = vm_compile_no_defers(&func,NULL);
	assert(code.data);
	assert(vm_run(&vm,code.data) == VM_OK);
	assert(y == 42);

	vm_code_free(&code);
	vm_free(&vm);
	free(func.blocks.data);
	free(func.ops.data);
	frontend_free(&fe);
}

static VM_RESULT macro_emit_x(VM* vm){
	Frontend* fe = vm->user;
	assert(fe);
	return frontend_emit_op(fe,(OP){.kind = OP_PUSH_ARG,.extra = 1}) ? VM_OK : VM_INVALID_BYTECODE;
}

static void test_immediate_runs_on_vm_during_compile(void){
	static VmNativeFunc macro_fn = macro_emit_x;
	static Var vars[] = {
		[0] = {.tid = TYPE_INT_ID,.name = "Y"},
		[1] = {.tid = TYPE_INT_ID,.name = "X"},
	};
	static SigInput ins[] = {{.var = {.tid = TYPE_INT_ID,.name = "X"}}};
	static Var outs[] = {{.tid = TYPE_INT_ID,.name = "Y"}};
	static Type macro_types[] = {
		[0] = {.kind = TYPE_INT,.name = "int",.payload_size = sizeof(num_t),.size = sizeof(num_t),.align = alignof(num_t)},
		[1] = {.kind = TYPE_BYTE,.name = "byte",.payload_size = 1,.size = 1,.align = 1},
		[2] = {.kind = TYPE_NATIVE_FUNC_POINTER,.name = "native_fn",.payload_size = sizeof(VmNativeFunc),.size = sizeof(VmNativeFunc),.align = alignof(VmNativeFunc)},
	};
	static OP macro_ops[] = {
		{.kind = OP_PUSH_GLOBAL,.extra = 0},
		{.kind = OP_CALL_NATIVE_ON_STACK},
	};
	static Block macro_blocks[] = {{.kind = BLOCK_BASIC,.data.basic = {.start = 0,.len = 2}}};
	static Global globals[] = {{.var = {.tid = 2,.name = "emit_x"},.mem = &macro_fn}};

	Func macro = {
		.name = "EmitX",
		.types = {.data = macro_types,.len = 3},
		.blocks = {.data = macro_blocks,.len = 1},
		.ops = {.data = macro_ops,.len = 2},
	};
	Func func = {
		.name = "macro_source",
		.sig = {.ins = {.data = ins,.len = 1},.outs = {.data = outs,.len = 1}},
		.types = test_type_slice(),
		.vars = {.data = vars,.len = 2},
	};
	CompileContext ctx = {.globals = {.data = globals,.len = 1,.cap = 1}};
	Frontend fe;
	frontend_init(&fe,&ctx,&func);
	assert(frontend_add_core_words(&fe));
	assert(frontend_add_word_immediate(&fe,"EmitX",&macro));
	assert(frontend_compile_source(&fe,"( Y EmitX Assign )"));
	assert(func.ops.len == 3);
	assert(func.ops.data[0].kind == OP_PUSH_ARG && func.ops.data[0].extra == 0);
	assert(func.ops.data[1].kind == OP_PUSH_ARG && func.ops.data[1].extra == 1);
	assert(func.ops.data[2].kind == OP_ASSIGN);

	free(func.blocks.data);
	free(func.ops.data);
	frontend_free(&fe);
}

int main(void){
	test_basic_frontend_assignment();
	puts("ok: test_basic_frontend_assignment");
	test_immediate_runs_on_vm_during_compile();
	puts("ok: test_immediate_runs_on_vm_during_compile");
	puts("frontend tests passed");
	return 0;
}
