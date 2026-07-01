#include "vm.h"

#define UNWIND_FUNC  ((count_t)-2)
#define UNWIND_CRASH ((count_t)-1)


int run_vm(VM* vm){	
	for(;;){
		if(!vm->block_stack.len) return 0;
		if(!vm->func_stack.len) return 1;

		BlockFrame b = TOP(vm->block_stack);
		FuncFrame f = TOP(vm->func_stack);

		const Func* func = &vm->functions.data[f.fid];
		const Block* block = &func->blocks.data[b.bid];

		if(vm->unwind){
			if(b.op<block->body_start){
				goto block_pop;
			}

			if(b.ip < block->body_end){
				//Run the block epilogue before actually leaving the block.
				b.ip = block->body_end;
				TOP(vm->block_stack) = b;
				continue;
			}

		} 

		if(b.ip >= block->ops.len){
			goto block_pop;
		}

		OP op = block->ops.data[b.ip];
		switch (op) {
		case OP_CRASH:
			vm->unwind = UNWIND_CRASH;
			goto next;

		case OP_RET:
			if(vm->unwind!=UNWIND_CRASH){
				vm->unwind = UNWIND_FUNC;
			}
			goto next;

		case OP_RET_BLOCKS:
			if(vm->unwind!=UNWIND_CRASH && vm->unwind!=UNWIND_FUNC){
				vm->unwind = op.extra;
			}
			goto next;
		}

	next:
		b.ip++;
		TOP(vm->block_stack) = b;
		continue;

	block_pop:
			if(block->var.tid != (size_t)(-1)){
				//TODO look up the var size and pop it out
			}

			vm->block_stack.len--;
			if(vm->unwind && vm->unwind != UNWIND_FUNC && vm->unwind != UNWIND_CRASH){
				vm->unwind--;
			}

			if(vm->block_stack.len <= f.block_stack_len){
				vm->func_stack.len--;
				if(vm->unwind != UNWIND_CRASH){
					vm->unwind = 0;
				}
			}
			continue;
	}
}
