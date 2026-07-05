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
		case OP_CALL_NATIVE_ON_STACK:
			return 1;
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

typedef struct PortalCtx {
	PortalErrorReporter reporter;
	void* user;
} PortalCtx;

static int report_portal(PortalCtx* p,CompileContext* ctx,PortalErrorKind kind,par_idx portal,par_idx backing,CodeLoc loc){
	if(!p->reporter) return 1;
	return p->reporter(p->user,ctx,(PortalError){
		.kind = kind,
		.portal = portal,
		.backing = backing,
		.loc = loc,
	});
}

static int check_portal_region(PortalCtx* p,CompileContext* ctx,const Func* func,const ScanState* state,par_idx portal,par_idx backing,CodeLoc loc){
	if(!portal_retargetable(ctx,func,portal)) return report_portal(p,ctx,PORTAL_ERROR_NON_LOCAL,portal,backing,loc);
	if(par_scope_depth(ctx,func,state,backing) > par_scope_depth(ctx,func,state,portal)){
		return report_portal(p,ctx,PORTAL_ERROR_BACKING_SCOPE,portal,backing,loc);
	}
	return 0;
}

static int copy_portal_holds(PortalCtx* p,CompileContext* ctx,const Func* func,const ScanState* state,par_idx dst,par_idx src,CodeLoc loc){
	if(src >= ctx->handles.len) return 1;
	Handle* h = &ctx->handles.data[src];
	for(count_t i = 0;i < h->num_holds;i++){
		HoldChain hold = ctx->holds.data[h->holds_start + i];
		int r = check_portal_region(p,ctx,func,state,dst,hold.par,loc);
		if(r) return r;
		if(!handle_add_hold(ctx,dst,hold.par,loc,hold.just_const)) return 1;
	}
	return 0;
}

static int gather_portal_regions_op(void* user,CompileContext* ctx,const ScanState* state,func_idx current,OP op,CodeLoc loc){
	PortalCtx* p = user;
	Func* func = &ctx->funcs.data[current];
	if(op.kind == OP_SLICE_FROM_AR){
		if(ctx->pars.len < 2) return 1;
		par_idx ref = ctx->pars.data[ctx->pars.len - 2];
		par_idx arr = ctx->pars.data[ctx->pars.len - 1];
		type_idx ref_tid = par_type(ctx,func,ref);
		if(!type_idx_valid(func->types,ref_tid)) return 1;
		Type ref_type = func->types.data[ref_tid];
		if(ref_type.kind != TYPE_SLICE && ref_type.kind != TYPE_VIEW) return 1;
		int r = check_portal_region(p,ctx,func,state,ref,arr,loc);
		if(r) return r;
		return handle_add_hold(ctx,ref,arr,loc,ref_type.kind == TYPE_VIEW) ? 0 : 1;
	}
	if(op.kind == OP_ASSIGN){
		if(ctx->pars.len < 2) return 1;
		par_idx dst = ctx->pars.data[ctx->pars.len - 2];
		par_idx src = ctx->pars.data[ctx->pars.len - 1];
		if(!par_is_portal(ctx,func,dst)) return 0;
		if(!portal_retargetable(ctx,func,dst)) return report_portal(p,ctx,PORTAL_ERROR_NON_LOCAL,dst,src,loc);
		if(par_is_portal(ctx,func,src)) return copy_portal_holds(p,ctx,func,state,dst,src,loc);
	}
	return 0;
}

int gather_portal_regions(PortalErrorReporter reporter,CompileContext* ctx){
	ctx->holds.len = 0;
	for(size_t i = 0;i < ctx->handles.len;i++){
		ctx->handles.data[i].holds_start = 0;
		ctx->handles.data[i].num_holds = 0;
	}
	PortalCtx p = {.reporter = reporter};
	return scan_funcs(ctx,gather_portal_regions_op,&p);
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

static int borrow_check_call(void* user,CompileContext* ctx,const ScanState* state,func_idx current,OP op,CodeLoc loc){
	(void)state;
	if(op.kind != OP_CALL) return 0;
	if(op.extra >= ctx->funcs.len) return 1;
	Func* caller = &ctx->funcs.data[current];
	Func* callee = &ctx->funcs.data[op.extra];
	count_t outs = (count_t)callee->sig.outs.len;
	count_t ins = (count_t)callee->sig.ins.len;
	if(ctx->pars.len < outs + ins) return 1;

	for(size_t i = 0;i < ctx->handles.len;i++) ctx->handles.data[i].life = LIFE_FREE;
	BorrowCtx* b = user;
	b->first_mut = PAR_IDX_INVALID;

	count_t base = (count_t)(ctx->pars.len - outs - ins);
	for(count_t i = 0;i < outs;i++){
		b->first_mut = ctx->pars.data[base + i];
		int r = borrow_par(b,ctx,caller,b->first_mut,LIFE_UNIQUE,loc);
		if(r) return r;
	}
	for(count_t i = 0;i < ins;i++){
		par_idx par = ctx->pars.data[base + outs + i];
		life_t want = callee->sig.ins.data[i].mut ? LIFE_UNIQUE : LIFE_SHARED;
		type_idx tid = par_type(ctx,caller,par);
		if(want == LIFE_UNIQUE && (!type_idx_valid(caller->types,tid) || caller->types.data[tid].is_portal)) return 1;
		if(want == LIFE_UNIQUE) b->first_mut = par;
		int r = borrow_par(b,ctx,caller,par,want,loc);
		if(r) return r;
	}
	return 0;
}

int borrow_check(AliasErrorReporter reporter,CompileContext* ctx){
	int r = gather_portal_regions(NULL,ctx);
	if(r) return r;
	BorrowCtx b = {.reporter = reporter,.user = NULL,.first_mut = PAR_IDX_INVALID};
	return scan_funcs(ctx,borrow_check_call,&b);
}
