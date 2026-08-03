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

	Func macros[] = {{
		.name = "EmitX",
		.types = {.data = macro_types,.len = 3},
		.blocks = {.data = macro_blocks,.len = 1},
		.ops = {.data = macro_ops,.len = 2},
	}};
	Func func = {
		.name = "macro_source",
		.sig = {.ins = {.data = ins,.len = 1},.outs = {.data = outs,.len = 1}},
		.types = test_type_slice(),
		.vars = {.data = vars,.len = 2},
	};
	CompileContext ctx = {
		.globals = {.data = globals,.len = 1,.cap = 1},
		.funcs = {.data = macros,.len = 1,.cap = 1},
	};
	Frontend fe;
	frontend_init(&fe,&ctx,&func);
	assert(frontend_add_core_words(&fe));
	assert(frontend_add_word_immediate(&fe,"EmitX",0));
	assert(frontend_compile_source(&fe,"( Y EmitX Assign )"));
	assert(func.ops.len == 3);
	assert(func.ops.data[0].kind == OP_PUSH_ARG && func.ops.data[0].extra == 0);
	assert(func.ops.data[1].kind == OP_PUSH_ARG && func.ops.data[1].extra == 1);
	assert(func.ops.data[2].kind == OP_ASSIGN);
	VmCode code = vm_compile_no_defers(&func,&ctx);
	assert(code.data);

	vm_code_free(&code);
	free(func.blocks.data);
	free(func.ops.data);
	frontend_free(&fe);
}

static void test_var_parses_types_and_builds_local_scopes(void){
	static Var args[] = {
		{.tid = TYPE_INT_ID,.name = "Y"},
		{.tid = TYPE_INT_ID,.name = "X"},
	};
	static SigInput ins[] = {{.var = {.tid = TYPE_INT_ID,.name = "X"}}};
	static Var outs[] = {{.tid = TYPE_INT_ID,.name = "Y"}};
	Func func = {
		.name = "locals_from_source",
		.sig = {.ins = {.data = ins,.len = 1},.outs = {.data = outs,.len = 1}},
		.types = test_type_slice(),
		.vars = {.data = args,.len = 2},
	};
	Frontend fe;
	frontend_init(&fe,NULL,&func);
	assert(frontend_add_core_words(&fe));
	assert(frontend_compile_source(&fe,
		"Var Slice Int Values Var Array 4 Byte Bytes Var Int Temp "
		"( Temp X Assign Y Temp Assign )"));

	assert(func.types.len == 4);
	assert(func.types.data[2].kind == TYPE_SLICE);
	assert(func.types.data[2].data.ref.elem == TYPE_INT_ID);
	assert(func.types.data[3].kind == TYPE_ARRAY);
	assert(func.types.data[3].data.array.elem == TYPE_BYTE_ID);
	assert(func.types.data[3].data.array.capacity == 4);
	assert(func.vars.len == 5);
	assert(strcmp(func.vars.data[2].name,"Values") == 0);
	assert(strcmp(func.vars.data[3].name,"Bytes") == 0);
	assert(strcmp(func.vars.data[4].name,"Temp") == 0);
	assert(func.ops.data[0].kind == OP_PUSH_VAR && func.ops.data[0].extra == 4);
	assert(func.ops.data[1].kind == OP_PUSH_ARG && func.ops.data[1].extra == 1);
	assert(func.ops.data[3].kind == OP_PUSH_ARG && func.ops.data[3].extra == 0);
	assert(func.ops.data[4].kind == OP_PUSH_VAR && func.ops.data[4].extra == 4);

	VM vm;
	vm_init_for_test(&vm);
	num_t y = 0;
	num_t x = 73;
	assert(vm_push_param(&vm,&y) == VM_OK);
	assert(vm_push_param(&vm,&x) == VM_OK);
	VmCode code = vm_compile_no_defers(&func,NULL);
	assert(code.data);
	assert(vm_run(&vm,code.data) == VM_OK);
	assert(y == 73);

	vm_code_free(&code);
	vm_free(&vm);
	frontend_free(&fe);
	free(func.types.data);
	free(func.vars.data);
	free(func.blocks.data);
	free(func.ops.data);
}

static void test_vm_parser_services(void){
	Func func = {.types = test_type_slice()};
	Frontend fe;
	frontend_init(&fe,NULL,&func);
	fe.parser = (FrontendParser){.source = "Thing -42 View Byte",.cursor = "Thing -42 View Byte"};

	VM* vm = &fe.macro_vm;
	FrontendName name = {0};
	assert(vm_parse_name(vm,&name));
	assert(name.len == 5 && memcmp(name.data,"Thing",5) == 0);

	num_t number = 0;
	assert(vm_parse_number(vm,&number));
	assert(number == -42);

	type_idx tid = TYPE_INVALID_ID;
	assert(vm_parse_type(vm,&tid));
	assert(tid == 2);
	assert(func.types.data[tid].kind == TYPE_VIEW);
	assert(func.types.data[tid].data.ref.elem == TYPE_BYTE_ID);
	assert(frontend_prepare_func(&fe));
	VmCode code = vm_compile_no_defers(&func,NULL);
	assert(code.data);

	vm_code_free(&code);
	frontend_free(&fe);
	free(func.blocks.data);
	free(func.types.data);
}

static void test_utf8_names_and_unicode_whitespace(void){
	Type types[] = {
		[0] = {.kind = TYPE_INT,.name = "int",.payload_size = sizeof(num_t),.size = sizeof(num_t),.align = alignof(num_t)},
		[1] = {.kind = TYPE_BYTE,.name = "byte",.payload_size = 1,.size = 1,.align = 1},
		[2] = {.kind = TYPE_INT,.name = "整数",.payload_size = sizeof(num_t),.size = sizeof(num_t),.align = alignof(num_t)},
	};
	Func func = {.name = "函数",.types = {.data = types,.len = 3}};
	Frontend fe;
	frontend_init(&fe,NULL,&func);
	assert(frontend_add_core_words(&fe));
	assert(frontend_compile_source(&fe,"Var　整数　値"));
	assert(func.vars.len == 1);
	assert(strcmp(func.vars.data[0].name,"値") == 0);
	assert(func.vars.data[0].tid == 2);

	frontend_free(&fe);
	free(func.vars.data);
	free(func.blocks.data);
}

static void test_ir_interpreter_handles_empty_source_and_recovers(void){
	Func func = {.name = "interpreter_recovery",.types = test_type_slice()};
	Frontend fe;
	frontend_init(&fe,NULL,&func);
	assert(frontend_add_core_words(&fe));

	assert(frontend_compile_source(&fe,""));
	assert(fe.interpreter_result == VM_OK);
	assert(func.ops.len == 0);

	assert(!frontend_compile_source(&fe,"MissingWord"));
	assert(fe.error == FRONTEND_UNKNOWN_WORD);
	assert(fe.interpreter_result == VM_HARD_CRASH);
	assert(fe.interpreter_vm.storage.len == 0);
	assert(fe.interpreter_vm.param_stack.len == 0);
	assert(fe.interpreter_vm.crash_stack.len == 0);

	assert(frontend_compile_source(&fe,"()"));
	assert(fe.interpreter_result == VM_OK);
	assert(func.ops.len == 0);

	frontend_free(&fe);
	free(func.blocks.data);
	free(func.ops.data);
}

int main(void){
	test_basic_frontend_assignment();
	puts("ok: test_basic_frontend_assignment");
	test_immediate_runs_on_vm_during_compile();
	puts("ok: test_immediate_runs_on_vm_during_compile");
	test_var_parses_types_and_builds_local_scopes();
	puts("ok: test_var_parses_types_and_builds_local_scopes");
	test_vm_parser_services();
	puts("ok: test_vm_parser_services");
	test_utf8_names_and_unicode_whitespace();
	puts("ok: test_utf8_names_and_unicode_whitespace");
	test_ir_interpreter_handles_empty_source_and_recovers();
	puts("ok: test_ir_interpreter_handles_empty_source_and_recovers");
	puts("frontend tests passed");
	return 0;
}
