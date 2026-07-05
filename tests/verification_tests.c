#include "../verification.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

enum {
	TYPE_INT_ARRAY4_ID = 2,
	TYPE_INT_SLICE_ID = 3,
	TYPE_SLICE_BOX_ID = 4,
	TYPE_SLICE_ARRAY_ID = 5,
	TYPE_PAIR_ID = 6,
	TYPE_NATIVE_CONFLICT_ID = 7,
	TYPE_NATIVE_IO_ID = 8,
};

static TypeField slice_box_fields[] = {
	[0] = {.name = "slice", .tid = TYPE_INT_SLICE_ID},
};

static TypeField pair_fields[] = {
	[0] = {.name = "first", .tid = TYPE_INT_ID},
	[1] = {.name = "second", .tid = TYPE_INT_ID},
};

static SigInput native_conflict_ins[] = {
	{.var = {.name = "mut", .tid = TYPE_INT_ID}, .mut = true},
	{.var = {.name = "shared", .tid = TYPE_INT_ID}, .mut = false},
};

static SigInput native_io_ins[] = {
	{.var = {.name = "in", .tid = TYPE_INT_ID}},
};

static Var native_io_outs[] = {
	{.name = "out", .tid = TYPE_INT_ID},
};

static Type test_types_template[] = {
	[0] = {.kind = TYPE_INT, .name = "int", .payload_size = sizeof(num_t), .align = alignof(num_t)},
	[1] = {.kind = TYPE_BYTE, .name = "byte", .payload_size = 1, .align = 1},
	[TYPE_INT_ARRAY4_ID] = {.kind = TYPE_ARRAY, .name = "int[4]", .data.array = {.elem = TYPE_INT_ID, .capacity = 4}},
	[TYPE_INT_SLICE_ID] = {.kind = TYPE_SLICE, .name = "Slice[int]", .data.ref = {.elem = TYPE_INT_ID}},
	[TYPE_SLICE_BOX_ID] = {.kind = TYPE_STRUCT, .name = "SliceBox", .data.fields = {.data = slice_box_fields, .len = 1}},
	[TYPE_SLICE_ARRAY_ID] = {.kind = TYPE_ARRAY, .name = "Slice[int][2]", .data.array = {.elem = TYPE_INT_SLICE_ID, .capacity = 2}},
	[TYPE_PAIR_ID] = {.kind = TYPE_STRUCT, .name = "Pair", .data.fields = {.data = pair_fields, .len = 2}},
	[TYPE_NATIVE_CONFLICT_ID] = {.kind = TYPE_NATIVE_FUNC_POINTER, .name = "native_conflict", .data.sig = {.ins = {.data = native_conflict_ins, .len = 2}}},
	[TYPE_NATIVE_IO_ID] = {.kind = TYPE_NATIVE_FUNC_POINTER, .name = "native_io", .data.sig = {.ins = {.data = native_io_ins, .len = 1}, .outs = {.data = native_io_outs, .len = 1}}},
};

static Type test_types[sizeof(test_types_template) / sizeof(test_types_template[0])];

static TypeS fresh_types(void){
	memcpy(test_types,test_types_template,sizeof(test_types));
	TypeS types = {.data = test_types,.len = sizeof(test_types) / sizeof(test_types[0])};
	assert(type_layout_all(types));
	return types;
}

static CompileContext make_ctx(Func* funcs,size_t len){
	return (CompileContext){
		.funcs = {.data = funcs,.len = len},
	};
}

typedef struct TypeCheckReport {
	count_t count;
	TypeCheckError last;
} TypeCheckReport;

static TypeCheckReport* active_type_check_report;

static int type_check_reporter(void* user,CompileContext* ctx,const TypeCheckError error){
	(void)user;
	(void)ctx;
	TypeCheckReport* report = active_type_check_report;
	report->count++;
	report->last = error;
	return 1;
}

static void expect_first_pass_error(CompileContext* ctx,TypeCheckErrorKind kind){
	TypeCheckReport report = {0};
	active_type_check_report = &report;
	assert(type_check_first_pass(type_check_reporter,ctx) == 1);
	active_type_check_report = NULL;
	assert(report.count == 1);
	assert(report.last.kind == kind);
	comp_context_free(ctx);
}

static void expect_first_pass_ok(CompileContext* ctx){
	assert(type_check_first_pass(NULL,ctx) == 0);
	comp_context_free(ctx);
}

static void expect_first_pass_rejected(CompileContext* ctx){
	assert(type_check_first_pass(NULL,ctx) == 1);
	comp_context_free(ctx);
}

static void expect_borrow_check(CompileContext* ctx,int expected){
	assert(borrow_check(NULL,ctx) == expected);
	comp_context_free(ctx);
}

static Block one_basic(size_t len){
	return (Block){.kind = BLOCK_BASIC,.data.basic = {.start = 0,.len = len}};
}

static void test_recursive_portal_types(void){
	TypeS types = fresh_types();
	assert(types.data[TYPE_INT_SLICE_ID].is_portal);
	assert(types.data[TYPE_SLICE_BOX_ID].is_portal);
	assert(types.data[TYPE_SLICE_ARRAY_ID].is_portal);
	assert(!types.data[TYPE_INT_ARRAY4_ID].is_portal);
	assert(!types.data[TYPE_PAIR_ID].is_portal);
}

static void test_local_slice_retarget_allowed(void){
	TypeS types = fresh_types();
	Var vars[] = {
		[0] = {.name = "s", .tid = TYPE_INT_SLICE_ID},
		[1] = {.name = "a", .tid = TYPE_INT_ARRAY4_ID},
	};
	OP ops[] = {
		{.kind = OP_PUSH_VAR,.extra = 0},
		{.kind = OP_PUSH_VAR,.extra = 1},
		{.kind = OP_SLICE_FROM_AR},
	};
	Block blocks[] = {one_basic(sizeof(ops) / sizeof(ops[0]))};
	Func funcs[] = {{.name = "caller",.types = types,.blocks = {.data = blocks,.len = 1},.ops = {.data = ops,.len = 3},.vars = {.data = vars,.len = 2}}};
	CompileContext ctx = make_ctx(funcs,1);
	expect_borrow_check(&ctx,0);
}

static void test_local_portal_assign_allowed(void){
	TypeS types = fresh_types();
	Var vars[] = {
		[0] = {.name = "dst", .tid = TYPE_INT_SLICE_ID},
		[1] = {.name = "src", .tid = TYPE_INT_SLICE_ID},
	};
	OP ops[] = {
		{.kind = OP_PUSH_VAR,.extra = 0},
		{.kind = OP_PUSH_VAR,.extra = 1},
		{.kind = OP_ASSIGN},
	};
	Block blocks[] = {one_basic(sizeof(ops) / sizeof(ops[0]))};
	Func funcs[] = {{.name = "caller",.types = types,.blocks = {.data = blocks,.len = 1},.ops = {.data = ops,.len = 3},.vars = {.data = vars,.len = 2}}};
	CompileContext ctx = make_ctx(funcs,1);
	expect_first_pass_ok(&ctx);
}

static void test_non_local_slice_retarget_reported(void){
	TypeS types = fresh_types();
	Var vars[] = {
		[0] = {.name = "s", .tid = TYPE_INT_SLICE_ID},
		[1] = {.name = "a", .tid = TYPE_INT_ARRAY4_ID},
	};
	OP ops[] = {
		{.kind = OP_PUSH_ARG,.extra = 0},
		{.kind = OP_PUSH_VAR,.extra = 1},
		{.kind = OP_SLICE_FROM_AR},
	};
	Block blocks[] = {one_basic(sizeof(ops) / sizeof(ops[0]))};
	Func funcs[] = {{.name = "caller",.types = types,.blocks = {.data = blocks,.len = 1},.ops = {.data = ops,.len = 3},.vars = {.data = vars,.len = 2}}};
	CompileContext ctx = make_ctx(funcs,1);
	expect_first_pass_error(&ctx,TYPE_CHECK_ERROR_PORTAL_NON_LOCAL);
}

static void test_local_struct_containing_portal_assign_allowed(void){
	TypeS types = fresh_types();
	Var vars[] = {
		[0] = {.name = "dst", .tid = TYPE_SLICE_BOX_ID},
		[1] = {.name = "src", .tid = TYPE_SLICE_BOX_ID},
	};
	OP ops[] = {
		{.kind = OP_PUSH_VAR,.extra = 0},
		{.kind = OP_PUSH_VAR,.extra = 1},
		{.kind = OP_ASSIGN},
	};
	Block blocks[] = {one_basic(sizeof(ops) / sizeof(ops[0]))};
	Func funcs[] = {{.name = "caller",.types = types,.blocks = {.data = blocks,.len = 1},.ops = {.data = ops,.len = 3},.vars = {.data = vars,.len = 2}}};
	CompileContext ctx = make_ctx(funcs,1);
	expect_first_pass_ok(&ctx);
}

static void test_portal_backing_scope_reported(void){
	TypeS types = fresh_types();
	Var vars[] = {
		[0] = {.name = "portal", .tid = TYPE_INT_SLICE_ID},
		[1] = {.name = "arr", .tid = TYPE_INT_ARRAY4_ID},
	};
	OP ops[] = {
		{.kind = OP_PUSH_VAR,.extra = 0},
		{.kind = OP_PUSH_VAR,.extra = 1},
		{.kind = OP_SLICE_FROM_AR},
	};
	Block blocks[] = {
		{.kind = BLOCK_VAR,.data.var = {.var = 0,.body = 1}},
		{.kind = BLOCK_VAR,.data.var = {.var = 1,.body = 2}},
		one_basic(sizeof(ops) / sizeof(ops[0])),
	};
	Func funcs[] = {{.name = "caller",.types = types,.blocks = {.data = blocks,.len = 3},.ops = {.data = ops,.len = 3},.vars = {.data = vars,.len = 2}}};
	CompileContext ctx = make_ctx(funcs,1);
	expect_first_pass_error(&ctx,TYPE_CHECK_ERROR_PORTAL_BACKING_SCOPE);
}

static void test_mut_portal_input_rejected(void){
	TypeS types = fresh_types();
	Var vars[] = {{.name = "s", .tid = TYPE_INT_SLICE_ID}};
	SigInput callee_ins[] = {{.var = {.name = "s", .tid = TYPE_INT_SLICE_ID}, .mut = true}};
	OP ops[] = {
		{.kind = OP_PUSH_VAR,.extra = 0},
		{.kind = OP_CALL,.extra = 1},
	};
	Block blocks[] = {one_basic(sizeof(ops) / sizeof(ops[0]))};
	Func funcs[] = {
		{.name = "caller",.types = types,.blocks = {.data = blocks,.len = 1},.ops = {.data = ops,.len = 2},.vars = {.data = vars,.len = 1}},
		{.name = "callee",.types = types,.sig.ins = {.data = callee_ins,.len = 1}},
	};
	CompileContext ctx = make_ctx(funcs,2);
	expect_borrow_check(&ctx,1);
}

static void test_distinct_struct_fields_can_borrow_mut(void){
	TypeS types = fresh_types();
	Var vars[] = {{.name = "p", .tid = TYPE_PAIR_ID}};
	SigInput callee_ins[] = {
		{.var = {.name = "a", .tid = TYPE_INT_ID}, .mut = true},
		{.var = {.name = "b", .tid = TYPE_INT_ID}, .mut = true},
	};
	OP ops[] = {
		{.kind = OP_PUSH_VAR,.extra = 0},
		{.kind = OP_STRUCT_AT,.extra = 0},
		{.kind = OP_PUSH_VAR,.extra = 0},
		{.kind = OP_STRUCT_AT,.extra = 1},
		{.kind = OP_CALL,.extra = 1},
	};
	Block blocks[] = {one_basic(sizeof(ops) / sizeof(ops[0]))};
	Func funcs[] = {
		{.name = "caller",.types = types,.blocks = {.data = blocks,.len = 1},.ops = {.data = ops,.len = 5},.vars = {.data = vars,.len = 1}},
		{.name = "callee",.types = types,.sig.ins = {.data = callee_ins,.len = 2}},
	};
	CompileContext ctx = make_ctx(funcs,2);
	expect_borrow_check(&ctx,0);
}

static void test_struct_parent_conflicts_with_field(void){
	TypeS types = fresh_types();
	Var vars[] = {{.name = "p", .tid = TYPE_PAIR_ID}};
	SigInput callee_ins[] = {
		{.var = {.name = "p", .tid = TYPE_PAIR_ID}, .mut = true},
		{.var = {.name = "a", .tid = TYPE_INT_ID}, .mut = false},
	};
	OP ops[] = {
		{.kind = OP_PUSH_VAR,.extra = 0},
		{.kind = OP_PUSH_VAR,.extra = 0},
		{.kind = OP_STRUCT_AT,.extra = 0},
		{.kind = OP_CALL,.extra = 1},
	};
	Block blocks[] = {one_basic(sizeof(ops) / sizeof(ops[0]))};
	Func funcs[] = {
		{.name = "caller",.types = types,.blocks = {.data = blocks,.len = 1},.ops = {.data = ops,.len = 4},.vars = {.data = vars,.len = 1}},
		{.name = "callee",.types = types,.sig.ins = {.data = callee_ins,.len = 2}},
	};
	CompileContext ctx = make_ctx(funcs,2);
	expect_borrow_check(&ctx,1);
}

static void test_native_signature_borrow_checked(void){
	TypeS types = fresh_types();
	Var vars[] = {{.name = "x", .tid = TYPE_INT_ID}};
	Global globals[] = {{.var = {.name = "native", .tid = TYPE_NATIVE_CONFLICT_ID}}};
	OP ops[] = {
		{.kind = OP_PUSH_VAR,.extra = 0},
		{.kind = OP_PUSH_VAR,.extra = 0},
		{.kind = OP_PUSH_GLOBAL,.extra = 0},
		{.kind = OP_CALL_NATIVE_ON_STACK},
	};
	Block blocks[] = {one_basic(sizeof(ops) / sizeof(ops[0]))};
	Func funcs[] = {{.name = "caller",.types = types,.blocks = {.data = blocks,.len = 1},.ops = {.data = ops,.len = 4},.vars = {.data = vars,.len = 1}}};
	CompileContext ctx = make_ctx(funcs,1);
	ctx.globals = (GlobalS){.data = globals,.len = 1,.cap = 1};
	expect_borrow_check(&ctx,1);
}

static void test_native_output_feeds_function_borrow_checked(void){
	TypeS types = fresh_types();
	Var vars[] = {
		{.name = "y", .tid = TYPE_INT_ID},
		{.name = "x", .tid = TYPE_INT_ID},
	};
	SigInput callee_ins[] = {{.var = {.name = "y", .tid = TYPE_INT_ID}, .mut = true}};
	Global globals[] = {{.var = {.name = "native", .tid = TYPE_NATIVE_IO_ID}}};
	OP ops[] = {
		{.kind = OP_PUSH_VAR,.extra = 0},
		{.kind = OP_PUSH_VAR,.extra = 1},
		{.kind = OP_PUSH_GLOBAL,.extra = 0},
		{.kind = OP_CALL_NATIVE_ON_STACK},
		{.kind = OP_CALL,.extra = 1},
	};
	Block blocks[] = {one_basic(sizeof(ops) / sizeof(ops[0]))};
	Func funcs[] = {
		{.name = "caller",.types = types,.blocks = {.data = blocks,.len = 1},.ops = {.data = ops,.len = 5},.vars = {.data = vars,.len = 2}},
		{.name = "callee",.types = types,.sig.ins = {.data = callee_ins,.len = 1}},
	};
	CompileContext ctx = make_ctx(funcs,2);
	ctx.globals = (GlobalS){.data = globals,.len = 1,.cap = 1};
	expect_borrow_check(&ctx,0);
}

static void test_call_type_mismatch_rejected_in_first_pass(void){
	TypeS types = fresh_types();
	Var vars[] = {{.name = "s", .tid = TYPE_INT_SLICE_ID}};
	SigInput callee_ins[] = {{.var = {.name = "x", .tid = TYPE_INT_ID}}};
	OP ops[] = {
		{.kind = OP_PUSH_VAR,.extra = 0},
		{.kind = OP_CALL,.extra = 1},
	};
	Block blocks[] = {one_basic(sizeof(ops) / sizeof(ops[0]))};
	Func funcs[] = {
		{.name = "caller",.types = types,.blocks = {.data = blocks,.len = 1},.ops = {.data = ops,.len = 2},.vars = {.data = vars,.len = 1}},
		{.name = "callee",.types = types,.sig.ins = {.data = callee_ins,.len = 1}},
	};
	CompileContext ctx = make_ctx(funcs,2);
	expect_first_pass_error(&ctx,TYPE_CHECK_ERROR_INVALID_CALL);
}

static void test_bad_index_position_rejected_in_first_pass(void){
	TypeS types = fresh_types();
	Var vars[] = {
		{.name = "a", .tid = TYPE_INT_ARRAY4_ID},
		{.name = "s", .tid = TYPE_INT_SLICE_ID},
	};
	OP ops[] = {
		{.kind = OP_PUSH_VAR,.extra = 0},
		{.kind = OP_PUSH_VAR,.extra = 1},
		{.kind = OP_ARR_AT},
	};
	Block blocks[] = {one_basic(sizeof(ops) / sizeof(ops[0]))};
	Func funcs[] = {{.name = "caller",.types = types,.blocks = {.data = blocks,.len = 1},.ops = {.data = ops,.len = 3},.vars = {.data = vars,.len = 2}}};
	CompileContext ctx = make_ctx(funcs,1);
	expect_first_pass_error(&ctx,TYPE_CHECK_ERROR_INVALID_INDEX);
}

static void test_struct_field_out_of_range_rejected_in_first_pass(void){
	TypeS types = fresh_types();
	Var vars[] = {{.name = "p", .tid = TYPE_PAIR_ID}};
	OP ops[] = {
		{.kind = OP_PUSH_VAR,.extra = 0},
		{.kind = OP_STRUCT_AT,.extra = 2},
	};
	Block blocks[] = {one_basic(sizeof(ops) / sizeof(ops[0]))};
	Func funcs[] = {{.name = "caller",.types = types,.blocks = {.data = blocks,.len = 1},.ops = {.data = ops,.len = 2},.vars = {.data = vars,.len = 1}}};
	CompileContext ctx = make_ctx(funcs,1);
	expect_first_pass_error(&ctx,TYPE_CHECK_ERROR_INVALID_FIELD);
}

static void test_native_call_type_mismatch_rejected_in_first_pass(void){
	TypeS types = fresh_types();
	Var vars[] = {
		{.name = "s", .tid = TYPE_INT_SLICE_ID},
		{.name = "x", .tid = TYPE_INT_ID},
	};
	Global globals[] = {{.var = {.name = "native", .tid = TYPE_NATIVE_CONFLICT_ID}}};
	OP ops[] = {
		{.kind = OP_PUSH_VAR,.extra = 0},
		{.kind = OP_PUSH_VAR,.extra = 1},
		{.kind = OP_PUSH_GLOBAL,.extra = 0},
		{.kind = OP_CALL_NATIVE_ON_STACK},
	};
	Block blocks[] = {one_basic(sizeof(ops) / sizeof(ops[0]))};
	Func funcs[] = {{.name = "caller",.types = types,.blocks = {.data = blocks,.len = 1},.ops = {.data = ops,.len = 4},.vars = {.data = vars,.len = 2}}};
	CompileContext ctx = make_ctx(funcs,1);
	ctx.globals = (GlobalS){.data = globals,.len = 1,.cap = 1};
	expect_first_pass_error(&ctx,TYPE_CHECK_ERROR_INVALID_CALL);
}

static void test_slice_from_wrong_array_type_rejected_in_first_pass(void){
	TypeS types = fresh_types();
	Var vars[] = {
		{.name = "s", .tid = TYPE_INT_SLICE_ID},
		{.name = "arrays", .tid = TYPE_SLICE_ARRAY_ID},
	};
	OP ops[] = {
		{.kind = OP_PUSH_VAR,.extra = 0},
		{.kind = OP_PUSH_VAR,.extra = 1},
		{.kind = OP_SLICE_FROM_AR},
	};
	Block blocks[] = {one_basic(sizeof(ops) / sizeof(ops[0]))};
	Func funcs[] = {{.name = "caller",.types = types,.blocks = {.data = blocks,.len = 1},.ops = {.data = ops,.len = 3},.vars = {.data = vars,.len = 2}}};
	CompileContext ctx = make_ctx(funcs,1);
	expect_first_pass_error(&ctx,TYPE_CHECK_ERROR_TYPE_MISMATCH);
}

static void test_stack_underflow_rejected_in_first_pass(void){
	TypeS types = fresh_types();
	OP ops[] = {{.kind = OP_ASSIGN}};
	Block blocks[] = {one_basic(sizeof(ops) / sizeof(ops[0]))};
	Func funcs[] = {{.name = "caller",.types = types,.blocks = {.data = blocks,.len = 1},.ops = {.data = ops,.len = 1}}};
	CompileContext ctx = make_ctx(funcs,1);
	expect_first_pass_error(&ctx,TYPE_CHECK_ERROR_STACK_UNDERFLOW);
}

static void test_branch_condition_must_leave_int(void){
	TypeS types = fresh_types();
	Var vars[] = {{.name = "x", .tid = TYPE_BYTE_ID}};
	OP ops[] = {{.kind = OP_PUSH_VAR,.extra = 0}};
	Block blocks[] = {
		{.kind = BLOCK_BRANCH,.data.branch = {.cond = {.start = 0,.len = 1},.yes = 1,.no = 2}},
		{.kind = BLOCK_HARD_CRASH},
		{.kind = BLOCK_HARD_CRASH},
	};
	Func funcs[] = {{.name = "caller",.types = types,.blocks = {.data = blocks,.len = 3},.ops = {.data = ops,.len = 1},.vars = {.data = vars,.len = 1}}};
	CompileContext ctx = make_ctx(funcs,1);
	expect_first_pass_rejected(&ctx);
}

static void test_invalid_local_rejected_in_first_pass(void){
	TypeS types = fresh_types();
	OP ops[] = {{.kind = OP_PUSH_VAR,.extra = 0}};
	Block blocks[] = {one_basic(sizeof(ops) / sizeof(ops[0]))};
	Func funcs[] = {{.name = "caller",.types = types,.blocks = {.data = blocks,.len = 1},.ops = {.data = ops,.len = 1}}};
	CompileContext ctx = make_ctx(funcs,1);
	expect_first_pass_error(&ctx,TYPE_CHECK_ERROR_INVALID_LOCAL);
}

static void test_invalid_global_rejected_in_first_pass(void){
	TypeS types = fresh_types();
	OP ops[] = {{.kind = OP_PUSH_GLOBAL,.extra = 0}};
	Block blocks[] = {one_basic(sizeof(ops) / sizeof(ops[0]))};
	Func funcs[] = {{.name = "caller",.types = types,.blocks = {.data = blocks,.len = 1},.ops = {.data = ops,.len = 1}}};
	CompileContext ctx = make_ctx(funcs,1);
	expect_first_pass_error(&ctx,TYPE_CHECK_ERROR_INVALID_GLOBAL);
}

static void test_invalid_func_rejected_in_first_pass(void){
	TypeS types = fresh_types();
	OP ops[] = {{.kind = OP_CALL,.extra = 1}};
	Block blocks[] = {one_basic(sizeof(ops) / sizeof(ops[0]))};
	Func funcs[] = {{.name = "caller",.types = types,.blocks = {.data = blocks,.len = 1},.ops = {.data = ops,.len = 1}}};
	CompileContext ctx = make_ctx(funcs,1);
	expect_first_pass_error(&ctx,TYPE_CHECK_ERROR_INVALID_FUNC);
}

int main(void){
	test_recursive_portal_types();
	puts("ok: test_recursive_portal_types");
	test_local_slice_retarget_allowed();
	puts("ok: test_local_slice_retarget_allowed");
	test_local_portal_assign_allowed();
	puts("ok: test_local_portal_assign_allowed");
	test_non_local_slice_retarget_reported();
	puts("ok: test_non_local_slice_retarget_reported");
	test_local_struct_containing_portal_assign_allowed();
	puts("ok: test_local_struct_containing_portal_assign_allowed");
	test_portal_backing_scope_reported();
	puts("ok: test_portal_backing_scope_reported");
	test_mut_portal_input_rejected();
	puts("ok: test_mut_portal_input_rejected");
	test_distinct_struct_fields_can_borrow_mut();
	puts("ok: test_distinct_struct_fields_can_borrow_mut");
	test_struct_parent_conflicts_with_field();
	puts("ok: test_struct_parent_conflicts_with_field");
	test_native_signature_borrow_checked();
	puts("ok: test_native_signature_borrow_checked");
	test_native_output_feeds_function_borrow_checked();
	puts("ok: test_native_output_feeds_function_borrow_checked");
	test_call_type_mismatch_rejected_in_first_pass();
	puts("ok: test_call_type_mismatch_rejected_in_first_pass");
	test_bad_index_position_rejected_in_first_pass();
	puts("ok: test_bad_index_position_rejected_in_first_pass");
	test_struct_field_out_of_range_rejected_in_first_pass();
	puts("ok: test_struct_field_out_of_range_rejected_in_first_pass");
	test_native_call_type_mismatch_rejected_in_first_pass();
	puts("ok: test_native_call_type_mismatch_rejected_in_first_pass");
	test_slice_from_wrong_array_type_rejected_in_first_pass();
	puts("ok: test_slice_from_wrong_array_type_rejected_in_first_pass");
	test_stack_underflow_rejected_in_first_pass();
	puts("ok: test_stack_underflow_rejected_in_first_pass");
	test_branch_condition_must_leave_int();
	puts("ok: test_branch_condition_must_leave_int");
	test_invalid_local_rejected_in_first_pass();
	puts("ok: test_invalid_local_rejected_in_first_pass");
	test_invalid_global_rejected_in_first_pass();
	puts("ok: test_invalid_global_rejected_in_first_pass");
	test_invalid_func_rejected_in_first_pass();
	puts("ok: test_invalid_func_rejected_in_first_pass");
	puts("all verification tests passed");
	return 0;
}
