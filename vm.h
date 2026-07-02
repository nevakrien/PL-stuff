#ifndef VM_H
#define VM_H

#include "ir.h"

typedef struct BlockFrame {
	count_t ip;
	count_t bid;
} BlockFrame;

typedef struct FuncFrame {
	count_t fid;
	size_t block_stack_len;//used to reset to the right block
}FuncFrame;

typedef struct Handle {
	Cell* data;
	const Var* var;
} Handle;

typedef struct VM {
	STACK(BlockFrame) block_stack;
	STACK(FuncFrame) func_stack;
	count_t unwind;//0 means running normally, -1 means return from function

	STACK(Cell) storage;
	STACK(Handle) param;

	STACK(Func) functions;//can maybe realloc?
} VM;


int run_vm(VM* vm);

#endif // VM_H
