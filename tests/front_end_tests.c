#include "../front_end.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

enum {
	TYPE_INT_ARRAY4_ID = 2,
	TYPE_INT_SLICE_ID = 3,
	TYPE_SLICE_BOX_ID = 4,
	TYPE_SLICE_ARRAY_ID = 5,
	TYPE_PAIR_ID = 6,
};

static TypeField slice_box_fields[] = {
	[0] = {.name = "slice", .tid = TYPE_INT_SLICE_ID},
};

static TypeField pair_fields[] = {
	[0] = {.name = "first", .tid = TYPE_INT_ID},
	[1] = {.name = "second", .tid = TYPE_INT_ID},
};

static Type test_types_template[] = {
	[0] = {.kind = TYPE_INT, .name = "int", .payload_size = sizeof(num_t), .align = alignof(num_t)},
	[1] = {.kind = TYPE_BYTE, .name = "byte", .payload_size = 1, .align = 1},
	[TYPE_INT_ARRAY4_ID] = {.kind = TYPE_ARRAY, .name = "int[4]", .data.array = {.elem = TYPE_INT_ID, .capacity = 4}},
	[TYPE_INT_SLICE_ID] = {.kind = TYPE_SLICE, .name = "Slice[int]", .data.ref = {.elem = TYPE_INT_ID}},
	[TYPE_SLICE_BOX_ID] = {.kind = TYPE_STRUCT, .name = "SliceBox", .data.fields = {.data = slice_box_fields, .len = 1}},
	[TYPE_SLICE_ARRAY_ID] = {.kind = TYPE_ARRAY, .name = "Slice[int][2]", .data.array = {.elem = TYPE_INT_SLICE_ID, .capacity = 2}},
	[TYPE_PAIR_ID] = {.kind = TYPE_STRUCT, .name = "Pair", .data.fields = {.data = pair_fields, .len = 2}},
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

typedef struct PortalReport {
	count_t count;
	PortalError last;
} PortalReport;

static PortalReport* active_portal_report;

static int portal_reporter(void* user,CompileContext* ctx,const PortalError error){
	(void)user;
	(void)ctx;
	PortalReport* report = active_portal_report;
	report->count++;
	report->last = error;
	return 1;
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
	assert(borrow_check(NULL,&ctx) == 0);
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
	assert(gather_portal_regions(NULL,&ctx) == 0);
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
	PortalReport report = {0};
	active_portal_report = &report;
	assert(gather_portal_regions(portal_reporter,&ctx) == 1);
	active_portal_report = NULL;
	assert(report.count == 1);
	assert(report.last.kind == PORTAL_ERROR_NON_LOCAL);
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
	assert(gather_portal_regions(NULL,&ctx) == 0);
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
	PortalReport report = {0};
	active_portal_report = &report;
	assert(gather_portal_regions(portal_reporter,&ctx) == 1);
	active_portal_report = NULL;
	assert(report.count == 1);
	assert(report.last.kind == PORTAL_ERROR_BACKING_SCOPE);
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
	assert(borrow_check(NULL,&ctx) == 1);
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
	assert(borrow_check(NULL,&ctx) == 0);
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
	assert(borrow_check(NULL,&ctx) == 1);
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
	puts("all frontend tests passed");
	return 0;
}
