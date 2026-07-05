#ifndef VM_H
#define VM_H

#include "front_end.h"

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

VmCode vm_compile_no_defers(const Func* func,CompileContext* ctx);//note func may not be from funcs.
VM_RESULT vm_run(VM* vm,const ByteCode* code);
void vm_free(VM* vm);

static inline VM_RESULT vm_push_param(VM* vm, void* param){
	if(vm->param_stack.len>=vm->param_stack.cap) return VM_OOM_PARAM;
	vm->param_stack.data[vm->param_stack.len++] = param;
	return VM_OK;
}
#endif // VM_H
