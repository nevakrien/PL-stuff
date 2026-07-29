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
	vm_free(&fe->macro_vm);
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
		&& frontend_add_word_op(fe,"CallNative",(OP){.kind = OP_CALL_NATIVE_ON_STACK});
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

static bool compile_token(Frontend* fe,const char* token,size_t len){
	if(len == 1 && (token[0] == '(' || token[0] == ')')) return true;

	const FrontendWord* word = find_word(fe,token,len);
	if(word){
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
			return run_immediate(fe,word->data.immediate.func);
		}
	}

	if(emit_named_var(fe,token,len)) return true;
	if(isdigit((unsigned char)token[0]) || token[0] == '-' || token[0] == '+'){
		return emit_number(fe,token,len);
	}

	fe->error = FRONTEND_UNKNOWN_WORD;
	fe->error_word = token;
	return false;
}

bool frontend_compile_source(Frontend* fe,const char* source){
	if(!frontend_prepare_func(fe)) return false;
	fe->error = FRONTEND_OK;
	fe->error_word = NULL;

	fe->parser = (FrontendParser){.source = source,.cursor = source};
	FrontendName token;
	while(next_token(fe,&token)){
		if(!compile_token(fe,token.data,token.len)) return false;
	}
	return true;
}
