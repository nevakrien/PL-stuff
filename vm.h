#ifndef VM_H
#define VM_H

#include "ir.h"

typedef struct Handle {
	Cell* data;
	const Var* var;
} Handle;

static const ssize_t NO_EPI = -1;
static const ssize_t FUNC_END = -2;

typedef struct UnwindFrame {
	Handle var;

	size_t param_bottom;//what to set the param stack to in the end
	ssize_t epi; //-1 means nothing
} UnwindFrame;

typedef struct FuncFrame {
	const Func* func;
	const Block* block;
	const OP* op;
}FuncFrame;

typedef enum VM_RESULT{
	VM_OK,
	VM_CRASHED,
	VM_OOM_FUNCS,
	VM_OOM_UNWIND,
	
	VM_PARAM_UNDERFLOW,
}VM_RESULT;



typedef struct VM {
	STACK(UnwindFrame) unwind_stack;
	STACK(FuncFrame) func_stack;

	STACK(Cell) storage;
	STACK(Handle) param_stack;

	STACK(Func) functions;//can maybe realloc?

} VM;




VM_RESULT vm_run(VM* vm);
VM_RESULT vm_call_func(VM* vm,const Func* func);

#endif // VM_H
