#include "frontend.h"

#include <ctype.h>
#include <stdlib.h>
#include <string.h>

void frontend_init(Frontend* fe,CompileContext* ctx,Func* func){
	*fe = (Frontend){.ctx = ctx,.func = func};
}

void frontend_free(Frontend* fe){
	free(fe->words.data);
	vm_free(&fe->macro_vm);
	*fe = (Frontend){0};
}

bool frontend_prepare_func(Frontend* fe){
	if(!fe || !fe->func) return false;
	if(fe->func->blocks.data) return true;

	Block* blocks = calloc(1,sizeof(*blocks));
	if(!blocks){
		fe->error = FRONTEND_OOM;
		return false;
	}
	blocks[0] = (Block){.kind = BLOCK_BASIC,.data.basic = {.start = 0,.len = 0}};
	fe->func->blocks = (BlockS){.data = blocks,.len = 1};
	return true;
}

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

bool frontend_add_word_immediate(Frontend* fe,const char* name,const Func* macro){
	return frontend_add_word(fe,(FrontendWord){.name = name,.kind = FRONTEND_WORD_IMMEDIATE,.data.immediate = macro});
}

bool frontend_add_core_words(Frontend* fe){
	return frontend_add_word_op(fe,"Assign",(OP){.kind = OP_ASSIGN})
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
	fe->func->blocks.data[0].data.basic.len = (count_t)fe->func->ops.len;
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
	for(size_t i=0;i<fe->func->vars.len;i++){
		const char* name = fe->func->vars.data[i].name;
		if(name && token_eq(token,len,name)){
			return frontend_emit_op(fe,(OP){.kind = OP_PUSH_ARG,.extra = (uoffset_t)i});
		}
	}
	return false;
}

static bool run_immediate(Frontend* fe,const Func* macro){
	VmCode code = vm_compile_no_defers(macro,fe->ctx);
	if(!code.data){
		fe->error = FRONTEND_MACRO_COMPILE_FAILED;
		return false;
	}

	fe->macro_vm.user = fe;
	VM_RESULT result = vm_run(&fe->macro_vm,code.data);
	vm_code_free(&code);
	fe->macro_result = result;
	if(result != VM_OK){
		fe->error = FRONTEND_MACRO_RUNTIME_FAILED;
		return false;
	}
	return true;
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
		case FRONTEND_WORD_IMMEDIATE:
			return run_immediate(fe,word->data.immediate);
		}
	}

	if(emit_named_var(fe,token,len)) return true;

	fe->error = FRONTEND_UNKNOWN_WORD;
	fe->error_word = token;
	return false;
}

bool frontend_compile_source(Frontend* fe,const char* source){
	if(!frontend_prepare_func(fe)) return false;
	fe->error = FRONTEND_OK;
	fe->error_word = NULL;

	const char* p = source;
	while(*p){
		while(isspace((unsigned char)*p)) p++;
		if(!*p) break;

		const char* start = p;
		if(*p == '(' || *p == ')'){
			p++;
		}else{
			while(*p && !isspace((unsigned char)*p) && *p != '(' && *p != ')') p++;
		}

		if(!compile_token(fe,start,(size_t)(p - start))) return false;
	}
	return true;
}
