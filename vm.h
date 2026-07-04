#ifndef VM_H
#define VM_H

#include "ir.h"

typedef enum ByteCode : char {
	B_DONE,
	B_RET,
	B_PUSH_VAR,
	B_PICK,
	B_DROP_N,
	B_JUMP,
	B_BRANCH,
	B_CALL,

	//numeric buildins
} ByteCode;

typedef SLICE(ByteCode) VmCode;


typedef struct VM {
	STACK(Cell) storage;
	STACK(Cell*) param_stack;
} VM;

typedef enum VM_RESULT {
	VM_OK,
	VM_CRASH,
	VM_OOM_PARAM,
	VM_OOM_STORAGE,
} VM_RESULT;


VmCode vm_compile_no_defers(const Func* func);
VM_RESULT vm_run(VM* vm,const ByteCode* code);

#endif // VM_H
