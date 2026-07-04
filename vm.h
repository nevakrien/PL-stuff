#ifndef VM_H
#define VM_H

#include "ir.h"

// Some ops have additional data stored immediately after them.
typedef enum ByteCode : char {
	B_DONE,
	B_RET,
	B_STORAGE_ADD,
	B_PUSH_VAR,
	B_PUSH_ARG,
	B_DROP_N,
	B_PUSH_ARR_AT,//pops an index pointer and an array pointer, pushes element pointer
	B_COPY,
	B_ADD,
	B_SUB,
	B_MUL,
	B_DIV,
	B_AND,
	B_OR,
	B_XOR,
	B_BIT_NOT,
	B_ARR_PUSH,
	B_ARR_DROP,
	B_JUMP,
	B_BRANCH,
	B_PUSH_CRASH,
	B_POP_CRASH,
	B_CRASH,
	B_HARD_CRASH,
	B_PUSH_GLOBAL,
	B_CALL,
	B_CALL_NATIVE,

	//numeric buildins
} ByteCode;

typedef SLICE(ByteCode) VmCode;
typedef STACK(VmCode) VmFuncS;

typedef struct CrashFrame {
	const ByteCode* pc;
	size_t storage_len;
	size_t param_len;
} CrashFrame;


typedef struct VM {
	STACK(char) storage; //aligned to CELL_ALIGN
	STACK(void*) param_stack;
	STACK(CrashFrame) crash_stack;
} VM;

typedef enum VM_RESULT {
	VM_OK,
	VM_CRASH,
	VM_OOM_PARAM,
	VM_OOM_STORAGE,
	VM_OOM_CRASH,
} VM_RESULT;

typedef VM_RESULT (*VmNativeFunc)(VM*);

typedef struct CompileContext {
	GlobalS globals;
	Funcs funcs;
	VmFuncS code;//code is malloced
} CompileContext;

VmCode vm_compile_no_defers(const Func* func,CompileContext* ctx);//note func may not be from funcs.
VM_RESULT vm_run(VM* vm,const ByteCode* code);

static inline VM_RESULT vm_push_param(VM* vm, void* param){
	if(vm->param_stack.len>=vm->param_stack.cap) return VM_OOM_PARAM;
	vm->param_stack.data[vm->param_stack.len++] = param;
	return VM_OK;
}
#endif // VM_H
