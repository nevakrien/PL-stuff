#include "frontend.h"

#include <ctype.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>

static bool parser_parse_name(void* user,VmParsedName* name);
static bool parser_parse_number(void* user,num_t* number);
static bool parser_parse_type(void* user,type_idx* tid);

size_t frontend_whitespace_len(const char* text){
	const unsigned char* p = (const unsigned char*)text;
	if(p[0] == ' ' || (p[0] >= '\t' && p[0] <= '\r')) return 1;
	if(p[0] == 0xc2 && (p[1] == 0x85 || p[1] == 0xa0)) return 2;
	if(p[0] == 0xe1 && p[1] == 0x9a && p[2] == 0x80) return 3;
	if(p[0] == 0xe2 && p[1] == 0x80 &&
		((p[2] >= 0x80 && p[2] <= 0x8a) || p[2] == 0xa8 || p[2] == 0xa9 || p[2] == 0xaf)) return 3;
	if(p[0] == 0xe2 && p[1] == 0x81 && p[2] == 0x9f) return 3;
	if(p[0] == 0xe3 && p[1] == 0x80 && p[2] == 0x80) return 3;
	return 0;
}

void frontend_init(Frontend* fe,CompileContext* ctx,Func* func){
	*fe = (Frontend){.ctx = ctx,.func = func};
	fe->macro_vm.parser = (VmParser){
		.user = fe,
		.parse_name = parser_parse_name,
		.parse_number = parser_parse_number,
		.parse_type = parser_parse_type,
	};
}

void frontend_free(Frontend* fe){
	for(size_t i=0;i<fe->owned_names.len;i++) free(fe->owned_names.data[i]);
	for(size_t i=0;i<fe->owned_globals.len;i++){
		uoffset_t idx = fe->owned_globals.data[i];
		if(fe->ctx && idx < fe->ctx->globals.len){
			Global* global = &fe->ctx->globals.data[idx];
			if(global->free_func) global->free_func(global->mem);
			global->mem = NULL;
			global->free_func = NULL;
		}
	}
	free(fe->owned_names.data);
	free(fe->owned_globals.data);
	free(fe->words.data);
	free(fe->scopes.data);
	vm_free(&fe->macro_vm);
	vm_free(&fe->interpreter_vm);
	*fe = (Frontend){0};
}

bool frontend_prepare_func(Frontend* fe){
	if(!fe || !fe->func) return false;
	if(fe->func->blocks.data){
		if(fe->current_basic < fe->func->blocks.len &&
			fe->func->blocks.data[fe->current_basic].kind == BLOCK_BASIC) return true;
		if(fe->func->blocks.data[0].kind != BLOCK_BASIC) return false;
		fe->current_basic = 0;
		return true;
	}

	Block* blocks = calloc(1,sizeof(*blocks));
	if(!blocks){
		fe->error = FRONTEND_OOM;
		return false;
	}
	blocks[0] = (Block){.kind = BLOCK_BASIC,.data.basic = {.start = 0,.len = 0}};
	fe->func->blocks = (BlockS){.data = blocks,.len = 1};
	fe->block_cap = 1;
	fe->current_basic = 0;
	return true;
}

//we want to be flexible over whether or not we assume a heap
static bool frontend_add_word(Frontend* fe,FrontendWord word){
	if(fe->words.len >= fe->words.cap){
		size_t cap = fe->words.cap ? fe->words.cap * 2 : 16;
		FrontendWord* data = realloc(fe->words.data,cap * sizeof(*data));
		if(!data){
			fe->error = FRONTEND_OOM;
			return false;
		}
		fe->words.data = data;
		fe->words.cap = cap;
	}
	fe->words.data[fe->words.len++] = word;
	return true;
}

bool frontend_add_word_op(Frontend* fe,const char* name,OP op){
	return frontend_add_word(fe,(FrontendWord){.name = name,.kind = FRONTEND_WORD_OP,.data.op = op});
}

bool frontend_add_word_func(Frontend* fe,const char* name,func_idx idx){
	return frontend_add_word(fe,(FrontendWord){.name = name,.kind = FRONTEND_WORD_FUNC,.data.func = idx});
}

bool frontend_add_word_native(Frontend* fe,const char* name,uoffset_t global){
	return frontend_add_word(fe,(FrontendWord){.name = name,.kind = FRONTEND_WORD_NATIVE,.data.global = global});
}

bool frontend_add_word_immediate(Frontend* fe,const char* name,func_idx idx){
	FrontendImmediate immediate = {.kind = FRONTEND_IMMEDIATE_FUNC,.func = idx};
	return frontend_add_word(fe,(FrontendWord){.name = name,.kind = FRONTEND_WORD_IMMEDIATE,.data.immediate = immediate});
}

bool frontend_add_core_words(Frontend* fe){
	FrontendImmediate var = {.kind = FRONTEND_IMMEDIATE_VAR};
	#define CONTROL_WORD(spelling, control) \
		frontend_add_word(fe,(FrontendWord){.name = (spelling),.kind = FRONTEND_WORD_IMMEDIATE, \
			.data.immediate = {.kind = (control)}})
	return frontend_add_word(fe,(FrontendWord){.name = "Var",.kind = FRONTEND_WORD_IMMEDIATE,.data.immediate = var})
		&& frontend_add_word_op(fe,"Assign",(OP){.kind = OP_ASSIGN})
		&& frontend_add_word_op(fe,"Add",(OP){.kind = OP_ADD_ASSIGN})
		&& frontend_add_word_op(fe,"Sub",(OP){.kind = OP_SUB_ASSIGN})
		&& frontend_add_word_op(fe,"Mul",(OP){.kind = OP_MUL_ASSIGN})
		&& frontend_add_word_op(fe,"Div",(OP){.kind = OP_DIV_ASSIGN})
		&& frontend_add_word_op(fe,"And",(OP){.kind = OP_AND_ASSIGN})
		&& frontend_add_word_op(fe,"Or",(OP){.kind = OP_OR_ASSIGN})
		&& frontend_add_word_op(fe,"Xor",(OP){.kind = OP_XOR_ASSIGN})
		&& frontend_add_word_op(fe,"BitNot",(OP){.kind = OP_BIT_NOT_ASSIGN})
		&& frontend_add_word_op(fe,"CallNative",(OP){.kind = OP_CALL_NATIVE_ON_STACK})
		&& CONTROL_WORD("Loop",FRONTEND_IMMEDIATE_LOOP)
		&& CONTROL_WORD("Again",FRONTEND_IMMEDIATE_AGAIN)
		&& CONTROL_WORD("If",FRONTEND_IMMEDIATE_IF)
		&& CONTROL_WORD("Else",FRONTEND_IMMEDIATE_ELSE)
		&& CONTROL_WORD("Done",FRONTEND_IMMEDIATE_DONE)
		&& CONTROL_WORD("Break",FRONTEND_IMMEDIATE_BREAK)
		&& CONTROL_WORD("Continue",FRONTEND_IMMEDIATE_CONTINUE)
		&& CONTROL_WORD("Start",FRONTEND_IMMEDIATE_START)
		&& CONTROL_WORD("Finally",FRONTEND_IMMEDIATE_FINALLY)
		&& CONTROL_WORD("End",FRONTEND_IMMEDIATE_END)
		&& CONTROL_WORD("Defer",FRONTEND_IMMEDIATE_DEFER);
	#undef CONTROL_WORD
}

bool frontend_emit_op(Frontend* fe,OP op){
	if(!frontend_prepare_func(fe)) return false;
	if(fe->func->ops.len >= fe->op_cap){
		size_t cap = fe->op_cap ? fe->op_cap * 2 : 16;
		OP* data = realloc(fe->func->ops.data,cap * sizeof(*data));
		if(!data){
			fe->error = FRONTEND_OOM;
			return false;
		}
		fe->func->ops.data = data;
		fe->op_cap = cap;
	}
	fe->func->ops.data[fe->func->ops.len++] = op;
	if(fe->scopes.len && TOP(fe->scopes).kind == FRONTEND_SCOPE_IF &&
		TOP(fe->scopes).phase == FRONTEND_SCOPE_CONDITION) return true;
	Block* basic = &fe->func->blocks.data[fe->current_basic];
	basic->data.basic.len = (count_t)(fe->func->ops.len - basic->data.basic.start);
	return true;
}

static bool token_eq(const char* token,size_t len,const char* name){
	return strlen(name) == len && memcmp(token,name,len) == 0;
}

static const FrontendWord* find_word(const Frontend* fe,const char* token,size_t len){
	for(size_t i=0;i<fe->words.len;i++){
		if(token_eq(token,len,fe->words.data[i].name)) return &fe->words.data[i];
	}
	return NULL;
}

static bool emit_named_var(Frontend* fe,const char* token,size_t len){
	size_t argc = fe->func->sig.outs.len + fe->func->sig.ins.len;
	for(size_t i=0;i<fe->func->vars.len;i++){
		const char* name = fe->func->vars.data[i].name;
		if(name && token_eq(token,len,name)){
			OP_KIND kind = i < argc ? OP_PUSH_ARG : OP_PUSH_VAR;
			return frontend_emit_op(fe,(OP){.kind = kind,.extra = (uoffset_t)i});
		}
	}
	return false;
}

static bool run_immediate(Frontend* fe,func_idx idx){
	if(!fe->ctx || idx >= fe->ctx->funcs.len){
		fe->error = FRONTEND_MACRO_COMPILE_FAILED;
		return false;
	}
	VmCode code = vm_compile_no_defers(&fe->ctx->funcs.data[idx],fe->ctx);
	if(!code.data){
		fe->error = FRONTEND_MACRO_COMPILE_FAILED;
		return false;
	}

	fe->macro_vm.user = fe;
	VM_RESULT result = vm_run(&fe->macro_vm,code.data);
	vm_code_free(&code);
	fe->macro_result = result;
	if(result != VM_OK){
		if(fe->error == FRONTEND_OK) fe->error = FRONTEND_MACRO_RUNTIME_FAILED;
		return false;
	}
	return true;
}

static bool next_token(Frontend* fe,FrontendName* token){
	const char* p = fe->parser.cursor;
	size_t whitespace;
	while((whitespace = frontend_whitespace_len(p))) p += whitespace;
	if(!*p) return false;
	const char* start = p;
	if(*p == '(' || *p == ')') p++;
	else while(*p && !frontend_whitespace_len(p) && *p != '(' && *p != ')') p++;
	fe->parser.cursor = p;
	*token = (FrontendName){.data = start,.len = (size_t)(p - start)};
	return true;
}

bool frontend_parse_name(Frontend* fe,FrontendName* name){
	FrontendName token;
	if(!fe || !name || !next_token(fe,&token) ||
		(token.len == 1 && (token.data[0] == '(' || token.data[0] == ')'))){
		if(fe) fe->error = FRONTEND_EXPECTED_NAME;
		return false;
	}
	*name = token;
	return true;
}

bool frontend_parse_number(Frontend* fe,num_t* number){
	FrontendName token;
	if(!fe || !number || !next_token(fe,&token) ||
		(token.len == 1 && (token.data[0] == '(' || token.data[0] == ')'))){
		if(fe) fe->error = FRONTEND_BAD_NUMBER;
		return false;
	}
	errno = 0;
	char* end;
	long long value = strtoll(token.data,&end,10);
	if(errno == ERANGE || end != token.data + token.len){
		fe->error = FRONTEND_BAD_NUMBER;
		fe->error_word = token.data;
		return false;
	}
	*number = (num_t)value;
	return true;
}

static bool append_owned_global(Frontend* fe,Global global,uoffset_t* idx){
	if(!fe->ctx || fe->ctx->globals.len > (uoffset_t)-1){
		fe->error = FRONTEND_OOM;
		return false;
	}
	GlobalS* globals = &fe->ctx->globals;
	if(globals->len >= globals->cap){
		size_t cap = globals->cap ? globals->cap * 2 : 8;
		Global* data = realloc(globals->data,cap * sizeof(*data));
		if(!data) goto oom;
		globals->data = data;
		globals->cap = cap;
	}
	if(fe->owned_globals.len >= fe->owned_globals.cap){
		size_t cap = fe->owned_globals.cap ? fe->owned_globals.cap * 2 : 4;
		uoffset_t* data = realloc(fe->owned_globals.data,cap * sizeof(*data));
		if(!data) goto oom;
		fe->owned_globals.data = data;
		fe->owned_globals.cap = cap;
	}
	*idx = (uoffset_t)globals->len;
	globals->data[globals->len++] = global;
	fe->owned_globals.data[fe->owned_globals.len++] = *idx;
	return true;
oom:
	fe->error = FRONTEND_OOM;
	return false;
}

static bool emit_number(Frontend* fe,const char* token,size_t len){
	errno = 0;
	char* end;
	long long parsed = strtoll(token,&end,10);
	if(end == token) return false;
	if(errno == ERANGE || end != token + len){
		fe->error = FRONTEND_BAD_NUMBER;
		fe->error_word = token;
		return false;
	}
	num_t* value = malloc(sizeof(*value));
	if(!value){ fe->error = FRONTEND_OOM; return false; }
	*value = (num_t)parsed;
	uoffset_t global;
	if(!append_owned_global(fe,(Global){
		.var = {.tid = TYPE_INT_ID,.name = "literal"},
		.mem = value,
		.free_func = free,
	},&global)){
		free(value);
		return false;
	}
	return frontend_emit_op(fe,(OP){.kind = OP_PUSH_GLOBAL,.extra = global});
}

static bool type_matches(const Type* type,TYPE_KIND kind,type_idx elem,count_t capacity){
	if(type->kind != kind) return false;
	if(kind == TYPE_ARRAY) return type->data.array.elem == elem && type->data.array.capacity == capacity;
	return type->data.ref.elem == elem;
}

static bool append_type(Frontend* fe,Type type,type_idx* tid){
	TypeS* types = &fe->func->types;
	if(!fe->type_cap){
		size_t cap = types->len ? types->len * 2 : 4;
		Type* data = malloc(cap * sizeof(*data));
		if(!data) goto oom;
		memcpy(data,types->data,types->len * sizeof(*data));
		types->data = data;
		fe->type_cap = cap;
	}else if(types->len >= fe->type_cap){
		size_t cap = fe->type_cap * 2;
		Type* data = realloc(types->data,cap * sizeof(*data));
		if(!data) goto oom;
		types->data = data;
		fe->type_cap = cap;
	}
	*tid = (type_idx)types->len;
	types->data[types->len++] = type;
	return true;
oom:
	fe->error = FRONTEND_OOM;
	return false;
}

bool frontend_parse_type(Frontend* fe,type_idx* tid){
	FrontendName token;
	if(!tid || !frontend_parse_name(fe,&token)) return false;
	if(token_eq(token.data,token.len,"Int") || token_eq(token.data,token.len,"int")){
		*tid = TYPE_INT_ID;
		return true;
	}
	if(token_eq(token.data,token.len,"Byte") || token_eq(token.data,token.len,"byte")){
		*tid = TYPE_BYTE_ID;
		return true;
	}

	TYPE_KIND kind;
	count_t capacity = 0;
	if(token_eq(token.data,token.len,"Slice")) kind = TYPE_SLICE;
	else if(token_eq(token.data,token.len,"View")) kind = TYPE_VIEW;
	else if(token_eq(token.data,token.len,"Array")){
		kind = TYPE_ARRAY;
		num_t parsed;
		if(!frontend_parse_number(fe,&parsed)) return false;
		if(parsed < 0 || (uintmax_t)parsed > (count_t)-1){
			fe->error = FRONTEND_BAD_NUMBER;
			return false;
		}
		capacity = (count_t)parsed;
	}else{
		for(type_idx i=0;i<fe->func->types.len;i++){
			const char* name = fe->func->types.data[i].name;
			if(name && token_eq(token.data,token.len,name)){
				*tid = i;
				return true;
			}
		}
		fe->error = FRONTEND_UNKNOWN_TYPE;
		fe->error_word = token.data;
		return false;
	}

	type_idx elem;
	if(!frontend_parse_type(fe,&elem)) return false;
	for(type_idx i=0;i<fe->func->types.len;i++){
		if(type_matches(&fe->func->types.data[i],kind,elem,capacity)){
			*tid = i;
			return true;
		}
	}
	Type type = kind == TYPE_ARRAY ? type_array(elem,capacity) :
		kind == TYPE_SLICE ? type_slice(elem) : type_view(elem);
	return append_type(fe,type,tid);
}

static bool grow_blocks(Frontend* fe,size_t add){
	BlockS* blocks = &fe->func->blocks;
	if(blocks->len + add <= fe->block_cap) return true;
	size_t cap = fe->block_cap ? fe->block_cap * 2 : (blocks->len ? blocks->len * 2 : 4);
	while(cap < blocks->len + add) cap *= 2;
	Block* data;
	if(fe->block_cap){
		data = realloc(blocks->data,cap * sizeof(*data));
	}else{
		data = malloc(cap * sizeof(*data));
		if(data) memcpy(data,blocks->data,blocks->len * sizeof(*data));
	}
	if(!data){ fe->error = FRONTEND_OOM; return false; }
	blocks->data = data;
	fe->block_cap = cap;
	return true;
}

static bool push_scope(Frontend* fe,FrontendScope scope){
	if(fe->scopes.len >= fe->scopes.cap){
		size_t cap = fe->scopes.cap ? fe->scopes.cap * 2 : 8;
		FrontendScope* data = realloc(fe->scopes.data,cap * sizeof(*data));
		if(!data){ fe->error = FRONTEND_OOM; return false; }
		fe->scopes.data = data;
		fe->scopes.cap = cap;
	}
	fe->scopes.data[fe->scopes.len++] = scope;
	return true;
}

// Replace the current basic block with: old basic; middle; new basic.
static bool wrap_current(Frontend* fe,Block middle,block_idx* middle_idx,block_idx* after_idx){
	if(fe->func->blocks.len + 5 >= BLOCK_INVALID || !grow_blocks(fe,5)) return false;
	block_idx slot = fe->current_basic;
	block_idx before = (block_idx)fe->func->blocks.len++;
	block_idx child = (block_idx)fe->func->blocks.len++;
	block_idx child_chain = (block_idx)fe->func->blocks.len++;
	block_idx after = (block_idx)fe->func->blocks.len++;
	block_idx after_chain = (block_idx)fe->func->blocks.len++;

	fe->func->blocks.data[before] = fe->func->blocks.data[slot];
	fe->func->blocks.data[child] = middle;
	fe->func->blocks.data[child_chain] = (Block){
		.kind = BLOCK_CHAIN,.data.chain = {.cur = child,.next = after_chain},
	};
	fe->func->blocks.data[after] = (Block){
		.kind = BLOCK_BASIC,.data.basic = {.start = (op_idx)fe->func->ops.len},
	};
	fe->func->blocks.data[after_chain] = (Block){
		.kind = BLOCK_CHAIN,.data.chain = {.cur = after,.next = BLOCK_INVALID},
	};
	fe->func->blocks.data[slot] = (Block){
		.kind = BLOCK_MANY,.data.chain = {.cur = before,.next = child_chain},
	};
	*middle_idx = child;
	*after_idx = after;
	return true;
}

static bool append_basic_block(Frontend* fe,block_idx* idx){
	if(fe->func->blocks.len + 1 >= BLOCK_INVALID || !grow_blocks(fe,1)) return false;
	*idx = (block_idx)fe->func->blocks.len++;
	fe->func->blocks.data[*idx] = (Block){
		.kind = BLOCK_BASIC,.data.basic = {.start = (op_idx)fe->func->ops.len},
	};
	return true;
}

static bool bad_control(Frontend* fe){
	fe->error = FRONTEND_BAD_CONTROL_FLOW;
	fe->error_word = fe->current_token.data;
	return false;
}

static bool begin_loop(Frontend* fe){
	block_idx loop_body;
	block_idx body;
	if(!append_basic_block(fe,&body)) return false;
	if(fe->func->blocks.len + 1 >= BLOCK_INVALID || !grow_blocks(fe,1)) return false;
	loop_body = (block_idx)fe->func->blocks.len++;
	fe->func->blocks.data[loop_body] = (Block){
		.kind = BLOCK_MANY,.data.chain = {.cur = body,.next = BLOCK_INVALID},
	};

	block_idx loop;
	block_idx after;
	count_t parent_depth = fe->scope_depth;
	if(!wrap_current(fe,(Block){.kind = BLOCK_LOOP,.data.loop.body = loop_body},&loop,&after)) return false;
	if(!push_scope(fe,(FrontendScope){
		.kind = FRONTEND_SCOPE_LOOP,
		.phase = FRONTEND_SCOPE_BODY,
		.block = loop,
		.first = loop_body,
		.after = after,
		.parent_depth = parent_depth,
		.break_depth = parent_depth + 2,
		.continue_depth = parent_depth + 3,
	})) return false;
	fe->current_basic = body;
	fe->scope_depth = parent_depth + 3;
	return true;
}

static bool end_loop(Frontend* fe){
	if(!fe->scopes.len || TOP(fe->scopes).kind != FRONTEND_SCOPE_LOOP) return bad_control(fe);
	FrontendScope scope = TOP(fe->scopes);
	fe->scopes.len--;
	fe->current_basic = scope.after;
	fe->func->blocks.data[scope.after].data.basic.start = (op_idx)fe->func->ops.len;
	fe->scope_depth = scope.parent_depth + 1;
	return true;
}

static bool begin_if(Frontend* fe){
	block_idx yes;
	block_idx no;
	if(!append_basic_block(fe,&yes) || !append_basic_block(fe,&no)) return false;
	block_idx branch;
	block_idx after;
	count_t parent_depth = fe->scope_depth;
	if(!wrap_current(fe,(Block){
		.kind = BLOCK_BRANCH,
		.data.branch = {
			.cond = {.start = (op_idx)fe->func->ops.len},
			.yes = yes,
			.no = no,
		},
	},&branch,&after)) return false;
	if(!push_scope(fe,(FrontendScope){
		.kind = FRONTEND_SCOPE_IF,
		.phase = FRONTEND_SCOPE_WAIT_OPEN,
		.block = branch,
		.first = yes,
		.second = no,
		.after = after,
		.parent_depth = parent_depth,
	})) return false;
	fe->current_basic = yes;
	fe->scope_depth = parent_depth + 1;
	return true;
}

static bool begin_else(Frontend* fe){
	if(!fe->scopes.len || TOP(fe->scopes).kind != FRONTEND_SCOPE_IF ||
		TOP(fe->scopes).phase != FRONTEND_SCOPE_BODY) return bad_control(fe);
	FrontendScope* scope = &TOP(fe->scopes);
	scope->phase = FRONTEND_SCOPE_ELSE_BODY;
	fe->current_basic = scope->second;
	fe->func->blocks.data[scope->second].data.basic.start = (op_idx)fe->func->ops.len;
	fe->scope_depth = scope->parent_depth + 1;
	return true;
}

static bool end_if(Frontend* fe){
	if(!fe->scopes.len || TOP(fe->scopes).kind != FRONTEND_SCOPE_IF ||
		(TOP(fe->scopes).phase != FRONTEND_SCOPE_BODY &&
		 TOP(fe->scopes).phase != FRONTEND_SCOPE_ELSE_BODY)) return bad_control(fe);
	FrontendScope scope = TOP(fe->scopes);
	if(scope.phase == FRONTEND_SCOPE_BODY){
		fe->func->blocks.data[scope.second].data.basic.start = (op_idx)fe->func->ops.len;
	}
	fe->scopes.len--;
	fe->current_basic = scope.after;
	fe->func->blocks.data[scope.after].data.basic.start = (op_idx)fe->func->ops.len;
	fe->scope_depth = scope.parent_depth + 1;
	return true;
}

static bool begin_epilogue(Frontend* fe,bool implicit_end){
	block_idx next;
	block_idx cleanup;
	if(!append_basic_block(fe,&next) || !append_basic_block(fe,&cleanup)) return false;
	block_idx defer;
	block_idx after;
	count_t parent_depth = fe->scope_depth;
	if(!wrap_current(fe,(Block){
		.kind = BLOCK_DEFER,.data.defer = {.next = next,.defer = cleanup},
	},&defer,&after)) return false;
	if(!push_scope(fe,(FrontendScope){
		.kind = FRONTEND_SCOPE_EPILOGUE,
		.phase = implicit_end ? FRONTEND_SCOPE_CLEANUP_WAIT_OPEN : FRONTEND_SCOPE_BODY,
		.block = defer,
		.first = next,
		.second = cleanup,
		.after = after,
		.parent_depth = parent_depth,
		.implicit_end = implicit_end,
	})) return false;
	fe->current_basic = implicit_end ? cleanup : next;
	// Lowering adds a BLOCK_MANY around the defer. Count that scope now so a
	// break crossing the defer still reaches its original loop destination.
	fe->scope_depth = parent_depth + 2;
	return true;
}

static bool begin_cleanup(Frontend* fe){
	if(!fe->scopes.len || TOP(fe->scopes).kind != FRONTEND_SCOPE_EPILOGUE ||
		TOP(fe->scopes).implicit_end || TOP(fe->scopes).phase != FRONTEND_SCOPE_BODY) return bad_control(fe);
	FrontendScope* scope = &TOP(fe->scopes);
	scope->phase = FRONTEND_SCOPE_CLEANUP;
	fe->current_basic = scope->second;
	fe->func->blocks.data[scope->second].data.basic.start = (op_idx)fe->func->ops.len;
	fe->scope_depth = scope->parent_depth + 2;
	return true;
}

static bool finish_epilogue(Frontend* fe){
	if(!fe->scopes.len || TOP(fe->scopes).kind != FRONTEND_SCOPE_EPILOGUE ||
		TOP(fe->scopes).implicit_end || TOP(fe->scopes).phase != FRONTEND_SCOPE_CLEANUP) return bad_control(fe);
	FrontendScope scope = TOP(fe->scopes);
	fe->scopes.len--;
	fe->current_basic = scope.after;
	fe->func->blocks.data[scope.after].data.basic.start = (op_idx)fe->func->ops.len;
	fe->scope_depth = scope.parent_depth + 1;
	return true;
}

static void finish_implicit_epilogues(Frontend* fe){
	while(fe->scopes.len && TOP(fe->scopes).kind == FRONTEND_SCOPE_EPILOGUE &&
		TOP(fe->scopes).implicit_end && TOP(fe->scopes).phase == FRONTEND_SCOPE_BODY){
		FrontendScope scope = TOP(fe->scopes);
		fe->scopes.len--;
		fe->current_basic = scope.after;
		fe->func->blocks.data[scope.after].data.basic.start = (op_idx)fe->func->ops.len;
		fe->scope_depth = scope.parent_depth + 1;
	}
}

static bool cleanup_exit_is_legal(const Frontend* fe,size_t loop_index){
	for(size_t i=loop_index;i<fe->scopes.len;i++){
		const FrontendScope* scope = &fe->scopes.data[i];
		if(scope->kind == FRONTEND_SCOPE_EPILOGUE && scope->phase == FRONTEND_SCOPE_CLEANUP) return false;
	}
	return true;
}

static bool emit_loop_exit(Frontend* fe,bool is_continue){
	size_t loop_index = fe->scopes.len;
	while(loop_index && fe->scopes.data[loop_index - 1].kind != FRONTEND_SCOPE_LOOP) loop_index--;
	if(!loop_index || !cleanup_exit_is_legal(fe,loop_index)) return bad_control(fe);
	FrontendScope loop = fe->scopes.data[loop_index - 1];
	count_t target_depth = is_continue ? loop.continue_depth : loop.break_depth;
	count_t break_depth = fe->scope_depth + 1;
	if(target_depth > break_depth) return bad_control(fe);
	count_t level = break_depth - target_depth + 1;
	block_idx jump;
	block_idx after;
	if(!wrap_current(fe,(Block){.kind = BLOCK_BREAK,.data.level = level},&jump,&after)) return false;
	(void)jump;
	fe->current_basic = after;
	fe->scope_depth++;
	return true;
}

static bool handle_delimiter(Frontend* fe,bool opening){
	if(!fe->scopes.len) return true;
	FrontendScope* scope = &TOP(fe->scopes);
	if(scope->phase == FRONTEND_SCOPE_WAIT_OPEN || scope->phase == FRONTEND_SCOPE_CLEANUP_WAIT_OPEN){
		if(!opening) return bad_control(fe);
		scope->delimiter_depth = 1;
		scope->phase = scope->kind == FRONTEND_SCOPE_IF ? FRONTEND_SCOPE_CONDITION : FRONTEND_SCOPE_CLEANUP;
		return true;
	}
	if(scope->delimiter_depth){
		if(opening){ scope->delimiter_depth++; return true; }
		if(--scope->delimiter_depth) return true;
		if(scope->kind == FRONTEND_SCOPE_IF){
			Block* branch = &fe->func->blocks.data[scope->block];
			branch->data.branch.cond.len = (count_t)(fe->func->ops.len - branch->data.branch.cond.start);
			scope->phase = FRONTEND_SCOPE_BODY;
			fe->current_basic = scope->first;
			fe->func->blocks.data[scope->first].data.basic.start = (op_idx)fe->func->ops.len;
			return true;
		}

		// A prefix Defer captures its cleanup first, then wraps the remaining body.
		scope->phase = FRONTEND_SCOPE_BODY;
		fe->current_basic = scope->first;
		fe->func->blocks.data[scope->first].data.basic.start = (op_idx)fe->func->ops.len;
		return true;
	}
	return true;
}

static bool append_var(Frontend* fe,Var var,var_idx* idx){
	VarS* vars = &fe->func->vars;
	if(!fe->var_cap){
		size_t cap = vars->len ? vars->len * 2 : 4;
		Var* data = malloc(cap * sizeof(*data));
		if(!data) goto oom;
		memcpy(data,vars->data,vars->len * sizeof(*data));
		vars->data = data;
		fe->var_cap = cap;
	}else if(vars->len >= fe->var_cap){
		size_t cap = fe->var_cap * 2;
		Var* data = realloc(vars->data,cap * sizeof(*data));
		if(!data) goto oom;
		vars->data = data;
		fe->var_cap = cap;
	}
	*idx = (var_idx)vars->len;
	vars->data[vars->len++] = var;
	return true;
oom:
	fe->error = FRONTEND_OOM;
	return false;
}

bool frontend_declare_var(Frontend* fe,type_idx tid,FrontendName name){
	if(!fe || !type_idx_valid(fe->func->types,tid)) return false;
	if(fe->func->vars.len > (var_idx)-1){
		fe->error = FRONTEND_OOM;
		return false;
	}
	for(size_t i=0;i<fe->func->vars.len;i++){
		const char* old = fe->func->vars.data[i].name;
		if(old && token_eq(name.data,name.len,old)){
			fe->error = FRONTEND_DUPLICATE_VAR;
			return false;
		}
	}
	char* owned = malloc(name.len + 1);
	if(!owned){ fe->error = FRONTEND_OOM; return false; }
	memcpy(owned,name.data,name.len);
	owned[name.len] = 0;
	if(fe->owned_names.len >= fe->owned_names.cap){
		size_t cap = fe->owned_names.cap ? fe->owned_names.cap * 2 : 4;
		char** data = realloc(fe->owned_names.data,cap * sizeof(*data));
		if(!data){ free(owned); fe->error = FRONTEND_OOM; return false; }
		fe->owned_names.data = data;
		fe->owned_names.cap = cap;
	}
	var_idx var;
	if(!append_var(fe,(Var){.tid = tid,.name = owned},&var)){ free(owned); return false; }
	fe->owned_names.data[fe->owned_names.len++] = owned;

	if(!grow_blocks(fe,4)) return false;
	block_idx slot = fe->current_basic;
	block_idx before = (block_idx)fe->func->blocks.len++;
	block_idx var_block = (block_idx)fe->func->blocks.len++;
	block_idx chain = (block_idx)fe->func->blocks.len++;
	block_idx body = (block_idx)fe->func->blocks.len++;
	fe->func->blocks.data[before] = fe->func->blocks.data[slot];
	fe->func->blocks.data[body] = (Block){.kind = BLOCK_BASIC,.data.basic = {.start = (op_idx)fe->func->ops.len}};
	fe->func->blocks.data[var_block] = (Block){.kind = BLOCK_VAR,.data.var = {.var = var,.body = body}};
	fe->func->blocks.data[chain] = (Block){.kind = BLOCK_CHAIN,.data.chain = {.cur = var_block,.next = BLOCK_INVALID}};
	fe->func->blocks.data[slot] = (Block){.kind = BLOCK_MANY,.data.chain = {.cur = before,.next = chain}};
	fe->current_basic = body;
	fe->scope_depth += 2;
	return true;
}

static bool parser_parse_name(void* user,VmParsedName* name){
	return frontend_parse_name(user,name);
}

static bool parser_parse_number(void* user,num_t* number){
	return frontend_parse_number(user,number);
}

static bool parser_parse_type(void* user,type_idx* tid){
	return frontend_parse_type(user,tid);
}

static bool run_var(Frontend* fe){
	type_idx tid;
	FrontendName name;
	return frontend_parse_type(fe,&tid) && frontend_parse_name(fe,&name) && frontend_declare_var(fe,tid,name);
}

static bool run_control(Frontend* fe,FrontendImmediateKind kind){
	if(fe->scopes.len){
		FrontendScopePhase phase = TOP(fe->scopes).phase;
		if(phase == FRONTEND_SCOPE_WAIT_OPEN || phase == FRONTEND_SCOPE_CONDITION ||
			phase == FRONTEND_SCOPE_CLEANUP_WAIT_OPEN) return bad_control(fe);
	}
	if(kind == FRONTEND_IMMEDIATE_AGAIN || kind == FRONTEND_IMMEDIATE_ELSE ||
		kind == FRONTEND_IMMEDIATE_DONE || kind == FRONTEND_IMMEDIATE_FINALLY ||
		kind == FRONTEND_IMMEDIATE_END) finish_implicit_epilogues(fe);
	switch(kind){
	case FRONTEND_IMMEDIATE_LOOP: return begin_loop(fe);
	case FRONTEND_IMMEDIATE_AGAIN: return end_loop(fe);
	case FRONTEND_IMMEDIATE_IF: return begin_if(fe);
	case FRONTEND_IMMEDIATE_ELSE: return begin_else(fe);
	case FRONTEND_IMMEDIATE_DONE: return end_if(fe);
	case FRONTEND_IMMEDIATE_BREAK: return emit_loop_exit(fe,false);
	case FRONTEND_IMMEDIATE_CONTINUE: return emit_loop_exit(fe,true);
	case FRONTEND_IMMEDIATE_START: return begin_epilogue(fe,false);
	case FRONTEND_IMMEDIATE_FINALLY: return begin_cleanup(fe);
	case FRONTEND_IMMEDIATE_END: return finish_epilogue(fe);
	case FRONTEND_IMMEDIATE_DEFER: return begin_epilogue(fe,true);
	case FRONTEND_IMMEDIATE_FUNC:
	case FRONTEND_IMMEDIATE_VAR:
		break;
	}
	return false;
}

static bool ready_for_word(Frontend* fe){
	if(!fe->scopes.len) return true;
	FrontendScopePhase phase = TOP(fe->scopes).phase;
	return phase != FRONTEND_SCOPE_WAIT_OPEN && phase != FRONTEND_SCOPE_CLEANUP_WAIT_OPEN;
}

static bool engage_word(Frontend* fe,const FrontendWord* word){
	if(!ready_for_word(fe)) return bad_control(fe);
	switch(word->kind){
	case FRONTEND_WORD_OP:
		return frontend_emit_op(fe,word->data.op);
	case FRONTEND_WORD_FUNC:
		return frontend_emit_op(fe,(OP){.kind = OP_CALL,.extra = word->data.func});
	case FRONTEND_WORD_NATIVE:
		return frontend_emit_op(fe,(OP){.kind = OP_PUSH_GLOBAL,.extra = word->data.global})
			&& frontend_emit_op(fe,(OP){.kind = OP_CALL_NATIVE_ON_STACK});
	case FRONTEND_WORD_IMMEDIATE:
		if(word->data.immediate.kind == FRONTEND_IMMEDIATE_VAR) return run_var(fe);
		if(word->data.immediate.kind == FRONTEND_IMMEDIATE_FUNC){
			return run_immediate(fe,word->data.immediate.func);
		}
		return run_control(fe,word->data.immediate.kind);
	}
	return false;
}

static bool compile_fallback(Frontend* fe,const char* token,size_t len){
	if(!ready_for_word(fe)) return bad_control(fe);
	if(emit_named_var(fe,token,len)) return true;
	if(isdigit((unsigned char)token[0]) || token[0] == '-' || token[0] == '+'){
		return emit_number(fe,token,len);
	}

	fe->error = FRONTEND_UNKNOWN_WORD;
	fe->error_word = token;
	return false;
}

static VM_RESULT interpreter_next_token(VM* vm){
	if(!vm || !vm->user || !vm->param_stack.len) return VM_INVALID_ARG;
	Frontend* fe = vm->user;
	num_t* has_token = TOP(vm->param_stack);
	*has_token = next_token(fe,&fe->current_token);
	return VM_OK;
}

static VM_RESULT interpreter_lookup_word(VM* vm){
	if(!vm || !vm->user || !vm->param_stack.len) return VM_INVALID_ARG;
	Frontend* fe = vm->user;
	num_t* handle = TOP(vm->param_stack);
	FrontendName token = fe->current_token;
	if(token.len == 1 && (token.data[0] == '(' || token.data[0] == ')')){
		*handle = token.data[0] == '(' ? -1 : -2;
		return VM_OK;
	}
	const FrontendWord* word = find_word(fe,token.data,token.len);
	*handle = word ? (num_t)(word - fe->words.data) + 1 : 0;
	return VM_OK;
}

static VM_RESULT interpreter_engage_word(VM* vm){
	if(!vm || !vm->user || !vm->param_stack.len) return VM_INVALID_ARG;
	Frontend* fe = vm->user;
	num_t handle = *(const num_t*)TOP(vm->param_stack);
	if(handle == -1 || handle == -2){
		return handle_delimiter(fe,handle == -1) ? VM_OK : VM_HARD_CRASH;
	}
	if(handle <= 0 || (uintmax_t)(handle - 1) >= fe->words.len) return VM_INVALID_ARG;
	return engage_word(fe,&fe->words.data[handle - 1]) ? VM_OK : VM_HARD_CRASH;
}

static VM_RESULT interpreter_fallback(VM* vm){
	if(!vm || !vm->user) return VM_INVALID_ARG;
	Frontend* fe = vm->user;
	FrontendName token = fe->current_token;
	return compile_fallback(fe,token.data,token.len) ? VM_OK : VM_HARD_CRASH;
}

static VmCode compile_interpreter(void){
	enum {
		INTERPRETER_NEXT_TOKEN_TYPE = 2,
		INTERPRETER_LOOKUP_WORD_TYPE,
		INTERPRETER_ENGAGE_WORD_TYPE,
		INTERPRETER_FALLBACK_TYPE,
	};
	enum {
		INTERPRETER_NEXT_TOKEN_GLOBAL,
		INTERPRETER_LOOKUP_WORD_GLOBAL,
		INTERPRETER_ENGAGE_WORD_GLOBAL,
		INTERPRETER_FALLBACK_GLOBAL,
	};
	enum {
		INTERPRETER_ROOT,
		INTERPRETER_WORD_VAR,
		INTERPRETER_LOOP,
		INTERPRETER_LOOP_BODY,
		INTERPRETER_READ,
		INTERPRETER_BODY_CHAIN,
		INTERPRETER_HAS_TOKEN,
		INTERPRETER_LOOKUP_BODY,
		INTERPRETER_LOOKUP,
		INTERPRETER_LOOKUP_CHAIN,
		INTERPRETER_WORD_FOUND,
		INTERPRETER_ENGAGE_WORD,
		INTERPRETER_FALLBACK,
		INTERPRETER_BREAK,
		INTERPRETER_BLOCK_COUNT,
	};

	static Var next_token_outs[] = {{.tid = TYPE_INT_ID,.name = "has_token"}};
	static Var lookup_word_outs[] = {{.tid = TYPE_INT_ID,.name = "word_handle"}};
	static SigInput engage_word_ins[] = {{.var = {.tid = TYPE_INT_ID,.name = "word_handle"}}};
	static Type types[] = {
		[0] = {.kind = TYPE_INT,.name = "int"},
		[1] = {.kind = TYPE_BYTE,.name = "byte"},
		[INTERPRETER_NEXT_TOKEN_TYPE] = {
			.kind = TYPE_NATIVE_FUNC_POINTER,
			.name = "next_token",
			.data.sig.outs = {.data = next_token_outs,.len = 1},
		},
		[INTERPRETER_LOOKUP_WORD_TYPE] = {
			.kind = TYPE_NATIVE_FUNC_POINTER,
			.name = "lookup_word",
			.data.sig.outs = {.data = lookup_word_outs,.len = 1},
		},
		[INTERPRETER_ENGAGE_WORD_TYPE] = {
			.kind = TYPE_NATIVE_FUNC_POINTER,
			.name = "engage_word",
			.data.sig.ins = {.data = engage_word_ins,.len = 1},
		},
		[INTERPRETER_FALLBACK_TYPE] = {
			.kind = TYPE_NATIVE_FUNC_POINTER,
			.name = "interpret_fallback",
		},
	};
	static Var vars[] = {
		{.tid = TYPE_INT_ID,.name = "has_token"},
		{.tid = TYPE_INT_ID,.name = "word_handle"},
	};
	static OP ops[] = {
		{.kind = OP_PUSH_VAR,.extra = 0},
		{.kind = OP_PUSH_GLOBAL,.extra = INTERPRETER_NEXT_TOKEN_GLOBAL},
		{.kind = OP_CALL_NATIVE_ON_STACK},
		{.kind = OP_PUSH_VAR,.extra = 0},
		{.kind = OP_PUSH_VAR,.extra = 1},
		{.kind = OP_PUSH_GLOBAL,.extra = INTERPRETER_LOOKUP_WORD_GLOBAL},
		{.kind = OP_CALL_NATIVE_ON_STACK},
		{.kind = OP_PUSH_VAR,.extra = 1},
		{.kind = OP_PUSH_VAR,.extra = 1},
		{.kind = OP_PUSH_GLOBAL,.extra = INTERPRETER_ENGAGE_WORD_GLOBAL},
		{.kind = OP_CALL_NATIVE_ON_STACK},
		{.kind = OP_PUSH_GLOBAL,.extra = INTERPRETER_FALLBACK_GLOBAL},
		{.kind = OP_CALL_NATIVE_ON_STACK},
	};
	static Block blocks[INTERPRETER_BLOCK_COUNT] = {
		[INTERPRETER_ROOT] = {
			.kind = BLOCK_VAR,
			.data.var = {.var = 0,.body = INTERPRETER_WORD_VAR},
		},
		[INTERPRETER_WORD_VAR] = {
			.kind = BLOCK_VAR,
			.data.var = {.var = 1,.body = INTERPRETER_LOOP},
		},
		[INTERPRETER_LOOP] = {
			.kind = BLOCK_LOOP,
			.data.loop.body = INTERPRETER_LOOP_BODY,
		},
		[INTERPRETER_LOOP_BODY] = {
			.kind = BLOCK_MANY,
			.data.chain = {.cur = INTERPRETER_READ,.next = INTERPRETER_BODY_CHAIN},
		},
		[INTERPRETER_READ] = {
			.kind = BLOCK_BASIC,
			.data.basic = {.start = 0,.len = 3},
		},
		[INTERPRETER_BODY_CHAIN] = {
			.kind = BLOCK_CHAIN,
			.data.chain = {.cur = INTERPRETER_HAS_TOKEN,.next = BLOCK_INVALID},
		},
		[INTERPRETER_HAS_TOKEN] = {
			.kind = BLOCK_BRANCH,
			.data.branch = {
				.cond = {.start = 3,.len = 1},
				.yes = INTERPRETER_LOOKUP_BODY,
				.no = INTERPRETER_BREAK,
			},
		},
		[INTERPRETER_LOOKUP_BODY] = {
			.kind = BLOCK_MANY,
			.data.chain = {.cur = INTERPRETER_LOOKUP,.next = INTERPRETER_LOOKUP_CHAIN},
		},
		[INTERPRETER_LOOKUP] = {
			.kind = BLOCK_BASIC,
			.data.basic = {.start = 4,.len = 3},
		},
		[INTERPRETER_LOOKUP_CHAIN] = {
			.kind = BLOCK_CHAIN,
			.data.chain = {.cur = INTERPRETER_WORD_FOUND,.next = BLOCK_INVALID},
		},
		[INTERPRETER_WORD_FOUND] = {
			.kind = BLOCK_BRANCH,
			.data.branch = {
				.cond = {.start = 7,.len = 1},
				.yes = INTERPRETER_ENGAGE_WORD,
				.no = INTERPRETER_FALLBACK,
			},
		},
		[INTERPRETER_ENGAGE_WORD] = {
			.kind = BLOCK_BASIC,
			.data.basic = {.start = 8,.len = 3},
		},
		[INTERPRETER_FALLBACK] = {
			.kind = BLOCK_BASIC,
			.data.basic = {.start = 11,.len = 2},
		},
		[INTERPRETER_BREAK] = {
			.kind = BLOCK_BREAK,
			.data.level = 2,
		},
	};
	static VmNativeFunc next_token_fn = interpreter_next_token;
	static VmNativeFunc lookup_word_fn = interpreter_lookup_word;
	static VmNativeFunc engage_word_fn = interpreter_engage_word;
	static VmNativeFunc fallback_fn = interpreter_fallback;
	Global globals[] = {
		[INTERPRETER_NEXT_TOKEN_GLOBAL] = {
			.var = {.tid = INTERPRETER_NEXT_TOKEN_TYPE,.name = "next_token"},
			.mem = &next_token_fn,
		},
		[INTERPRETER_LOOKUP_WORD_GLOBAL] = {
			.var = {.tid = INTERPRETER_LOOKUP_WORD_TYPE,.name = "lookup_word"},
			.mem = &lookup_word_fn,
		},
		[INTERPRETER_ENGAGE_WORD_GLOBAL] = {
			.var = {.tid = INTERPRETER_ENGAGE_WORD_TYPE,.name = "engage_word"},
			.mem = &engage_word_fn,
		},
		[INTERPRETER_FALLBACK_GLOBAL] = {
			.var = {.tid = INTERPRETER_FALLBACK_TYPE,.name = "interpret_fallback"},
			.mem = &fallback_fn,
		},
	};
	CompileContext ctx = {.globals = {.data = globals,.len = 4,.cap = 4}};
	Func interpreter = {
		.name = "frontend_interpreter",
		.types = {.data = types,.len = sizeof(types) / sizeof(types[0])},
		.blocks = {.data = blocks,.len = INTERPRETER_BLOCK_COUNT},
		.ops = {.data = ops,.len = sizeof(ops) / sizeof(ops[0])},
		.vars = {.data = vars,.len = 2},
	};
	return vm_compile_no_defers(&interpreter,&ctx);
}

bool frontend_compile_source(Frontend* fe,const char* source){
	if(!frontend_prepare_func(fe)) return false;
	fe->error = FRONTEND_OK;
	fe->error_word = NULL;

	fe->parser = (FrontendParser){.source = source,.cursor = source};
	fe->current_token = (FrontendName){0};
	VmCode code = compile_interpreter();
	if(!code.data){
		fe->error = FRONTEND_INTERPRETER_COMPILE_FAILED;
		return false;
	}

	fe->interpreter_vm.user = fe;
	fe->interpreter_vm.storage.len = 0;
	fe->interpreter_vm.param_stack.len = 0;
	fe->interpreter_vm.crash_stack.len = 0;
	fe->interpreter_result = vm_run(&fe->interpreter_vm,code.data);
	vm_code_free(&code);
	fe->interpreter_vm.storage.len = 0;
	fe->interpreter_vm.param_stack.len = 0;
	fe->interpreter_vm.crash_stack.len = 0;
	if(fe->interpreter_result != VM_OK){
		if(fe->error == FRONTEND_OK) fe->error = FRONTEND_INTERPRETER_RUNTIME_FAILED;
		return false;
	}
	finish_implicit_epilogues(fe);
	if(fe->scopes.len){
		fe->error = FRONTEND_BAD_CONTROL_FLOW;
		return false;
	}
	return true;
}
