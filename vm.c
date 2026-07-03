#include "vm.h"

/**
 * currently written somewhat lazily
 * a few important safety checks are missing and they should probably be done by static analysis ahead of time
 * but the VM is still pretending to be dynamic with handles so... idk yet we will see
*/

static const Func** vm_func(const VM* vm){
	if(vm->func_stack.len==0) return NULL;
	return &TOP(vm->func_stack).func;
}

static const Block** vm_block(const VM* vm){
	if(vm->func_stack.len==0) return NULL;
	return &TOP(vm->func_stack).block;
}

static const OP** vm_op(const VM* vm){
	if(vm->func_stack.len==0) return NULL;
	return &TOP(vm->func_stack).op;
}


#define VM_FUNC(vm) (&*vm_func(vm))
#define VM_BLOCK(vm) (&*vm_block(vm))
#define VM_OP(vm) (&*vm_op(vm))

static VM_RESULT start_func(VM* vm,size_t fid){
	if(vm->func_stack.len>=vm->func_stack.cap){
		return VM_OOM_FUNCS;
	}

	FuncFrame* frame = &vm->func_stack.data[vm->func_stack.len++];
	*frame = (FuncFrame){};

	frame->func = &vm->functions.data[fid];
	frame->block = frame->func->blocks.data;
	frame->op = frame->block->ops.data;

	UnwindFrame* unwind = &vm->unwind_stack.data[vm->unwind_stack.len++];
	*unwind = (UnwindFrame){};

	unwind->param_bottom =  vm->param_stack.len-frame->func->sig.ins.len;
	unwind->epi = FUNC_END;

	return VM_OK;
}

//does not do necisary unwinding.
static VM_RESULT end_func(VM* vm){
	vm->param_stack.len = TOP(vm->unwind_stack).param_bottom;
	vm->unwind_stack.len--;
	vm->func_stack.len--;

	return VM_OK;
}


static VM_RESULT run_ops(VM* vm){
	while(vm->func_stack.len>0 && vm->unwind_stack.len>0){
		
	run_ops:
		while(VM_OP(vm).kind!=OP_NULL){
			OP op = VM_OP(vm);
			VM_OP(vm)++;
			//todo
			switch(op.kind){
			case OP_CALL:{
				start_func(vm,op.extra);
				goto run_ops;
			}
			case OP_DROP:
				if(vm->param_stack.len<=TOP(vm->unwind_stack).param_bottom)
					return VM_PARAM_UNDERFLOW;
				vm->param_stack.len--;
				break;
				
			};
		}

		return VM_OK;
	}

	return VM_OK;
}

typedef enum UnwindRes {
	UNWIND_EMPTY=-1,
	UNWIND_LOCAL,
	UNWIND_EXITS,
}UnwindRes;

static UnwindRes local_unwind(VM* vm){
	while(!vm->unwind_stack.len){
		ssize_t epi = TOP(vm->unwind_stack).epi;
		
		if(epi==FUNC_END) {
			return UNWIND_EXITS;
		}

		vm->param_stack.len = TOP(vm->unwind_stack).param_bottom;
		vm->storage.len = TOP(vm->unwind_stack).var.data - vm->storage.len;

		if(epi==NO_EPI) {
			vm->unwind_stack.len--;
			continue;
		}		

		VM_BLOCK(vm) = VM_FUNC(vm).blocks[epi];
		VM_OP(vm) = VM_BLOCK(vm).ops.data;
		return UNWIND_LOCAL;
	}

	return UNWIND_EMPTY;
}


static VM_RESULT run_terminator(VM* vm){
	VM_RESULT res = VM_OK;

	switch(VM_BLOCK(vm).term.kind){
	case TERM_SIMPLE:
		VM_BLOCK(vm) = VM_FUNC(vm).blocks[VM_BLOCK(vm).term.data.simple.tgt];
		VM_OP(vm) = VM_BLOCK(vm).ops.data;
		break;

	case TERM_UNWIND:
		return unwind(vm);

	case TERM_RET:
		if((res = unwind(vm)))
			return res;


	
	case TERM_RET_UNWIND:{

	}
	}

	return res;
}

static void crash_gracefully(VM* vm){
	if(vm->func_stack.len==0 || vm->unwind_stack.len==0)
		return;

	vm->param_stack.len = TOP(vm->unwind_stack).param_bottom;
	ssize_t epi = TOP(vm->unwind_stack).epi;
	if(epi>0){
		const Func* func = vm->funcs.data[epi];

	}
}

VM_RESULT vm_run(VM* vm){
	while(vm->func_stack.len>0 && vm->unwind_stack.len>0){
		VM_RESULT res = run_ops(vm);
		if(!res){
			crash_gracefully(vm);
			return res;
		}

		res = run_terminator(vm);
		if(!res){
			crash_gracefully(vm);
			return res;
		}
	}
	
	return VM_OK;
}