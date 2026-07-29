#ifndef FRONTEND_H
#define FRONTEND_H

#include "vm.h"

typedef enum FrontendError {
	FRONTEND_OK,
	FRONTEND_OOM,
	FRONTEND_UNKNOWN_WORD,
	FRONTEND_BAD_TOKEN,
	FRONTEND_EXPECTED_NAME,
	FRONTEND_BAD_NUMBER,
	FRONTEND_UNKNOWN_TYPE,
	FRONTEND_DUPLICATE_VAR,
	FRONTEND_MACRO_COMPILE_FAILED,
	FRONTEND_MACRO_RUNTIME_FAILED,
} FrontendError;

typedef enum FrontendWordKind {
	FRONTEND_WORD_OP,
	FRONTEND_WORD_FUNC,
	FRONTEND_WORD_NATIVE,
	FRONTEND_WORD_IMMEDIATE,
} FrontendWordKind;

typedef enum FrontendImmediateKind {
	FRONTEND_IMMEDIATE_FUNC,
	FRONTEND_IMMEDIATE_VAR,
} FrontendImmediateKind;

typedef struct FrontendImmediate {
	FrontendImmediateKind kind;
	func_idx func;
} FrontendImmediate;

typedef VmParsedName FrontendName;

typedef struct FrontendParser {
	const char* source;
	const char* cursor;
} FrontendParser;

typedef struct FrontendWord {
	const char* name;
	FrontendWordKind kind;
	union {
		OP op;
		func_idx func;
		uoffset_t global;
		FrontendImmediate immediate;
	} data;
} FrontendWord;

typedef struct Frontend {
	CompileContext* ctx;
	Func* func;
	STACK(FrontendWord) words;
	VM macro_vm;
	FrontendParser parser;
	STACK(char*) owned_names;
	STACK(uoffset_t) owned_globals;
	size_t op_cap;
	size_t block_cap;
	size_t var_cap;
	size_t type_cap;
	block_idx current_basic;
	FrontendError error;
	const char* error_word;
	VM_RESULT macro_result;
} Frontend;

void frontend_init(Frontend* fe,CompileContext* ctx,Func* func);
void frontend_free(Frontend* fe);
size_t frontend_whitespace_len(const char* text);
bool frontend_prepare_func(Frontend* fe);
bool frontend_add_core_words(Frontend* fe);
bool frontend_add_word_op(Frontend* fe,const char* name,OP op);
bool frontend_add_word_func(Frontend* fe,const char* name,func_idx idx);
bool frontend_add_word_native(Frontend* fe,const char* name,uoffset_t global);
bool frontend_add_word_immediate(Frontend* fe,const char* name,func_idx idx);
bool frontend_emit_op(Frontend* fe,OP op);
bool frontend_parse_name(Frontend* fe,FrontendName* name);
bool frontend_parse_number(Frontend* fe,num_t* number);
bool frontend_parse_type(Frontend* fe,type_idx* tid);
bool frontend_declare_var(Frontend* fe,type_idx tid,FrontendName name);
bool frontend_compile_source(Frontend* fe,const char* source);

#endif // FRONTEND_H
