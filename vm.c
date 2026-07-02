#include "vm.h"

#define UNWIND_FUNC  ((count_t)-2)
#define UNWIND_CRASH ((count_t)-1)

static int push_block(VM* vm, count_t bid){
	if(vm->block_stack.len >= vm->block_stack.cap) return 1;
	vm->block_stack.data[vm->block_stack.len++] = (BlockFrame){
		.ip = 0,
		.bid = bid,
	};
	return 0;
}

static int push_func(VM* vm, count_t fid){
	if(vm->func_stack.len >= vm->func_stack.cap) return 1;
	vm->func_stack.data[vm->func_stack.len++] = (FuncFrame){
		.fid = fid,
		.block_stack_len = vm->block_stack.len,
	};
	if(push_block(vm, 0)){
		vm->func_stack.len--;
		return 1;
	}
	return 0;
}

static int pop_branch(VM* vm){
	// if(!vm->param.len) return 0;
	Handle h = TOP(vm->param);
	vm->param.len--;
	return h.data && h.data->num;
}

static int start_unwind(VM* vm, count_t target, count_t unest_level){
	if(target == (count_t)GOTO_CRASH){
		vm->unwind = UNWIND_CRASH;
		return 0;
	}

	if(target == (count_t)GOTO_RET){
		if(vm->unwind != UNWIND_CRASH){
			vm->unwind = UNWIND_FUNC;
		}
		return 0;
	}

	if(unest_level){
		vm->unwind = unest_level;
		return 0;
	}

	return 0;
}

static int finish_unwind_step(VM* vm, count_t target){
	if(target == (count_t)GOTO_CRASH || target == (count_t)GOTO_RET){
		return 0;
	}

	if(vm->unwind == 0){
		return push_block(vm, target);
	}

	return 0;
}

int run_vm(VM* vm){	
	count_t unwind_target = 0;

	for(;;){
		if(!vm->block_stack.len){
			return vm->unwind == UNWIND_CRASH ? 1 : 0;
		}
		// if(!vm->func_stack.len) return 1;

		BlockFrame b = TOP(vm->block_stack);
		FuncFrame f = TOP(vm->func_stack);

		const Func* func = &vm->functions.data[f.fid];
		const Block* block = &func->blocks.data[b.bid];

		if(vm->unwind){
			if(b.ip < block->body_start){
				goto block_pop;
			}

			if(b.ip >= block->body_start && b.ip < block->body_end){
				//Run the block epilogue before actually leaving the block.
				b.ip = block->body_end;
				TOP(vm->block_stack) = b;
				continue;
			}

		} 

		if(b.ip >= block->ops.len){
			count_t target = block->term.extra1;
			if(block->term.kind == TERM_BRANCH && !pop_branch(vm)){
				target = block->term.extra2;
			}

			if(start_unwind(vm, target, block->term.unest_level)){
				return 1;
			}

			unwind_target = target;
			goto block_pop;
		}

		OP op = block->ops.data[b.ip];
		switch (op.kind) {
		case OP_CALL:
			b.ip++;
			TOP(vm->block_stack) = b;
			if(push_func(vm, op.extra)){
				return 1;
			}
			continue;

		case OP_NEST_BLOCK:
			b.ip++;
			TOP(vm->block_stack) = b;
			if(push_block(vm, op.extra)){
				return 1;
			}
			continue;

		default:
			goto next;
		}

	next:
		b.ip++;
		TOP(vm->block_stack) = b;
		continue;

	block_pop:
			vm->storage.len-=block->var.size;

			vm->block_stack.len--;
			if(vm->unwind && vm->unwind != UNWIND_FUNC && vm->unwind != UNWIND_CRASH){
				vm->unwind--;
			}

			if(vm->block_stack.len <= f.block_stack_len){
				vm->func_stack.len--;
				if(vm->unwind != UNWIND_CRASH){
					vm->unwind = 0;
				}
				if(unwind_target == (count_t)GOTO_RET){
					continue;
				}
			} else if(vm->unwind == 0 && finish_unwind_step(vm, unwind_target)){
				return 1;
			}
			continue;
	}
}
