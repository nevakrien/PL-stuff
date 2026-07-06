#ifndef FRONTEND_H
#define FRONTEND_H

#include "vm.h"

typedef enum FrontendError {
	FRONTEND_OK,
	FRONTEND_OOM,
	FRONTEND_UNKNOWN_WORD,
	FRONTEND_BAD_TOKEN,
	FRONTEND_MACRO_COMPILE_FAILED,
	FRONTEND_MACRO_RUNTIME_FAILED,
} FrontendError;

typedef enum FrontendWordKind {
	FRONTEND_WORD_OP,
	FRONTEND_WORD_FUNC,
	FRONTEND_WORD_IMMEDIATE,
} FrontendWordKind;

typedef struct FrontendWord {
	const char* name;
	FrontendWordKind kind;
	union {
		OP op;
		func_idx func;
		const Func* immediate;
	} data;
} FrontendWord;

typedef struct Frontend {
	CompileContext* ctx;
	Func* func;
	STACK(FrontendWord) words;
	VM macro_vm;
	size_t op_cap;
	FrontendError error;
	const char* error_word;
	VM_RESULT macro_result;
} Frontend;

void frontend_init(Frontend* fe,CompileContext* ctx,Func* func);
void frontend_free(Frontend* fe);
bool frontend_prepare_func(Frontend* fe);
bool frontend_add_core_words(Frontend* fe);
bool frontend_add_word_op(Frontend* fe,const char* name,OP op);
bool frontend_add_word_func(Frontend* fe,const char* name,func_idx idx);
bool frontend_add_word_immediate(Frontend* fe,const char* name,const Func* macro);
bool frontend_emit_op(Frontend* fe,OP op);
bool frontend_compile_source(Frontend* fe,const char* source);

#endif // FRONTEND_H
