#include <string.h>
#include "front_end.h"

static bool par_eq(Par a, Par b){
	return a.kind == b.kind && a.idx == b.idx && a.parent == b.parent;
}

static size_t par_hash(Par par){
	size_t h = 1469598103934665603ull;
	h = (h ^ (size_t)par.kind) * 1099511628211ull;
	h = (h ^ (size_t)par.idx) * 1099511628211ull;
	h = (h ^ (size_t)par.parent) * 1099511628211ull;
	return h;
}

void par_idx_table_free(ParIdxTable* table){
	free(table->keys);
	free(table->vals);
	free(table->used);
	*table = (ParIdxTable){0};
}

void par_idx_table_reset(ParIdxTable* table){
	table->len = 0;
	if(table->used) memset(table->used, 0, table->cap);
}

static void par_idx_table_grow(ParIdxTable* table){
	ParIdxTable old = *table;
	size_t cap = old.cap ? old.cap * 2 : 16;
	table->keys = calloc(cap, sizeof(*table->keys));
	table->vals = calloc(cap, sizeof(*table->vals));
	table->used = calloc(cap, sizeof(*table->used));
	assert(table->keys && table->vals && table->used);
	table->len = 0;
	table->cap = cap;

	for(size_t i = 0; i < old.cap; i++){
		if(!old.used[i]) continue;
		size_t j = par_hash(old.keys[i]) & (table->cap - 1);
		while(table->used[j]) j = (j + 1) & (table->cap - 1);
		table->keys[j] = old.keys[i];
		table->vals[j] = old.vals[i];
		table->used[j] = 1;
		table->len++;
	}

	free(old.keys);
	free(old.vals);
	free(old.used);
}

bool par_idx_table_get(const ParIdxTable* table, Par key, par_idx* out){
	if(!table->cap) return false;
	size_t i = par_hash(key) & (table->cap - 1);
	while(table->used[i]){
		if(par_eq(table->keys[i], key)){
			if(out) *out = table->vals[i];
			return true;
		}
		i = (i + 1) & (table->cap - 1);
	}
	return false;
}

void par_idx_table_put(ParIdxTable* table, Par key, par_idx val){
	if((table->len + 1) * 4 >= table->cap * 3) par_idx_table_grow(table);
	size_t i = par_hash(key) & (table->cap - 1);
	while(table->used[i]){
		if(par_eq(table->keys[i], key)){
			table->vals[i] = val;
			return;
		}
		i = (i + 1) & (table->cap - 1);
	}
	table->keys[i] = key;
	table->vals[i] = val;
	table->used[i] = 1;
	table->len++;
}

void comp_context_reset(CompileContext* ctx){
	ctx->handles.len = 0;
	ctx->holds.len = 0;
	ctx->pars.len = 0;
	par_idx_table_reset(&ctx->par_idxs);
}

par_idx comp_context_intern_par(CompileContext* ctx, Par par){
	par_idx idx = PAR_IDX_INVALID;
	if(par_idx_table_get(&ctx->par_idxs, par, &idx)) return idx;
	assert(ctx->handles.len == (size_t)(par_idx)ctx->handles.len);
	idx = (par_idx)ctx->handles.len;
	PUSH_HEAP(ctx->handles, ((Handle){.par = par,.life = LIFE_FREE}));
	par_idx_table_put(&ctx->par_idxs, par, idx);
	return idx;
}

typedef struct ScanState {
	count_t scope_depth;
	count_t* var_depths;
} ScanState;

typedef int (*ScanOp)(void* user,CompileContext* ctx,const ScanState* state,func_idx current,OP op,CodeLoc loc);

static bool pars_push(CompileContext* ctx,par_idx par){
	PUSH_HEAP(ctx->pars, par);
	return true;
}

static bool pars_pop(CompileContext* ctx,count_t n){
	if(ctx->pars.len < n) return false;
	ctx->pars.len -= n;
	return true;
}

static type_idx par_type(CompileContext* ctx,const Func* func,par_idx idx){
	if(idx >= ctx->handles.len) return TYPE_INVALID_ID;
	Par par = ctx->handles.data[idx].par;
	switch(par.kind){
	case PAR_LOCAL:
	case PAR_ARG:
		if(par.idx >= func->vars.len) return TYPE_INVALID_ID;
		return func->vars.data[par.idx].tid;
	case PAR_GLOBAL:
		if(par.idx >= ctx->globals.len) return TYPE_INVALID_ID;
		return ctx->globals.data[par.idx].var.tid;
	case PAR_STRUCT_FILED: {
		type_idx parent_tid = par_type(ctx,func,par.parent);
		if(!type_idx_valid(func->types,parent_tid)) return TYPE_INVALID_ID;
		Type parent = func->types.data[parent_tid];
		if(parent.kind != TYPE_STRUCT || par.idx >= parent.data.fields.len) return TYPE_INVALID_ID;
		return parent.data.fields.data[par.idx].tid;
	}
	case PAR_ARR_MEMBER: {
		type_idx parent_tid = par_type(ctx,func,par.parent);
		if(!type_idx_valid(func->types,parent_tid)) return TYPE_INVALID_ID;
		Type parent = func->types.data[parent_tid];
		if(parent.kind == TYPE_ARRAY) return parent.data.array.elem;
		if(parent.kind == TYPE_SLICE || parent.kind == TYPE_VIEW) return parent.data.ref.elem;
		return TYPE_INVALID_ID;
	}
	}
	return TYPE_INVALID_ID;
}

static bool par_root_is_local(CompileContext* ctx,par_idx idx){
	if(idx >= ctx->handles.len) return false;
	Par par = ctx->handles.data[idx].par;
	while(par.kind == PAR_STRUCT_FILED || par.kind == PAR_ARR_MEMBER){
		idx = par.parent;
		if(idx >= ctx->handles.len) return false;
		par = ctx->handles.data[idx].par;
	}
	return par.kind == PAR_LOCAL;
}

static bool par_root(CompileContext* ctx,par_idx idx,Par* out){
	if(idx >= ctx->handles.len) return false;
	Par par = ctx->handles.data[idx].par;
	while(par.kind == PAR_STRUCT_FILED || par.kind == PAR_ARR_MEMBER){
		idx = par.parent;
		if(idx >= ctx->handles.len) return false;
		par = ctx->handles.data[idx].par;
	}
	*out = par;
	return true;
}

static count_t par_scope_depth(CompileContext* ctx,const Func* func,const ScanState* state,par_idx idx){
	(void)func;
	Par root;
	if(!par_root(ctx,idx,&root)) return 0;
	if(root.kind == PAR_LOCAL && state && root.idx < func->vars.len) return state->var_depths[root.idx];
	return 0;
}

static bool par_is_portal(CompileContext* ctx,const Func* func,par_idx idx){
	type_idx tid = par_type(ctx,func,idx);
	return type_idx_valid(func->types,tid) && func->types.data[tid].is_portal;
}

static bool par_has_type(CompileContext* ctx,const Func* func,par_idx idx,type_idx want){
	type_idx got = par_type(ctx,func,idx);
	return type_idx_valid(func->types,got) && got == want;
}

static bool par_is_int(CompileContext* ctx,const Func* func,par_idx idx){
	return par_has_type(ctx,func,idx,TYPE_INT_ID);
}

static bool type_is_array_of(TypeS types,type_idx tid,type_idx elem){
	return type_idx_valid(types,tid)
		&& types.data[tid].kind == TYPE_ARRAY
		&& types.data[tid].data.array.elem == elem;
}

static const Sig* native_sig_on_stack(CompileContext* ctx,const Func* func){
	if(ctx->pars.len < 1) return NULL;
	type_idx tid = par_type(ctx,func,TOP(ctx->pars));
	if(!type_idx_valid(func->types,tid)) return NULL;
	Type* type = &func->types.data[tid];
	if(type->kind != TYPE_NATIVE_FUNC_POINTER) return NULL;
	return &type->data.sig;
}

typedef struct TypeCheckCtx {
	TypeCheckErrorReporter reporter;
	void* user;
} TypeCheckCtx;

static int report_type_check(TypeCheckCtx* t,CompileContext* ctx,TypeCheckError error){
	if(!t->reporter) return 1;
	return t->reporter(t->user,ctx,error);
}

static int report_stack_underflow(TypeCheckCtx* t,CompileContext* ctx,OP op,CodeLoc loc,count_t needed,count_t actual){
	return report_type_check(t,ctx,(TypeCheckError){
		.kind = TYPE_CHECK_ERROR_STACK_UNDERFLOW,
		.loc = loc,
		.op = op,
		.data.stack = {.needed = needed,.actual = actual},
	});
}

static int report_index_error(TypeCheckCtx* t,CompileContext* ctx,TypeCheckErrorKind kind,OP op,CodeLoc loc,count_t idx,count_t len){
	return report_type_check(t,ctx,(TypeCheckError){
		.kind = kind,
		.loc = loc,
		.op = op,
		.data.index = {.idx = idx,.len = len},
	});
}

static int report_type_error(TypeCheckCtx* t,CompileContext* ctx,TypeCheckErrorKind kind,OP op,CodeLoc loc,par_idx par,type_idx expected,type_idx actual){
	return report_type_check(t,ctx,(TypeCheckError){
		.kind = kind,
		.loc = loc,
		.op = op,
		.data.type = {.par = par,.expected = expected,.actual = actual},
	});
}

static int type_check_sig(TypeCheckCtx* t,CompileContext* ctx,const Func* caller,const Sig* sig,size_t base,OP op,CodeLoc loc){
	if(ctx->pars.len < base + sig->outs.len + sig->ins.len) return report_stack_underflow(t,ctx,op,loc,(count_t)(base + sig->outs.len + sig->ins.len),(count_t)ctx->pars.len);

	for(count_t i = 0;i < sig->outs.len;i++){
		type_idx want = sig->outs.data[i].tid;
		par_idx par = ctx->pars.data[base + i];
		type_idx got = par_type(ctx,caller,par);
		if(!type_idx_valid(caller->types,want)) return report_type_error(t,ctx,TYPE_CHECK_ERROR_INVALID_TYPE,op,loc,par,want,got);
		if(got != want) return report_type_error(t,ctx,TYPE_CHECK_ERROR_INVALID_CALL,op,loc,par,want,got);
	}
	for(count_t i = 0;i < sig->ins.len;i++){
		type_idx want = sig->ins.data[i].var.tid;
		par_idx par = ctx->pars.data[base + sig->outs.len + i];
		type_idx got = par_type(ctx,caller,par);
		if(!type_idx_valid(caller->types,want)) return report_type_error(t,ctx,TYPE_CHECK_ERROR_INVALID_TYPE,op,loc,par,want,got);
		if(got != want) return report_type_error(t,ctx,TYPE_CHECK_ERROR_INVALID_CALL,op,loc,par,want,got);
	}
	return 0;
}

static int type_check_op(TypeCheckCtx* t,CompileContext* ctx,const Func* func,OP op,CodeLoc loc){
	switch(op.kind){
	case OP_NULL:
	case OP_DROP:
		return 0;
	case OP_PUSH_VAR:
	case OP_PUSH_ARG:
		if(op.extra >= func->vars.len) return report_index_error(t,ctx,TYPE_CHECK_ERROR_INVALID_LOCAL,op,loc,op.extra,(count_t)func->vars.len);
		if(!type_idx_valid(func->types,func->vars.data[op.extra].tid)) return report_type_error(t,ctx,TYPE_CHECK_ERROR_INVALID_TYPE,op,loc,PAR_IDX_INVALID,func->vars.data[op.extra].tid,TYPE_INVALID_ID);
		return 0;
	case OP_PUSH_GLOBAL:
		if(op.extra >= ctx->globals.len) return report_index_error(t,ctx,TYPE_CHECK_ERROR_INVALID_GLOBAL,op,loc,op.extra,(count_t)ctx->globals.len);
		if(!type_idx_valid(func->types,ctx->globals.data[op.extra].var.tid)) return report_type_error(t,ctx,TYPE_CHECK_ERROR_INVALID_TYPE,op,loc,PAR_IDX_INVALID,ctx->globals.data[op.extra].var.tid,TYPE_INVALID_ID);
		return 0;
	case OP_ASSIGN:
	case OP_ADD_ASSIGN:
	case OP_SUB_ASSIGN:
	case OP_MUL_ASSIGN:
	case OP_DIV_ASSIGN:
	case OP_AND_ASSIGN:
	case OP_OR_ASSIGN:
	case OP_XOR_ASSIGN: {
		if(ctx->pars.len < 2) return report_stack_underflow(t,ctx,op,loc,2,(count_t)ctx->pars.len);
		par_idx dst = ctx->pars.data[ctx->pars.len - 2];
		par_idx src = ctx->pars.data[ctx->pars.len - 1];
		type_idx dst_tid = par_type(ctx,func,dst);
		type_idx src_tid = par_type(ctx,func,src);
		if(!type_idx_valid(func->types,dst_tid)) return report_type_error(t,ctx,TYPE_CHECK_ERROR_INVALID_TYPE,op,loc,dst,dst_tid,dst_tid);
		if(dst_tid != src_tid) return report_type_error(t,ctx,TYPE_CHECK_ERROR_TYPE_MISMATCH,op,loc,src,dst_tid,src_tid);
		if(op.kind != OP_ASSIGN && dst_tid != TYPE_INT_ID) return report_type_error(t,ctx,TYPE_CHECK_ERROR_TYPE_MISMATCH,op,loc,dst,TYPE_INT_ID,dst_tid);
		return 0;
	}
	case OP_BIT_NOT_ASSIGN:
		if(ctx->pars.len < 1) return report_stack_underflow(t,ctx,op,loc,1,(count_t)ctx->pars.len);
		return par_is_int(ctx,func,TOP(ctx->pars)) ? 0 : report_type_error(t,ctx,TYPE_CHECK_ERROR_TYPE_MISMATCH,op,loc,TOP(ctx->pars),TYPE_INT_ID,par_type(ctx,func,TOP(ctx->pars)));
	case OP_ARR_PUSH: {
		if(ctx->pars.len < 2) return report_stack_underflow(t,ctx,op,loc,2,(count_t)ctx->pars.len);
		par_idx arr = ctx->pars.data[ctx->pars.len - 2];
		par_idx elem = ctx->pars.data[ctx->pars.len - 1];
		type_idx arr_tid = par_type(ctx,func,ctx->pars.data[ctx->pars.len - 2]);
		type_idx elem_tid = par_type(ctx,func,ctx->pars.data[ctx->pars.len - 1]);
		if(!type_idx_valid(func->types,arr_tid) || func->types.data[arr_tid].kind != TYPE_ARRAY) return report_type_error(t,ctx,TYPE_CHECK_ERROR_INVALID_INDEX,op,loc,arr,TYPE_INVALID_ID,arr_tid);
		return func->types.data[arr_tid].data.array.elem == elem_tid ? 0 : report_type_error(t,ctx,TYPE_CHECK_ERROR_TYPE_MISMATCH,op,loc,elem,func->types.data[arr_tid].data.array.elem,elem_tid);
	}
	case OP_ARR_DROP: {
		if(ctx->pars.len < 2) return report_stack_underflow(t,ctx,op,loc,2,(count_t)ctx->pars.len);
		if(!par_is_int(ctx,func,ctx->pars.data[ctx->pars.len - 1])) return report_type_error(t,ctx,TYPE_CHECK_ERROR_TYPE_MISMATCH,op,loc,ctx->pars.data[ctx->pars.len - 1],TYPE_INT_ID,par_type(ctx,func,ctx->pars.data[ctx->pars.len - 1]));
		type_idx arr_tid = par_type(ctx,func,ctx->pars.data[ctx->pars.len - 2]);
		return type_idx_valid(func->types,arr_tid) && func->types.data[arr_tid].kind == TYPE_ARRAY ? 0 : report_type_error(t,ctx,TYPE_CHECK_ERROR_INVALID_INDEX,op,loc,ctx->pars.data[ctx->pars.len - 2],TYPE_INVALID_ID,arr_tid);
	}
	case OP_SLICE_FROM_AR: {
		if(ctx->pars.len < 2) return report_stack_underflow(t,ctx,op,loc,2,(count_t)ctx->pars.len);
		type_idx ref_tid = par_type(ctx,func,ctx->pars.data[ctx->pars.len - 2]);
		type_idx arr_tid = par_type(ctx,func,ctx->pars.data[ctx->pars.len - 1]);
		if(!type_idx_valid(func->types,ref_tid)) return report_type_error(t,ctx,TYPE_CHECK_ERROR_INVALID_TYPE,op,loc,ctx->pars.data[ctx->pars.len - 2],ref_tid,ref_tid);
		if(!type_idx_valid(func->types,arr_tid)) return report_type_error(t,ctx,TYPE_CHECK_ERROR_INVALID_TYPE,op,loc,ctx->pars.data[ctx->pars.len - 1],arr_tid,arr_tid);
		if(func->types.data[ref_tid].kind != TYPE_SLICE && func->types.data[ref_tid].kind != TYPE_VIEW) return report_type_error(t,ctx,TYPE_CHECK_ERROR_TYPE_MISMATCH,op,loc,ctx->pars.data[ctx->pars.len - 2],TYPE_INVALID_ID,ref_tid);
		return type_is_array_of(func->types,arr_tid,func->types.data[ref_tid].data.ref.elem) ? 0 : report_type_error(t,ctx,TYPE_CHECK_ERROR_TYPE_MISMATCH,op,loc,ctx->pars.data[ctx->pars.len - 1],func->types.data[ref_tid].data.ref.elem,arr_tid);
	}
	case OP_SLICE_INC:
	case OP_SLICE_DEC:
		if(ctx->pars.len < 2) return report_stack_underflow(t,ctx,op,loc,2,(count_t)ctx->pars.len);
		if(!par_is_int(ctx,func,TOP(ctx->pars))) return report_type_error(t,ctx,TYPE_CHECK_ERROR_TYPE_MISMATCH,op,loc,TOP(ctx->pars),TYPE_INT_ID,par_type(ctx,func,TOP(ctx->pars)));
		{
			type_idx ref_tid = par_type(ctx,func,ctx->pars.data[ctx->pars.len - 2]);
			return type_idx_valid(func->types,ref_tid)
				&& func->types.data[ref_tid].kind == TYPE_SLICE ? 0 : report_type_error(t,ctx,TYPE_CHECK_ERROR_TYPE_MISMATCH,op,loc,ctx->pars.data[ctx->pars.len - 2],TYPE_INVALID_ID,ref_tid);
		}
	case OP_ARR_AT: {
		if(ctx->pars.len < 2) return report_stack_underflow(t,ctx,op,loc,2,(count_t)ctx->pars.len);
		if(!par_is_int(ctx,func,ctx->pars.data[ctx->pars.len - 1])) return report_type_error(t,ctx,TYPE_CHECK_ERROR_INVALID_INDEX,op,loc,ctx->pars.data[ctx->pars.len - 1],TYPE_INT_ID,par_type(ctx,func,ctx->pars.data[ctx->pars.len - 1]));
		type_idx arr_tid = par_type(ctx,func,ctx->pars.data[ctx->pars.len - 2]);
		return type_idx_valid(func->types,arr_tid) && func->types.data[arr_tid].kind == TYPE_ARRAY ? 0 : report_type_error(t,ctx,TYPE_CHECK_ERROR_INVALID_INDEX,op,loc,ctx->pars.data[ctx->pars.len - 2],TYPE_INVALID_ID,arr_tid);
	}
	case OP_SLICE_AT: {
		if(ctx->pars.len < 2) return report_stack_underflow(t,ctx,op,loc,2,(count_t)ctx->pars.len);
		if(!par_is_int(ctx,func,ctx->pars.data[ctx->pars.len - 1])) return report_type_error(t,ctx,TYPE_CHECK_ERROR_INVALID_INDEX,op,loc,ctx->pars.data[ctx->pars.len - 1],TYPE_INT_ID,par_type(ctx,func,ctx->pars.data[ctx->pars.len - 1]));
		type_idx ref_tid = par_type(ctx,func,ctx->pars.data[ctx->pars.len - 2]);
		return type_idx_valid(func->types,ref_tid)
			&& (func->types.data[ref_tid].kind == TYPE_SLICE || func->types.data[ref_tid].kind == TYPE_VIEW) ? 0 : report_type_error(t,ctx,TYPE_CHECK_ERROR_INVALID_INDEX,op,loc,ctx->pars.data[ctx->pars.len - 2],TYPE_INVALID_ID,ref_tid);
	}
	case OP_STRUCT_AT: {
		if(ctx->pars.len < 1) return report_stack_underflow(t,ctx,op,loc,1,(count_t)ctx->pars.len);
		type_idx parent_tid = par_type(ctx,func,TOP(ctx->pars));
		if(!type_idx_valid(func->types,parent_tid) || func->types.data[parent_tid].kind != TYPE_STRUCT) return report_type_error(t,ctx,TYPE_CHECK_ERROR_TYPE_MISMATCH,op,loc,TOP(ctx->pars),TYPE_INVALID_ID,parent_tid);
		return op.extra < func->types.data[parent_tid].data.fields.len ? 0 : report_index_error(t,ctx,TYPE_CHECK_ERROR_INVALID_FIELD,op,loc,op.extra,(count_t)func->types.data[parent_tid].data.fields.len);
	}
	case OP_CALL: {
		if(op.extra >= ctx->funcs.len) return report_index_error(t,ctx,TYPE_CHECK_ERROR_INVALID_FUNC,op,loc,op.extra,(count_t)ctx->funcs.len);
		Func* callee = &ctx->funcs.data[op.extra];
		count_t argc = (count_t)(callee->sig.outs.len + callee->sig.ins.len);
		if(ctx->pars.len < argc) return report_stack_underflow(t,ctx,op,loc,argc,(count_t)ctx->pars.len);
		return type_check_sig(t,ctx,func,&callee->sig,ctx->pars.len - argc,op,loc);
	}
	case OP_CALL_NATIVE_ON_STACK: {
		const Sig* sig = native_sig_on_stack(ctx,func);
		if(!sig) return report_type_error(t,ctx,TYPE_CHECK_ERROR_INVALID_CALL_TARGET,op,loc,ctx->pars.len ? TOP(ctx->pars) : PAR_IDX_INVALID,TYPE_INVALID_ID,ctx->pars.len ? par_type(ctx,func,TOP(ctx->pars)) : TYPE_INVALID_ID);
		count_t argc = (count_t)(sig->outs.len + sig->ins.len);
		if(ctx->pars.len < argc + 1) return report_stack_underflow(t,ctx,op,loc,argc + 1,(count_t)ctx->pars.len);
		return type_check_sig(t,ctx,func,sig,ctx->pars.len - argc - 1,op,loc);
	}
	}
	return report_type_check(t,ctx,(TypeCheckError){.kind = TYPE_CHECK_ERROR_INVALID_TYPE,.loc = loc,.op = op});
}

static bool portal_retargetable(CompileContext* ctx,const Func* func,par_idx idx){
	type_idx tid = par_type(ctx,func,idx);
	return type_idx_valid(func->types,tid) && func->types.data[tid].is_portal && par_root_is_local(ctx,idx);
}

static bool handle_add_hold(CompileContext* ctx,par_idx owner,par_idx held,CodeLoc loc,bool just_const){
	if(owner >= ctx->handles.len || held >= ctx->handles.len) return false;
	Handle* h = &ctx->handles.data[owner];
	for(count_t i = 0;i < h->num_holds;i++){
		HoldChain old = ctx->holds.data[h->holds_start + i];
		if(old.par == held && old.just_const == just_const) return true;
	}

	if(h->num_holds && h->holds_start + h->num_holds != ctx->holds.len){
		hold_idx start = (hold_idx)ctx->holds.len;
		for(count_t i = 0;i < h->num_holds;i++){
			PUSH_HEAP(ctx->holds, ctx->holds.data[h->holds_start + i]);
		}
		h = &ctx->handles.data[owner];
		h->holds_start = start;
	}
	else if(!h->num_holds){
		h->holds_start = (hold_idx)ctx->holds.len;
	}

	PUSH_HEAP(ctx->holds, ((HoldChain){
		.par = held,
		.site = loc,
		.parent = NO_PARRENT,
		.just_const = just_const,
	}));
	ctx->handles.data[owner].num_holds++;
	return true;
}

static int scan_basic(CompileContext* ctx,ScanState* state,func_idx current,block_idx block,Block b,ScanOp handler,void* user){
	Func* func = &ctx->funcs.data[current];
	size_t start = ctx->pars.len;
	for(count_t i = 0;i < b.data.basic.len;i++){
		op_idx opi = b.data.basic.start + i;
		OP op = func->ops.data[opi];
		CodeLoc loc = {.block = block,.op = opi};
		if(handler){
			int r = handler(user,ctx,state,current,op,loc);
			if(r) return r;
		}

		switch(op.kind){
		case OP_NULL:
			break;
		case OP_PUSH_VAR:
			if(!pars_push(ctx,comp_context_intern_par(ctx,(Par){.kind = PAR_LOCAL,.idx = op.extra}))) return 1;
			break;
		case OP_PUSH_ARG:
			if(!pars_push(ctx,comp_context_intern_par(ctx,(Par){.kind = PAR_ARG,.idx = op.extra}))) return 1;
			break;
		case OP_PUSH_GLOBAL:
			if(!pars_push(ctx,comp_context_intern_par(ctx,(Par){.kind = PAR_GLOBAL,.idx = op.extra}))) return 1;
			break;
		case OP_ASSIGN:
			if(ctx->pars.len < 2) return 1;
			if(!pars_pop(ctx,1)) return 1;
			break;
		case OP_ADD_ASSIGN:
		case OP_SUB_ASSIGN:
		case OP_MUL_ASSIGN:
		case OP_DIV_ASSIGN:
		case OP_AND_ASSIGN:
		case OP_OR_ASSIGN:
		case OP_XOR_ASSIGN:
			if(ctx->pars.len < 2) return 1;
			if(!pars_pop(ctx,1)) return 1;
			break;
		case OP_BIT_NOT_ASSIGN:
			if(ctx->pars.len < 1) return 1;
			break;
		case OP_DROP:
			if(!pars_pop(ctx,op.extra)) return 1;
			break;
		case OP_ARR_PUSH:
		case OP_ARR_DROP:
		case OP_SLICE_FROM_AR:
			if(ctx->pars.len < 2) return 1;
			if(!pars_pop(ctx,1)) return 1;
			break;
		case OP_SLICE_INC:
		case OP_SLICE_DEC:
			if(!pars_pop(ctx,1)) return 1;
			break;
		case OP_ARR_AT:
		case OP_SLICE_AT: {
			if(ctx->pars.len < 2) return 1;
			par_idx parent = ctx->pars.data[ctx->pars.len - 2];
			par_idx member = comp_context_intern_par(ctx,(Par){.kind = PAR_ARR_MEMBER,.parent = parent});
			ctx->pars.len--;
			ctx->pars.data[ctx->pars.len - 1] = member;
			break;
		}
		case OP_STRUCT_AT: {
			if(ctx->pars.len < 1) return 1;
			par_idx parent = ctx->pars.data[ctx->pars.len - 1];
			ctx->pars.data[ctx->pars.len - 1] = comp_context_intern_par(ctx,(Par){
				.kind = PAR_STRUCT_FILED,
				.idx = op.extra,
				.parent = parent,
			});
			break;
		}
		case OP_CALL: {
			if(op.extra >= ctx->funcs.len) return 1;
			Func* callee = &ctx->funcs.data[op.extra];
			if(ctx->pars.len < callee->sig.ins.len + callee->sig.outs.len) return 1;
			ctx->pars.len -= callee->sig.ins.len;
			break;
		}
		case OP_CALL_NATIVE_ON_STACK: {
			const Sig* sig = native_sig_on_stack(ctx,func);
			if(!sig) return 1;
			if(ctx->pars.len < sig->outs.len + sig->ins.len + 1) return 1;
			ctx->pars.len -= sig->ins.len + 1;
			break;
		}
		}
	}
	if(ctx->pars.len < start) return 1;
	ctx->pars.len = start;
	return 0;
}

static int scan_block(CompileContext* ctx,ScanState* state,func_idx current,block_idx idx,ScanOp handler,void* user){
	Func* func = &ctx->funcs.data[current];
	if(idx >= func->blocks.len) return 1;
	Block b = func->blocks.data[idx];
	size_t before = ctx->pars.len;
	switch(b.kind){
	case BLOCK_BASIC:
		return scan_basic(ctx,state,current,idx,b,handler,user);
	case BLOCK_MANY:
		for(count_t i = 0;i < b.data.many.len;i++){
			int r = scan_block(ctx,state,current,b.data.many.start + i,handler,user);
			if(r) return r;
		}
		return 0;
	case BLOCK_DEFER:
		if(scan_block(ctx,state,current,b.data.defer.next,handler,user)) return 1;
		ctx->pars.len = before;
		return scan_block(ctx,state,current,b.data.defer.defer,handler,user);
	case BLOCK_CRASH:
	case BLOCK_HARD_CRASH:
	case BLOCK_BREAK:
		return 0;
	case BLOCK_CRASH_PAD:
		if(scan_block(ctx,state,current,b.data.crash_pad.body,handler,user)) return 1;
		ctx->pars.len = before;
		return scan_block(ctx,state,current,b.data.crash_pad.pad,handler,user);
	case BLOCK_BRANCH:
		if(scan_block(ctx,state,current,b.data.branch.yes,handler,user)) return 1;
		ctx->pars.len = before;
		return scan_block(ctx,state,current,b.data.branch.no,handler,user);
	case BLOCK_LOOP:
		return scan_block(ctx,state,current,b.data.loop.body,handler,user);
	case BLOCK_VAR:
		if(b.data.var.var >= func->vars.len) return 1;
		{
			count_t old_var_depth = state->var_depths[b.data.var.var];
			count_t old_scope_depth = state->scope_depth;
			state->scope_depth++;
			state->var_depths[b.data.var.var] = state->scope_depth;
			int r = scan_block(ctx,state,current,b.data.var.body,handler,user);
			state->var_depths[b.data.var.var] = old_var_depth;
			state->scope_depth = old_scope_depth;
			return r;
		}
	}
	return 1;
}

static int scan_funcs(CompileContext* ctx,ScanOp handler,void* user){
	for(func_idx fid = 0;fid < ctx->funcs.len;fid++){
		ScanState state = {0};
		if(ctx->funcs.data[fid].vars.len){
			state.var_depths = calloc(ctx->funcs.data[fid].vars.len,sizeof(*state.var_depths));
			if(!state.var_depths) return 1;
		}
		ctx->pars.len = 0;
		int r = ctx->funcs.data[fid].blocks.len && scan_block(ctx,&state,fid,0,handler,user);
		free(state.var_depths);
		if(r) return 1;
	}
	ctx->pars.len = 0;
	return 0;
}

typedef struct CallAdapter {
	ProcessCall f;
	void* user;
} CallAdapter;

static int call_adapter(void* user,CompileContext* ctx,const ScanState* state,func_idx current,OP op,CodeLoc loc){
	(void)state;
	(void)current;
	if(op.kind != OP_CALL) return 0;
	CallAdapter* adapter = user;
	return adapter->f(adapter->user,ctx,op.extra,loc);
}

int comp_run_calls(CompileContext* ctx,ProcessCall f,void* user){
	CallAdapter adapter = {.f = f,.user = user};
	return scan_funcs(ctx,call_adapter,&adapter);
}

static int report_portal(TypeCheckCtx* p,CompileContext* ctx,TypeCheckErrorKind kind,par_idx portal,par_idx backing,CodeLoc loc,OP op){
	return report_type_check(p,ctx,(TypeCheckError){
		.kind = kind,
		.loc = loc,
		.op = op,
		.data.portal = {.portal = portal,.backing = backing},
	});
}

static int check_portal_region(TypeCheckCtx* p,CompileContext* ctx,const Func* func,const ScanState* state,par_idx portal,par_idx backing,CodeLoc loc,OP op){
	if(!portal_retargetable(ctx,func,portal)) return report_portal(p,ctx,TYPE_CHECK_ERROR_PORTAL_NON_LOCAL,portal,backing,loc,op);
	if(par_scope_depth(ctx,func,state,backing) > par_scope_depth(ctx,func,state,portal)){
		return report_portal(p,ctx,TYPE_CHECK_ERROR_PORTAL_BACKING_SCOPE,portal,backing,loc,op);
	}
	return 0;
}

static int type_check_first_pass_op(
	void* user,
	CompileContext* ctx,
	const ScanState* state,
	func_idx current,
	OP op,
	CodeLoc loc
){
	TypeCheckCtx* p = user;
	Func* func = &ctx->funcs.data[current];
	if(type_check_op(p,ctx,func,op,loc)) return 1;

	if(op.kind == OP_SLICE_FROM_AR){
		if(ctx->pars.len < 2) return 1;

		par_idx ref = ctx->pars.data[ctx->pars.len - 2];
		par_idx arr = ctx->pars.data[ctx->pars.len - 1];

		type_idx ref_tid = par_type(ctx, func, ref);
		if(!type_idx_valid(func->types, ref_tid)) return 1;

		Type ref_type = func->types.data[ref_tid];
		if(ref_type.kind != TYPE_SLICE && ref_type.kind != TYPE_VIEW) return 1;

		int r = check_portal_region(p, ctx, func, state, ref, arr, loc, op);
		if(r) return r;

		/*
			A view holds its backing data through a shared-only edge.
			A slice holds its backing data through an edge whose borrow kind
			depends on how the slice itself is borrowed.
		*/
		bool shared_only = ref_type.kind == TYPE_VIEW;
		return handle_add_hold(ctx, ref, arr, loc, shared_only) ? 0 : 1;
	}

	if(op.kind == OP_ASSIGN){
		if(ctx->pars.len < 2) return 1;

		par_idx dst = ctx->pars.data[ctx->pars.len - 2];
		par_idx src = ctx->pars.data[ctx->pars.len - 1];

		if(!par_is_portal(ctx, func, dst)) return 0;

		if(!portal_retargetable(ctx, func, dst)){
			return report_portal(
				p,
				ctx,
				TYPE_CHECK_ERROR_PORTAL_NON_LOCAL,
				dst,
				src,
				loc,
				op
			);
		}

		/*
			If assigning one portal to another, do not copy src's holds into dst.
			Record the actual graph edge:

				dst -> src

			Then borrow_par() will recursively walk:

				dst -> src -> src's backing data

			This preserves the fact that dst depends on src as a portal object.
		*/
		if(par_is_portal(ctx, func, src)){
			int r = check_portal_region(p, ctx, func, state, dst, src, loc, op);
			if(r) return r;

			return handle_add_hold(ctx, dst, src, loc, false) ? 0 : 1;
		}

		/*
			Assignment from a non-portal into a portal is either ordinary value
			copying or should be rejected by the later type checker. This pass
			only records backing relationships.
		*/
		return 0;
	}

	return 0;
}

int type_check_first_pass(TypeCheckErrorReporter reporter,CompileContext* ctx){
	ctx->holds.len = 0;
	for(size_t i = 0;i < ctx->handles.len;i++){
		ctx->handles.data[i].holds_start = 0;
		ctx->handles.data[i].num_holds = 0;
	}
	TypeCheckCtx p = {.reporter = reporter};
	return scan_funcs(ctx,type_check_first_pass_op,&p);
}

typedef struct BorrowCtx {
	AliasErrorReporter reporter;
	void* user;
	par_idx first_mut;
} BorrowCtx;

static int report_alias(BorrowCtx* b,CompileContext* ctx,par_idx second,CodeLoc loc){
	if(!b->reporter) return 1;
	return b->reporter(b->user,ctx,(AliasError){
		.first_mut = b->first_mut,
		.second = second,
		.call = loc,
	});
}

static int borrow_par(BorrowCtx* b,CompileContext* ctx,const Func* func,par_idx idx,life_t want,CodeLoc loc);

static int borrow_struct_fields(BorrowCtx* b,CompileContext* ctx,const Func* func,par_idx idx,Type type,life_t want,CodeLoc loc){
	for(count_t i = 0;i < type.data.fields.len;i++){
		par_idx field = comp_context_intern_par(ctx,(Par){.kind = PAR_STRUCT_FILED,.idx = i,.parent = idx});
		int r = borrow_par(b,ctx,func,field,want,loc);
		if(r) return r;
	}
	return 0;
}

static int borrow_par(BorrowCtx* b,CompileContext* ctx,const Func* func,par_idx idx,life_t want,CodeLoc loc){
	if(idx >= ctx->handles.len) return 1;
	Par par = ctx->handles.data[idx].par;
	if(par.kind == PAR_ARR_MEMBER) return borrow_par(b,ctx,func,par.parent,want,loc);

	Handle* h = &ctx->handles.data[idx];
	if(h->life == LIFE_UNIQUE || (h->life == LIFE_SHARED && want == LIFE_UNIQUE)){
		return report_alias(b,ctx,idx,loc);
	}
	if(want == LIFE_UNIQUE){
		h->life = LIFE_UNIQUE;
		b->first_mut = idx;
	}
	else if(h->life == LIFE_FREE){
		h->life = LIFE_SHARED;
	}

	for(count_t i = 0;i < h->num_holds;i++){
		HoldChain hold = ctx->holds.data[h->holds_start + i];
		life_t held_want = hold.just_const ? LIFE_SHARED : want;
		int r = borrow_par(b,ctx,func,hold.par,held_want,loc);
		if(r) return r;
	}

	type_idx tid = par_type(ctx,func,idx);
	if(type_idx_valid(func->types,tid)){
		Type type = func->types.data[tid];
		if(type.kind == TYPE_STRUCT){
			int r = borrow_struct_fields(b,ctx,func,idx,type,want,loc);
			if(r) return r;
		}
	}
	return 0;
}

static int borrow_check_sig(BorrowCtx* b,CompileContext* ctx,const Func* caller,const Sig* sig,size_t base,CodeLoc loc){
	count_t outs = (count_t)sig->outs.len;
	count_t ins = (count_t)sig->ins.len;
	if(ctx->pars.len < base + outs + ins) return 1;

	for(size_t i = 0;i < ctx->handles.len;i++) ctx->handles.data[i].life = LIFE_FREE;
	b->first_mut = PAR_IDX_INVALID;

	for(count_t i = 0;i < outs;i++){
		b->first_mut = ctx->pars.data[base + i];
		int r = borrow_par(b,ctx,caller,b->first_mut,LIFE_UNIQUE,loc);
		if(r) return r;
	}
	for(count_t i = 0;i < ins;i++){
		par_idx par = ctx->pars.data[base + outs + i];
		life_t want = sig->ins.data[i].mut ? LIFE_UNIQUE : LIFE_SHARED;
		type_idx tid = par_type(ctx,caller,par);
		if(want == LIFE_UNIQUE && (!type_idx_valid(caller->types,tid) || caller->types.data[tid].is_portal)) return 1;
		if(want == LIFE_UNIQUE) b->first_mut = par;
		int r = borrow_par(b,ctx,caller,par,want,loc);
		if(r) return r;
	}
	return 0;
}

static int borrow_check_call(void* user,CompileContext* ctx,const ScanState* state,func_idx current,OP op,CodeLoc loc){
	(void)state;
	Func* caller = &ctx->funcs.data[current];
	BorrowCtx* b = user;

	if(op.kind == OP_CALL){
		if(op.extra >= ctx->funcs.len) return 1;
		Func* callee = &ctx->funcs.data[op.extra];
		count_t outs = (count_t)callee->sig.outs.len;
		count_t ins = (count_t)callee->sig.ins.len;
		if(ctx->pars.len < outs + ins) return 1;
		return borrow_check_sig(b,ctx,caller,&callee->sig,ctx->pars.len - outs - ins,loc);
	}

	if(op.kind == OP_CALL_NATIVE_ON_STACK){
		const Sig* sig = native_sig_on_stack(ctx,caller);
		if(!sig) return 1;
		count_t outs = (count_t)sig->outs.len;
		count_t ins = (count_t)sig->ins.len;
		if(ctx->pars.len < outs + ins + 1) return 1;
		return borrow_check_sig(b,ctx,caller,sig,ctx->pars.len - 1 - outs - ins,loc);
	}

	return 0;
}

int borrow_check(AliasErrorReporter reporter,CompileContext* ctx){
	int r = type_check_first_pass(NULL,ctx);
	if(r) return r;
	BorrowCtx b = {.reporter = reporter,.user = NULL,.first_mut = PAR_IDX_INVALID};
	return scan_funcs(ctx,borrow_check_call,&b);
}
