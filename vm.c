#include "vm.h"

static int ret_block(VM* vm){
	if(!vm->block_stack.len) return 1;
	BlockFrame b = TOP(vm->block_stack);
	FuncFrame f = TOP(vm->func_stack);
	
	const Func* func = &vm->functions.data[f.fid];
	const Block* block = &func->blocks.data[b.bid];

	if(b.ip >= block->body_start && b.ip<block->body_end){
		//go run epilogue
		TOP(vm->block_stack).ip = block->body_end;
		//probably want a goto interpter here... or a continue
		//so this likely wants to be a case in the main body
	} 

	vm->block_stack.len--;
	if(vm->block_stack.len <= f.block_stack_len){
		//return out of function
	}

	return 0;
}

static int pop_func(VM* vm){
	if(vm->func_stack.len==0) return 1;
	vm->block_stack.len = vm->func_stack.data[--vm->func_stack.len].block_stack_len;
	return 0;
}


int run_vm(VM* vm){	
	for(;;){
		if(!vm->block_stack.len) return 0;
		if(!vm->func_stack.len) return 1;

		BlockFrame b = TOP(vm->block_stack);
		FuncFrame f = TOP(vm->func_stack);

		const Func* func = &vm->functions.data[f.fid];
		const Block* block = &func->blocks.data[b.bid];

		if(b.ip>=block->ops.len){
	block_pop:
			//need to add aditional logic for actual param stack
			vm->block_stack.len--;
			if(vm->block_stack.len <= f.block_stack_len){
				vm->func_stack.len--;
			}
			continue;
		}


		switch (block->ops.data[b.ip].kind) {
			
		}

		goto next;

	ret_block:
		if(b.ip >= block->body_start && b.ip<block->body_end){
			//go run epilogue
			TOP(vm->block_stack).ip = block->body_end;
			continue;
			
		}

		goto block_pop;

	next:
		b.ip++;
		TOP(vm->block_stack)=b;

		if(b.ip >= block->ops.len){	
			goto ret_block;
		}
	}
}