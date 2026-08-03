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
	FRONTEND_INTERPRETER_COMPILE_FAILED,
	FRONTEND_INTERPRETER_RUNTIME_FAILED,
	FRONTEND_BAD_CONTROL_FLOW,
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
	FRONTEND_IMMEDIATE_LOOP,
	FRONTEND_IMMEDIATE_AGAIN,
	FRONTEND_IMMEDIATE_IF,
	FRONTEND_IMMEDIATE_ELSE,
	FRONTEND_IMMEDIATE_DONE,
	FRONTEND_IMMEDIATE_BREAK,
	FRONTEND_IMMEDIATE_CONTINUE,
	FRONTEND_IMMEDIATE_START,
	FRONTEND_IMMEDIATE_FINALLY,
	FRONTEND_IMMEDIATE_END,
	FRONTEND_IMMEDIATE_DEFER,
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

typedef enum FrontendScopeKind {
	FRONTEND_SCOPE_LOOP,
	FRONTEND_SCOPE_IF,
	FRONTEND_SCOPE_EPILOGUE,
} FrontendScopeKind;

typedef enum FrontendScopePhase {
	FRONTEND_SCOPE_BODY,
	FRONTEND_SCOPE_WAIT_OPEN,
	FRONTEND_SCOPE_CONDITION,
	FRONTEND_SCOPE_ELSE_BODY,
	FRONTEND_SCOPE_CLEANUP,
	FRONTEND_SCOPE_CLEANUP_WAIT_OPEN,
} FrontendScopePhase;

typedef struct FrontendScope {
	FrontendScopeKind kind;
	FrontendScopePhase phase;
	block_idx block;
	block_idx first;
	block_idx second;
	block_idx after;
	count_t parent_depth;
	count_t break_depth;
	count_t continue_depth;
	count_t delimiter_depth;
	bool implicit_end;
} FrontendScope;

typedef struct Frontend {
	CompileContext* ctx;
	Func* func;
	STACK(FrontendWord) words;
	VM macro_vm;
	VM interpreter_vm;
	FrontendParser parser;
	FrontendName current_token;
	STACK(char*) owned_names;
	STACK(uoffset_t) owned_globals;
	STACK(FrontendScope) scopes;
	size_t op_cap;
	size_t block_cap;
	size_t var_cap;
	size_t type_cap;
	block_idx current_basic;
	count_t scope_depth;
	FrontendError error;
	const char* error_word;
	VM_RESULT macro_result;
	VM_RESULT interpreter_result;
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
