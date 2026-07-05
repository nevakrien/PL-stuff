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
	B_SLICE_FROM_ARR,
	B_PUSH_SLICE_AT,
	B_SLICE_INC,
	B_SLICE_DEC,

	//numeric buildins
} ByteCode;

typedef SLICE(ByteCode) VmCode;
typedef STACK(VmCode) VmFuncS;

typedef struct CrashFrame {
	const ByteCode* pc;
	const ByteCode* base;
	size_t storage_len;
	size_t param_len;
	size_t param_base;
} CrashFrame;


typedef struct VM {
	STACK(char) storage; //aligned to CELL_ALIGN
	STACK(void*) param_stack;
	STACK(CrashFrame) crash_stack;
} VM;

typedef enum VM_RESULT {
	VM_OK,
	VM_CRASH,
	VM_HARD_CRASH,
	VM_OOM_PARAM,
	VM_OOM_STORAGE,
	VM_OOM_CRASH,
	VM_PARAM_UNDERFLOW,
	VM_STORAGE_UNDERFLOW,
	VM_CRASH_UNDERFLOW,
	VM_BAD_CRASH_FRAME,
	VM_INVALID_STORAGE_OFFSET,
	VM_INVALID_ARG,
	VM_NULL_GLOBAL,
	VM_ARRAY_BOUNDS,
	VM_DIV_BY_ZERO,
	VM_ARRAY_CAPACITY,
	VM_ARRAY_UNDERFLOW,
	VM_NULL_CALL_TARGET,
	VM_NULL_NATIVE_FUNC,
	VM_INVALID_BYTECODE,
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
