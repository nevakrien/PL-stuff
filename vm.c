#include "vm.h"

#include <string.h>

typedef STACK(ByteCode) CodeBuilder;
typedef STACK(type_idx) TypeStack;

typedef enum CompileResult {
	COMPILE_FAIL = 0,
	COMPILE_REACHABLE,
	COMPILE_UNREACHABLE,
} CompileResult;

typedef struct VarLayout {
	type_idx tid;
	offset_t base;
	bool live;
} VarLayout;

typedef struct Scope {
	STACK(size_t) patches;
	size_t target;
	offset_t frame_size;
	size_t crash_depth;
} Scope;

typedef STACK(Scope) ScopeStack;

typedef struct Compiler {
	const Func* func;
	CompileContext* ctx;
	CodeBuilder code;
	TypeStack params;
	VarLayout* vars;
	ScopeStack scopes;
	offset_t frame_size;
	size_t crash_depth;
} Compiler;

typedef struct ReturnFrame {
	const ByteCode* pc;
	const ByteCode* base;
	size_t param_base;
} ReturnFrame;

VmCode vm_compile_no_defers(const Func* func,CompileContext* ctx);

typedef struct CompilerState {
	TypeStack params;
	VarLayout* vars;
	offset_t frame_size;
	size_t crash_depth;
} CompilerState;

static bool grow_bytes(CodeBuilder* code,size_t n){
	if(code->len + n <= code->cap) return true;
	size_t cap = code->cap ? code->cap : 64;
	while(cap < code->len + n) cap *= 2;
	ByteCode* data = realloc(code->data,cap * sizeof(*code->data));
	if(!data) return false;
	code->data = data;
	code->cap = cap;
	return true;
}

static bool emit_bytes(CodeBuilder* code,const void* data,size_t n){
	if(!grow_bytes(code,n)) return false;
	memcpy(code->data + code->len,data,n);
	code->len += n;
	return true;
}

static bool emit_op(CodeBuilder* code,ByteCode op){
	return emit_bytes(code,&op,sizeof(op));
}

static bool emit_offset(CodeBuilder* code,offset_t x){
	return emit_bytes(code,&x,sizeof(x));
}

static bool emit_uoffset(CodeBuilder* code,uoffset_t x){
	return emit_bytes(code,&x,sizeof(x));
}

static bool emit_count(CodeBuilder* code,count_t x){
	return emit_bytes(code,&x,sizeof(x));
}

static bool emit_size(CodeBuilder* code,size_t x){
	return emit_bytes(code,&x,sizeof(x));
}

static bool emit_pointer(CodeBuilder* code,const void* x){
	return emit_bytes(code,&x,sizeof(x));
}

static bool patch_uoffset(CodeBuilder* code,size_t at,size_t target){
	if(target > (size_t)(uoffset_t)-1) return false;
	uoffset_t x = (uoffset_t)target;
	memcpy(code->data + at,&x,sizeof(x));
	return true;
}

static const ByteCode* read_offset(const ByteCode* pc,offset_t* x){
	memcpy(x,pc,sizeof(*x));
	return pc + sizeof(*x);
}

static const ByteCode* read_uoffset(const ByteCode* pc,uoffset_t* x){
	memcpy(x,pc,sizeof(*x));
	return pc + sizeof(*x);
}

static const ByteCode* read_count(const ByteCode* pc,count_t* x){
	memcpy(x,pc,sizeof(*x));
	return pc + sizeof(*x);
}

static const ByteCode* read_size(const ByteCode* pc,size_t* x){
	memcpy(x,pc,sizeof(*x));
	return pc + sizeof(*x);
}

static const ByteCode* read_pointer(const ByteCode* pc,const void** x){
	memcpy(x,pc,sizeof(*x));
	return pc + sizeof(*x);
}

static bool push_type(TypeStack* stack,type_idx tid){
	if(stack->len == stack->cap){
		size_t cap = stack->cap ? stack->cap * 2 : 16;
		type_idx* data = realloc(stack->data,cap * sizeof(*stack->data));
		if(!data) return false;
		stack->data = data;
		stack->cap = cap;
	}
	stack->data[stack->len++] = tid;
	return true;
}

static bool pop_types(TypeStack* stack,count_t n){
	if(stack->len < n) return false;
	stack->len -= n;
	return true;
}

static size_t type_payload_size(TypeS types,type_idx tid){
	assert(type_idx_valid(types,tid));
	return types.data[tid].payload_size;
}

static bool emit_storage_add(Compiler* c,offset_t amount){
	if(!emit_op(&c->code,B_STORAGE_ADD)) return false;
	if(!emit_offset(&c->code,amount)) return false;
	c->frame_size += amount;
	return c->frame_size >= 0;
}

static offset_t var_rel(const Compiler* c,var_idx idx){
	assert(idx < c->func->vars.len);
	assert(c->vars[idx].live);
	return c->vars[idx].base - c->frame_size;
}

static bool emit_push_var(Compiler* c,var_idx idx){
	if(idx >= c->func->vars.len || !c->vars[idx].live) return false;
	if(!emit_op(&c->code,B_PUSH_VAR)) return false;
	if(!emit_offset(&c->code,var_rel(c,idx))) return false;
	return push_type(&c->params,c->vars[idx].tid);
}

static bool emit_push_arg(Compiler* c,var_idx idx){
	if(idx >= c->func->vars.len) return false;
	if(!type_idx_valid(c->func->types,c->func->vars.data[idx].tid)) return false;
	if(!emit_op(&c->code,B_PUSH_ARG)) return false;
	if(!emit_count(&c->code,idx)) return false;
	return push_type(&c->params,c->func->vars.data[idx].tid);
}

static bool emit_jump_placeholder(Compiler* c,size_t* patch){
	if(!emit_op(&c->code,B_JUMP)) return false;
	*patch = c->code.len;
	return emit_uoffset(&c->code,0);
}

static void state_free(CompilerState* s){
	free(s->params.data);
	free(s->vars);
	*s = (CompilerState){0};
}

static bool state_save(const Compiler* c,CompilerState* out){
	*out = (CompilerState){0};
	out->frame_size = c->frame_size;
	out->crash_depth = c->crash_depth;
	out->params.len = c->params.len;
	out->params.cap = c->params.len;

	if(c->params.len){
		out->params.data = malloc(c->params.len * sizeof(*out->params.data));
		if(!out->params.data) return false;
		memcpy(out->params.data,c->params.data,c->params.len * sizeof(*out->params.data));
	}

	if(c->func->vars.len){
		out->vars = malloc(c->func->vars.len * sizeof(*out->vars));
		if(!out->vars){
			state_free(out);
			return false;
		}
		memcpy(out->vars,c->vars,c->func->vars.len * sizeof(*out->vars));
	}

	return true;
}

static bool state_restore(Compiler* c,const CompilerState* s){
	if(c->params.cap < s->params.len){
		type_idx* data = realloc(c->params.data,s->params.len * sizeof(*data));
		if(!data && s->params.len) return false;
		c->params.data = data;
		c->params.cap = s->params.len;
	}

	if(s->params.len){
		memcpy(c->params.data,s->params.data,s->params.len * sizeof(*c->params.data));
	}
	c->params.len = s->params.len;

	if(c->func->vars.len){
		memcpy(c->vars,s->vars,c->func->vars.len * sizeof(*c->vars));
	}

	c->frame_size = s->frame_size;
	c->crash_depth = s->crash_depth;
	return true;
}

static bool state_equal(const Compiler* c,const CompilerState* s){
	if(c->frame_size != s->frame_size) return false;
	if(c->crash_depth != s->crash_depth) return false;
	if(c->params.len != s->params.len) return false;

	for(size_t i=0;i<c->params.len;i++){
		if(c->params.data[i] != s->params.data[i]) return false;
	}

	for(var_idx i=0;i<c->func->vars.len;i++){
		if(c->vars[i].tid != s->vars[i].tid) return false;
		if(c->vars[i].base != s->vars[i].base) return false;
		if(c->vars[i].live != s->vars[i].live) return false;
	}

	return true;
}

static bool scope_push(Compiler* c,offset_t frame_size){
	Scope s = {0};
	s.target = (size_t)-1;
	s.frame_size = frame_size;
	s.crash_depth = c->crash_depth;

	if(c->scopes.len == c->scopes.cap){
		size_t cap = c->scopes.cap ? c->scopes.cap * 2 : 16;
		Scope* data = realloc(c->scopes.data,cap * sizeof(*c->scopes.data));
		if(!data) return false;
		c->scopes.data = data;
		c->scopes.cap = cap;
	}

	c->scopes.data[c->scopes.len++] = s;
	return true;
}

static bool scope_add_patch(Scope* s,size_t patch){
	if(s->patches.len == s->patches.cap){
		size_t cap = s->patches.cap ? s->patches.cap * 2 : 8;
		size_t* data = realloc(s->patches.data,cap * sizeof(*s->patches.data));
		if(!data) return false;
		s->patches.data = data;
		s->patches.cap = cap;
	}
	s->patches.data[s->patches.len++] = patch;
	return true;
}

static bool scope_finish(Compiler* c,size_t target,bool* had_patches){
	assert(c->scopes.len);
	Scope* s = &TOP(c->scopes);
	s->target = target;

	if(had_patches) *had_patches = s->patches.len != 0;

	for(size_t i=0;i<s->patches.len;i++){
		if(!patch_uoffset(&c->code,s->patches.data[i],target)) return false;
	}

	free(s->patches.data);
	c->scopes.len--;
	return true;
}

static CompileResult compile_block(Compiler* c,block_idx idx);

static bool ensure_code_slot(CompileContext* ctx,size_t idx){
	if(idx < ctx->code.len) return true;
	size_t old_len = ctx->code.len;
	if(idx >= ctx->code.cap){
		size_t cap = ctx->code.cap ? ctx->code.cap : 8;
		while(cap <= idx) cap *= 2;
		VmCode* data = realloc(ctx->code.data,cap * sizeof(*ctx->code.data));
		if(!data) return false;
		ctx->code.data = data;
		ctx->code.cap = cap;
	}
	ctx->code.len = idx + 1;
	memset(ctx->code.data + old_len,0,(ctx->code.len - old_len) * sizeof(*ctx->code.data));
	return true;
}

static VmCode* compile_context_func_code(CompileContext* ctx,count_t idx){
	if(!ctx || idx >= ctx->funcs.len) return NULL;
	if(!ensure_code_slot(ctx,idx)) return NULL;

	VmCode* code = &ctx->code.data[idx];
	if(!code->data){
		*code = vm_compile_no_defers(&ctx->funcs.data[idx],ctx);
		if(!code->data) return NULL;
	}
	return code;
}

static bool compile_func_call(Compiler* c,count_t idx){
	VmCode* code = compile_context_func_code(c->ctx,idx);
	if(!code) return false;
	Func* func = &c->ctx->funcs.data[idx];
	if(c->params.len < func->sig.outs.len + func->sig.ins.len) return false;

	size_t start = c->params.len - func->sig.outs.len - func->sig.ins.len;
	for(size_t i=0;i<func->sig.outs.len;i++){
		if(c->params.data[start + i] != func->sig.outs.data[i].tid) return false;
	}
	for(size_t i=0;i<func->sig.ins.len;i++){
		if(c->params.data[start + func->sig.outs.len + i] != func->sig.ins.data[i].tid) return false;
	}

	if(!emit_op(&c->code,B_CALL)) return false;
	if(!emit_pointer(&c->code,code->data)) return false;
	if(!emit_count(&c->code,(count_t)(func->sig.outs.len + func->sig.ins.len))) return false;
	return pop_types(&c->params,(count_t)func->sig.ins.len);
}

static bool compile_op(Compiler* c,OP op){
	TypeS types = c->func->types;
	switch(op.kind){
	case OP_NULL:
		return true;

	case OP_CALL:
		return compile_func_call(c,op.extra);

	case OP_CALL_NATIVE_ON_STACK:
		if(c->params.len < 1) return false;
		if(!type_idx_valid(types,TOP(c->params))) return false;
		if(types.data[TOP(c->params)].kind != TYPE_NATIVE_FUNC_POINTER) return false;
		if(!emit_op(&c->code,B_CALL_NATIVE)) return false;
		return pop_types(&c->params,1);

	case OP_ASSIGN: {
		if(c->params.len < 2) return false;
		type_idx dst = c->params.data[c->params.len - 2];
		if(dst != c->params.data[c->params.len - 1]) return false;
		if(!emit_op(&c->code,B_COPY)) return false;
		if(!emit_size(&c->code,type_payload_size(types,dst))) return false;
		return pop_types(&c->params,1);
	}

	case OP_ADD_ASSIGN:
	case OP_SUB_ASSIGN:
	case OP_MUL_ASSIGN:
	case OP_DIV_ASSIGN:
	case OP_AND_ASSIGN:
	case OP_OR_ASSIGN:
	case OP_XOR_ASSIGN: {
		if(c->params.len < 2) return false;
		if(c->params.data[c->params.len - 2] != TYPE_INT_ID) return false;
		if(c->params.data[c->params.len - 1] != TYPE_INT_ID) return false;
		ByteCode bop = B_ADD;
		if(op.kind == OP_SUB_ASSIGN) bop = B_SUB;
		if(op.kind == OP_MUL_ASSIGN) bop = B_MUL;
		if(op.kind == OP_DIV_ASSIGN) bop = B_DIV;
		if(op.kind == OP_AND_ASSIGN) bop = B_AND;
		if(op.kind == OP_OR_ASSIGN) bop = B_OR;
		if(op.kind == OP_XOR_ASSIGN) bop = B_XOR;
		if(!emit_op(&c->code,bop)) return false;
		return pop_types(&c->params,1);
	}

	case OP_BIT_NOT_ASSIGN:
		if(c->params.len < 1 || c->params.data[c->params.len - 1] != TYPE_INT_ID) return false;
		return emit_op(&c->code,B_BIT_NOT);

	case OP_DROP:
		if(!pop_types(&c->params,op.extra)) return false;
		if(!emit_op(&c->code,B_DROP_N)) return false;
		return emit_count(&c->code,op.extra);

	case OP_PUSH_VAR:
		return emit_push_var(c,op.extra);
	case OP_PUSH_ARG:
		return emit_push_arg(c,op.extra);

	case OP_PUSH_GLOBAL:
		if(!c->ctx || op.extra >= c->ctx->globals.len) return false;
		if(!type_idx_valid(types,c->ctx->globals.data[op.extra].var.tid)) return false;
		if(!emit_op(&c->code,B_PUSH_GLOBAL)) return false;
		if(!emit_pointer(&c->code,c->ctx->globals.data[op.extra].mem)) return false;
		return push_type(&c->params,c->ctx->globals.data[op.extra].var.tid);

	case OP_ARR_AT: {
		if(c->params.len < 2) return false;
		type_idx arr_tid = c->params.data[c->params.len - 2];
		if(c->params.data[c->params.len - 1] != TYPE_INT_ID) return false;
		if(!type_idx_valid(types,arr_tid) || types.data[arr_tid].kind != TYPE_ARRAY) return false;
		Type arr = types.data[arr_tid];
		Type elem = types.data[arr.data.array.elem];
		if(!emit_op(&c->code,B_PUSH_ARR_AT)) return false;
		if(!emit_size(&c->code,arr.data.array.data_offset)) return false;
		if(!emit_size(&c->code,elem.payload_size)) return false;
		if(!emit_count(&c->code,arr.data.array.capacity)) return false;
		if(!pop_types(&c->params,2)) return false;
		return push_type(&c->params,arr.data.array.elem);
	}

	case OP_ARR_PUSH: {
		if(c->params.len < 2) return false;
		type_idx arr_tid = c->params.data[c->params.len - 2];
		if(!type_idx_valid(types,arr_tid) || types.data[arr_tid].kind != TYPE_ARRAY) return false;
		Type arr = types.data[arr_tid];
		if(c->params.data[c->params.len - 1] != arr.data.array.elem) return false;
		Type elem = types.data[arr.data.array.elem];
		if(!emit_op(&c->code,B_ARR_PUSH)) return false;
		if(!emit_size(&c->code,arr.data.array.data_offset)) return false;
		if(!emit_size(&c->code,elem.payload_size)) return false;
		if(!emit_count(&c->code,arr.data.array.capacity)) return false;
		return pop_types(&c->params,1);
	}

	case OP_ARR_DROP:
		if(c->params.len < 1) return false;
		if(!type_idx_valid(types,TOP(c->params)) || types.data[TOP(c->params)].kind != TYPE_ARRAY) return false;
		return emit_op(&c->code,B_ARR_DROP);
	}

	return false;
}

static CompileResult compile_basic(Compiler* c,Block b){
	size_t start_params = c->params.len;
	for(count_t i=0;i<b.data.basic.len;i++){
		if(!compile_op(c,c->func->ops.data[b.data.basic.start + i])) return COMPILE_FAIL;
	}

	if(c->params.len < start_params) return COMPILE_FAIL;
	count_t extra = (count_t)(c->params.len - start_params);
	if(extra){
		if(!emit_op(&c->code,B_DROP_N)) return COMPILE_FAIL;
		if(!emit_count(&c->code,extra)) return COMPILE_FAIL;
		if(!pop_types(&c->params,extra)) return COMPILE_FAIL;
	}

	return COMPILE_REACHABLE;
}

static CompileResult compile_break(Compiler* c,count_t level){
	if(level == 0 || level > c->scopes.len) return COMPILE_FAIL;

	Scope* target = &c->scopes.data[c->scopes.len - level];

	for(size_t i=c->crash_depth;i>target->crash_depth;i--){
		if(!emit_op(&c->code,B_POP_CRASH)) return COMPILE_FAIL;
	}

	offset_t cleanup = target->frame_size - c->frame_size;
	if(cleanup){
		if(!emit_op(&c->code,B_STORAGE_ADD)) return COMPILE_FAIL;
		if(!emit_offset(&c->code,cleanup)) return COMPILE_FAIL;
	}

	if(!emit_op(&c->code,B_JUMP)) return COMPILE_FAIL;
	size_t patch = c->code.len;
	if(!emit_uoffset(&c->code,0)) return COMPILE_FAIL;
	if(!scope_add_patch(target,patch)) return COMPILE_FAIL;

	return COMPILE_UNREACHABLE;
}

static CompileResult compile_many(Compiler* c,Block b){
	offset_t frame = c->frame_size;
	if(!scope_push(c,frame)) return COMPILE_FAIL;

	CompileResult r = COMPILE_REACHABLE;
	for(count_t i=0;i<b.data.many.len;i++){
		if(r == COMPILE_UNREACHABLE) break;
		r = compile_block(c,b.data.many.start + i);
		if(r == COMPILE_FAIL) return COMPILE_FAIL;
	}

	bool had_breaks = false;
	if(!scope_finish(c,c->code.len,&had_breaks)) return COMPILE_FAIL;

	if(r == COMPILE_REACHABLE || had_breaks) return COMPILE_REACHABLE;
	return COMPILE_UNREACHABLE;
}

static CompileResult compile_var(Compiler* c,Block b){
	if(b.data.var.var >= c->func->vars.len) return COMPILE_FAIL;

	Var var = c->func->vars.data[b.data.var.var];
	if(!type_idx_valid(c->func->types,var.tid)) return COMPILE_FAIL;

	offset_t old_frame = c->frame_size;
	size_t size = type_stack_size(c->func->types,var.tid);
	if(size > (size_t)((uoffset_t)-1 / 2)) return COMPILE_FAIL;

	if(!scope_push(c,old_frame)) return COMPILE_FAIL;

	c->vars[b.data.var.var] = (VarLayout){
		.tid = var.tid,
		.base = c->frame_size,
		.live = true,
	};

	if(!emit_storage_add(c,(offset_t)size)) return COMPILE_FAIL;

	CompileResult r = compile_block(c,b.data.var.body);
	if(r == COMPILE_FAIL) return COMPILE_FAIL;

	if(r == COMPILE_REACHABLE){
		c->vars[b.data.var.var].live = false;
		if(!emit_storage_add(c,-(offset_t)size)) return COMPILE_FAIL;
	}

	bool had_breaks = false;
	if(!scope_finish(c,c->code.len,&had_breaks)) return COMPILE_FAIL;

	c->vars[b.data.var.var].live = false;
	c->frame_size = old_frame;

	if(r == COMPILE_REACHABLE || had_breaks) return COMPILE_REACHABLE;
	return COMPILE_UNREACHABLE;
}

static CompileResult compile_branch(Compiler* c,Block b){
	if(b.data.branch.cond >= c->func->vars.len) return COMPILE_FAIL;
	if(!c->vars[b.data.branch.cond].live) return COMPILE_FAIL;
	if(c->vars[b.data.branch.cond].tid != TYPE_INT_ID) return COMPILE_FAIL;

	CompilerState before = {0};
	CompilerState yes_state = {0};
	CompilerState no_state = {0};

	if(!state_save(c,&before)) return COMPILE_FAIL;

	if(!emit_op(&c->code,B_BRANCH)) goto fail;
	if(!emit_offset(&c->code,var_rel(c,b.data.branch.cond))) goto fail;

	size_t yes_patch = c->code.len;
	if(!emit_uoffset(&c->code,0)) goto fail;

	size_t no_patch = c->code.len;
	if(!emit_uoffset(&c->code,0)) goto fail;

	if(!patch_uoffset(&c->code,yes_patch,c->code.len)) goto fail;

	CompileResult yes_r = compile_block(c,b.data.branch.yes);
	if(yes_r == COMPILE_FAIL) goto fail;

	size_t end_patch = (size_t)-1;
	if(yes_r == COMPILE_REACHABLE){
		if(!state_save(c,&yes_state)) goto fail;
		if(!emit_jump_placeholder(c,&end_patch)) goto fail;
	}

	if(!state_restore(c,&before)) goto fail;

	if(!patch_uoffset(&c->code,no_patch,c->code.len)) goto fail;

	CompileResult no_r = compile_block(c,b.data.branch.no);
	if(no_r == COMPILE_FAIL) goto fail;

	if(no_r == COMPILE_REACHABLE){
		if(!state_save(c,&no_state)) goto fail;
	}

	if(end_patch != (size_t)-1){
		if(!patch_uoffset(&c->code,end_patch,c->code.len)) goto fail;
	}

	if(yes_r == COMPILE_REACHABLE && no_r == COMPILE_REACHABLE){
		if(!state_restore(c,&yes_state)) goto fail;
		if(!state_equal(c,&no_state)) goto fail;
		state_free(&before);
		state_free(&yes_state);
		state_free(&no_state);
		return COMPILE_REACHABLE;
	}

	if(yes_r == COMPILE_REACHABLE){
		if(!state_restore(c,&yes_state)) goto fail;
		state_free(&before);
		state_free(&yes_state);
		state_free(&no_state);
		return COMPILE_REACHABLE;
	}

	if(no_r == COMPILE_REACHABLE){
		if(!state_restore(c,&no_state)) goto fail;
		state_free(&before);
		state_free(&yes_state);
		state_free(&no_state);
		return COMPILE_REACHABLE;
	}

	state_free(&before);
	state_free(&yes_state);
	state_free(&no_state);
	return COMPILE_UNREACHABLE;

fail:
	state_free(&before);
	state_free(&yes_state);
	state_free(&no_state);
	return COMPILE_FAIL;
}

static CompileResult compile_loop(Compiler* c,Block b){
	if(b.data.loop.cond >= c->func->vars.len) return COMPILE_FAIL;
	if(!c->vars[b.data.loop.cond].live) return COMPILE_FAIL;
	if(c->vars[b.data.loop.cond].tid != TYPE_INT_ID) return COMPILE_FAIL;

	size_t start = c->code.len;
	offset_t frame = c->frame_size;

	CompilerState head = {0};
	if(!state_save(c,&head)) return COMPILE_FAIL;

	if(!scope_push(c,frame)){
		state_free(&head);
		return COMPILE_FAIL;
	}

	if(!emit_op(&c->code,B_BRANCH)) goto fail;
	if(!emit_offset(&c->code,var_rel(c,b.data.loop.cond))) goto fail;

	size_t body_patch = c->code.len;
	if(!emit_uoffset(&c->code,0)) goto fail;

	size_t end_patch = c->code.len;
	if(!emit_uoffset(&c->code,0)) goto fail;

	if(!patch_uoffset(&c->code,body_patch,c->code.len)) goto fail;

	CompileResult body_r = compile_block(c,b.data.loop.body);
	if(body_r == COMPILE_FAIL) goto fail;

	if(body_r == COMPILE_REACHABLE){
		if(!state_equal(c,&head)) goto fail;
		if(!emit_op(&c->code,B_JUMP)) goto fail;
		if(!emit_uoffset(&c->code,(uoffset_t)start)) goto fail;
	}

	if(!patch_uoffset(&c->code,end_patch,c->code.len)) goto fail;

	bool had_breaks = false;
	if(!scope_finish(c,c->code.len,&had_breaks)) goto fail;
	(void)had_breaks;

	if(!state_restore(c,&head)) goto fail;
	state_free(&head);
	return COMPILE_REACHABLE;

fail:
	state_free(&head);
	return COMPILE_FAIL;
}

static CompileResult compile_crash_pad(Compiler* c,Block b){
	CompilerState before = {0};
	CompilerState body_state = {0};
	size_t end_patch = (size_t)-1;

	if(!state_save(c,&before)) return COMPILE_FAIL;

	if(!emit_op(&c->code,B_PUSH_CRASH)) goto fail;
	size_t pad_patch = c->code.len;
	if(!emit_uoffset(&c->code,0)) goto fail;
	c->crash_depth++;

	CompileResult body_r = compile_block(c,b.data.crash_pad.body);
	if(body_r == COMPILE_FAIL) goto fail;

	if(body_r == COMPILE_REACHABLE){
		if(!emit_op(&c->code,B_POP_CRASH)) goto fail;
		c->crash_depth--;
		if(!state_save(c,&body_state)) goto fail;
		if(!emit_jump_placeholder(c,&end_patch)) goto fail;
	}

	if(!state_restore(c,&before)) goto fail;
	if(!patch_uoffset(&c->code,pad_patch,c->code.len)) goto fail;

	CompileResult pad_r = compile_block(c,b.data.crash_pad.pad);
	if(pad_r == COMPILE_FAIL) goto fail;
	if(pad_r == COMPILE_REACHABLE){
		if(!emit_op(&c->code,B_CRASH)) goto fail;
		pad_r = COMPILE_UNREACHABLE;
	}

	if(end_patch != (size_t)-1 && !patch_uoffset(&c->code,end_patch,c->code.len)) goto fail;

	if(body_r == COMPILE_REACHABLE){
		if(!state_restore(c,&body_state)) goto fail;
		state_free(&before);
		state_free(&body_state);
		return COMPILE_REACHABLE;
	}

	state_free(&before);
	state_free(&body_state);
	return COMPILE_UNREACHABLE;

fail:
	state_free(&before);
	state_free(&body_state);
	return COMPILE_FAIL;
}

static CompileResult compile_block(Compiler* c,block_idx idx){
	if(idx >= c->func->blocks.len) return COMPILE_FAIL;

	Block b = c->func->blocks.data[idx];
	switch(b.kind){
	case BLOCK_BASIC:
		return compile_basic(c,b);
	case BLOCK_MANY:
		return compile_many(c,b);
	case BLOCK_CRASH:
		if(!emit_op(&c->code,B_CRASH)) return COMPILE_FAIL;
		return COMPILE_UNREACHABLE;
	case BLOCK_HARD_CRASH:
		if(!emit_op(&c->code,B_HARD_CRASH)) return COMPILE_FAIL;
		return COMPILE_UNREACHABLE;
	case BLOCK_CRASH_PAD:
		return compile_crash_pad(c,b);
	case BLOCK_BRANCH:
		return compile_branch(c,b);
	case BLOCK_LOOP:
		return compile_loop(c,b);
	case BLOCK_BREAK:
		return compile_break(c,b.data.level);
	case BLOCK_VAR:
		return compile_var(c,b);
	case BLOCK_DEFER:
		return COMPILE_FAIL;
	}
	return COMPILE_FAIL;
}

VmCode vm_compile_no_defers(const Func* func,CompileContext* ctx){
	Compiler c = {.func=func,.ctx=ctx};
	VmCode fail = {0};
	if(!func || func->blocks.len == 0) return fail;
	if(!type_layout_all(func->types)) return fail;

	c.vars = calloc(func->vars.len,sizeof(*c.vars));
	if(func->vars.len && !c.vars) return fail;
	for(var_idx i=0;i<func->vars.len;i++) c.vars[i].tid = func->vars.data[i].tid;

	CompileResult r = compile_block(&c,0);
	bool ok = r != COMPILE_FAIL;
	if(ok && func->sig.ins.len > (count_t)-1) ok = false;
	if(ok && func->sig.ins.len){
		ok = emit_op(&c.code,B_DROP_N) && emit_count(&c.code,(count_t)func->sig.ins.len);
	}
	if(ok) ok = emit_op(&c.code,B_RET);

	for(size_t i=0;i<c.scopes.len;i++) free(c.scopes.data[i].patches.data);
	free(c.scopes.data);
	free(c.params.data);
	free(c.vars);

	if(!ok){
		free(c.code.data);
		return fail;
	}

	return (VmCode){.data=c.code.data,.len=c.code.len};
}

static bool push_param(VM* vm,void* p){
	if(vm->param_stack.len == vm->param_stack.cap){
		size_t cap = vm->param_stack.cap ? vm->param_stack.cap * 2 : 32;
		void** data = realloc(vm->param_stack.data,cap * sizeof(*vm->param_stack.data));
		if(!data) return false;
		vm->param_stack.data = data;
		vm->param_stack.cap = cap;
	}
	vm->param_stack.data[vm->param_stack.len++] = p;
	return true;
}

static bool push_crash(VM* vm,const ByteCode* p){
	if(vm->crash_stack.len == vm->crash_stack.cap){
		size_t cap = vm->crash_stack.cap ? vm->crash_stack.cap * 2 : 16;
		CrashFrame* data = realloc(vm->crash_stack.data,cap * sizeof(*vm->crash_stack.data));
		if(!data) return false;
		vm->crash_stack.data = data;
		vm->crash_stack.cap = cap;
	}
	vm->crash_stack.data[vm->crash_stack.len++] = (CrashFrame){
		.pc = p,
		.storage_len = vm->storage.len,
		.param_len = vm->param_stack.len,
	};
	return true;
}

static bool storage_resize(VM* vm,offset_t amount){
	if(amount < 0){
		size_t n = (size_t)(-amount);
		if(n > vm->storage.len) return false;
		vm->storage.len -= n;
		return true;
	}
	if(vm->storage.len + (size_t)amount > vm->storage.cap){
		size_t cap = vm->storage.cap ? vm->storage.cap : 64;
		while(cap < vm->storage.len + (size_t)amount) cap *= 2;
		char* data = realloc(vm->storage.data,cap);
		if(!data) return false;
		vm->storage.data = data;
		vm->storage.cap = cap;
	}
	memset(vm->storage.data + vm->storage.len,0,(size_t)amount);
	vm->storage.len += (size_t)amount;
	return true;
}

static bool storage_push_return(VM* vm,ReturnFrame ret){
	if(!storage_resize(vm,(offset_t)sizeof(ret))) return false;
	memcpy(vm->storage.data + vm->storage.len - sizeof(ret),&ret,sizeof(ret));
	return true;
}

static bool storage_pop_return(VM* vm,ReturnFrame* ret){
	if(vm->storage.len < sizeof(*ret)) return false;
	vm->storage.len -= sizeof(*ret);
	memcpy(ret,vm->storage.data + vm->storage.len,sizeof(*ret));
	return true;
}

static VM_RESULT vm_crash(VM* vm,const ByteCode** pc){
	if(vm->crash_stack.len == 0) return VM_CRASH;
	CrashFrame frame = TOP(vm->crash_stack);
	vm->crash_stack.len--;
	if(frame.storage_len > vm->storage.len) return VM_HARD_CRASH;
	if(frame.param_len > vm->param_stack.len) return VM_HARD_CRASH;
	vm->storage.len = frame.storage_len;
	vm->param_stack.len = frame.param_len;
	*pc = frame.pc;
	return VM_OK;
}

VM_RESULT vm_run(VM* vm,const ByteCode* code){
	const ByteCode* base = code;
	const ByteCode* pc = code;
	size_t param_base = 0;
	if(!storage_push_return(vm,(ReturnFrame){0})) return VM_OOM_STORAGE;

	for(;;){
		switch(*pc++){
		case B_DONE:
			return VM_OK;

		case B_RET: {
			ReturnFrame ret = {0};
			if(!storage_pop_return(vm,&ret)) return VM_OK;
			if(!ret.pc) return VM_OK;
			pc = ret.pc;
			base = ret.base;
			param_base = ret.param_base;
			break;
		}

		case B_STORAGE_ADD: {
			offset_t amount;
			pc = read_offset(pc,&amount);
			if(!storage_resize(vm,amount)) return VM_OOM_STORAGE;
			break;
		}

		case B_PUSH_VAR: {
			offset_t offset;
			pc = read_offset(pc,&offset);
			if(offset > 0) return VM_HARD_CRASH;
			if(offset < 0 && (size_t)(-offset) > vm->storage.len) return VM_HARD_CRASH;
			if(!push_param(vm,vm->storage.data + vm->storage.len + offset)) return VM_OOM_PARAM;
			break;
		}

		case B_PUSH_ARG: {
			count_t idx;
			pc = read_count(pc,&idx);
			if(param_base + idx >= vm->param_stack.len) return VM_HARD_CRASH;
			if(!push_param(vm,vm->param_stack.data[param_base + idx])) return VM_OOM_PARAM;
			break;
		}

		case B_PUSH_GLOBAL: {
			const void* ptr;
			pc = read_pointer(pc,&ptr);
			if(!ptr) return VM_HARD_CRASH;
			if(!push_param(vm,(void*)ptr)) return VM_OOM_PARAM;
			break;
		}

		case B_DROP_N: {
			count_t n;
			pc = read_count(pc,&n);
			if(vm->param_stack.len < n) return VM_HARD_CRASH;
			vm->param_stack.len -= n;
			break;
		}

		case B_PUSH_ARR_AT: {
			size_t data_offset, elem_size;
			count_t capacity;
			pc = read_size(pc,&data_offset);
			pc = read_size(pc,&elem_size);
			pc = read_count(pc,&capacity);
			if(vm->param_stack.len < 2) return VM_HARD_CRASH;

			num_t idx;
			memcpy(&idx,TOP(vm->param_stack),sizeof(idx));
			vm->param_stack.len--;

			char* arr = TOP(vm->param_stack);
			count_t len;
			memcpy(&len,arr,sizeof(len));
			if(idx < 0 || (count_t)idx >= len || (count_t)idx >= capacity){
				VM_RESULT r = vm_crash(vm,&pc);
				if(r != VM_OK) return r;
				break;
			}
			TOP(vm->param_stack) = arr + data_offset + elem_size * (size_t)idx;
			break;
		}

		case B_COPY: {
			size_t size;
			pc = read_size(pc,&size);
			if(vm->param_stack.len < 2) return VM_HARD_CRASH;
			void* src = TOP(vm->param_stack);
			vm->param_stack.len--;
			memmove(TOP(vm->param_stack),src,size);
			break;
		}

		case B_ADD:
		case B_SUB:
		case B_MUL:
		case B_DIV:
		case B_AND:
		case B_OR:
		case B_XOR: {
			if(vm->param_stack.len < 2) return VM_HARD_CRASH;

			num_t rhs;
			memcpy(&rhs,TOP(vm->param_stack),sizeof(rhs));
			vm->param_stack.len--;

			num_t lhs;
			memcpy(&lhs,TOP(vm->param_stack),sizeof(lhs));

			ByteCode bop = *(pc - 1);
			if(bop == B_ADD) lhs += rhs;
			else if(bop == B_SUB) lhs -= rhs;
			else if(bop == B_MUL) lhs *= rhs;
			else if(bop == B_DIV){
				if(rhs == 0){
					VM_RESULT r = vm_crash(vm,&pc);
					if(r != VM_OK) return r;
					break;
				}
				lhs /= rhs;
			}
			else if(bop == B_AND) lhs &= rhs;
			else if(bop == B_OR) lhs |= rhs;
			else lhs ^= rhs;

			memcpy(TOP(vm->param_stack),&lhs,sizeof(lhs));
			break;
		}

		case B_BIT_NOT: {
			if(vm->param_stack.len < 1) return VM_HARD_CRASH;
			num_t x;
			memcpy(&x,TOP(vm->param_stack),sizeof(x));
			x = ~x;
			memcpy(TOP(vm->param_stack),&x,sizeof(x));
			break;
		}

		case B_ARR_PUSH: {
			size_t data_offset, elem_size;
			count_t capacity;
			pc = read_size(pc,&data_offset);
			pc = read_size(pc,&elem_size);
			pc = read_count(pc,&capacity);
			if(vm->param_stack.len < 2) return VM_HARD_CRASH;
			void* elem = TOP(vm->param_stack);
			vm->param_stack.len--;
			char* arr = TOP(vm->param_stack);
			count_t len;
			memcpy(&len,arr,sizeof(len));
			if(len >= capacity){
				VM_RESULT r = vm_crash(vm,&pc);
				if(r != VM_OK) return r;
				break;
			}
			memmove(arr + data_offset + elem_size * len,elem,elem_size);
			len++;
			memcpy(arr,&len,sizeof(len));
			break;
		}

		case B_ARR_DROP: {
			if(vm->param_stack.len < 1) return VM_HARD_CRASH;
			char* arr = TOP(vm->param_stack);
			count_t len;
			memcpy(&len,arr,sizeof(len));
			if(len == 0){
				VM_RESULT r = vm_crash(vm,&pc);
				if(r != VM_OK) return r;
				break;
			}
			len--;
			memcpy(arr,&len,sizeof(len));
			break;
		}

		case B_JUMP: {
			uoffset_t target;
			pc = read_uoffset(pc,&target);
			pc = base + target;
			break;
		}

		case B_BRANCH: {
			offset_t cond_offset;
			uoffset_t yes,no;
			pc = read_offset(pc,&cond_offset);
			pc = read_uoffset(pc,&yes);
			pc = read_uoffset(pc,&no);
			if(cond_offset > 0) return VM_HARD_CRASH;
			if(cond_offset < 0 && (size_t)(-cond_offset) > vm->storage.len) return VM_HARD_CRASH;

			num_t cond;
			memcpy(&cond,vm->storage.data + vm->storage.len + cond_offset,sizeof(cond));
			pc = base + (cond ? yes : no);
			break;
		}

		case B_PUSH_CRASH: {
			uoffset_t target;
			pc = read_uoffset(pc,&target);
			if(!push_crash(vm,base + target)) return VM_OOM_CRASH;
			break;
		}

		case B_POP_CRASH:
			if(vm->crash_stack.len == 0) return VM_HARD_CRASH;
			vm->crash_stack.len--;
			break;

		case B_CRASH:
			{
				VM_RESULT r = vm_crash(vm,&pc);
				if(r != VM_OK) return r;
			}
			break;

		case B_HARD_CRASH:
			return VM_HARD_CRASH;

		case B_CALL: {
			const void* target;
			count_t argc;
			pc = read_pointer(pc,&target);
			pc = read_count(pc,&argc);
			if(!target) return VM_HARD_CRASH;
			if(vm->param_stack.len < argc) return VM_HARD_CRASH;
			if(!storage_push_return(vm,(ReturnFrame){.pc=pc,.base=base,.param_base=param_base})) return VM_OOM_STORAGE;
			base = target;
			pc = target;
			param_base = vm->param_stack.len - argc;
			break;
		}

		case B_CALL_NATIVE: {
			if(vm->param_stack.len < 1) return VM_HARD_CRASH;
			VmNativeFunc fn = NULL;
			memcpy(&fn,TOP(vm->param_stack),sizeof(fn));
			vm->param_stack.len--;
			if(!fn) return VM_HARD_CRASH;
			VM_RESULT r = fn(vm);
			if(r == VM_CRASH) r = vm_crash(vm,&pc);
			if(r != VM_OK) return r;
			break;
		}

		default:
			return VM_HARD_CRASH;
		}
	}
}
