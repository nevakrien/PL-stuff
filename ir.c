#include "ir.h"

typedef STACK(count_t) EpiStack;

typedef enum TypeLayoutState : char {
	TYPE_LAYOUT_PENDING,
	TYPE_LAYOUT_ACTIVE,
	TYPE_LAYOUT_DONE,
} TypeLayoutState;

static size_t align_up(size_t n,size_t align){
	assert(align);
	return (n + align - 1) / align * align;
}


static bool type_layout_one(TypeS types,type_idx tid,TypeLayoutState* states){
	if(!type_idx_valid(types,tid)) return false;
	if(states[tid] == TYPE_LAYOUT_DONE) return true;
	if(states[tid] == TYPE_LAYOUT_ACTIVE) return false;

	states[tid] = TYPE_LAYOUT_ACTIVE;
	Type* type = &types.data[tid];

	switch(type->kind){
	case TYPE_INT:
		type->payload_size = sizeof(num_t);
		type->align = alignof(num_t);
		break;

	case TYPE_BYTE:
		type->payload_size = 1;
		type->align = 1;
		break;

	case TYPE_ARRAY: {
		type_idx elem_tid = type->data.array.elem;
		if(!type_layout_one(types,elem_tid,states)) return false;

		Type elem = types.data[elem_tid];
		size_t len_size = sizeof(count_t);
		size_t data_offset = align_up(len_size,elem.align);
		type->align = elem.align > alignof(count_t) ? elem.align : alignof(count_t);
		type->data.array.data_offset = data_offset;
		type->payload_size = align_up(data_offset + elem.payload_size * type->data.array.capacity,type->align);
		break;
	}

	case TYPE_SLICE:
	case TYPE_VIEW:
		if(!type_layout_one(types,type->data.ref.elem,states)) return false;
		type->payload_size = type_slice_payload_size();
		type->align = alignof(void*) > alignof(count_t) ? alignof(void*) : alignof(count_t);
		break;

	case TYPE_STRUCT: {
		size_t offset = 0;
		size_t max_align = 1;
		TypeFieldS fields = type->data.fields;

		for(size_t i=0;i<fields.len;i++){
			type_idx field_tid = fields.data[i].tid;
			if(!type_layout_one(types,field_tid,states)) return false;

			Type field_type = types.data[field_tid];
			offset = align_up(offset,field_type.align);
			fields.data[i].offset = offset;
			offset += field_type.payload_size;
			if(max_align < field_type.align) max_align = field_type.align;
		}

		type->align = max_align;
		type->payload_size = align_up(offset,max_align);
		break;
	}

	case TYPE_NATIVE_FUNC_POINTER:
		type->payload_size = sizeof(void*);
		type->align = alignof(void*);
		break;
	}

	type->size = align_up(type->payload_size,CELL_ALIGN);
	states[tid] = TYPE_LAYOUT_DONE;
	return true;
}

bool type_layout_all(TypeS types){
	if(types.len < 2) return false;
	if(types.data[TYPE_INT_ID].kind != TYPE_INT) return false;
	if(types.data[TYPE_BYTE_ID].kind != TYPE_BYTE) return false;

	TypeLayoutState* states = calloc(types.len,sizeof(*states));
	if(!states) return false;

	bool ok = true;
	for(type_idx tid=0;tid<types.len;tid++){
		if(!type_layout_one(types,tid,states)){
			ok = false;
			break;
		}
	}

	free(states);
	return ok;
}

static void _remove_defers(count_t src,count_t dst,const BlockS* blocks,BlocksBuilder* res,EpiStack* epis){

	Block b = blocks->data[src];
	res->data[dst] = b;

	PUSH_HEAP(*epis,0);//poped later

	switch(b.kind){
	case BLOCK_BASIC: break;		
	    case BLOCK_DEFER: {
	    	count_t dst_pad = res->len;
	    	count_t dst_defer = res->len+1;
	    	count_t dst_next = res->len+2;
	    	EXTEND_HEAP(*res,3);

	    	res->data[dst].kind = BLOCK_MANY;
	    	res->data[dst].data.many.start = dst_pad;
	    	res->data[dst].data.many.len = 2;

	    	res->data[dst_pad].kind = BLOCK_CRASH_PAD;
	    	res->data[dst_pad].data.crash_pad.body = dst_next;
	    	res->data[dst_pad].data.crash_pad.pad = dst_defer;

		// The defer body must not have itself on the epilogue stack.
		_remove_defers(b.data.defer.defer,dst_defer,blocks,res,epis);

		TOP(*epis)=dst_defer;
		_remove_defers(b.data.defer.next,dst_next,blocks,res,epis);
		break;
	}
	case BLOCK_CRASH_PAD: {
		count_t dst_next = res->len;
		count_t dst_pad = res->len+1;
		EXTEND_HEAP(*res,2);

		res->data[dst].data.crash_pad.body = dst_next;
		res->data[dst].data.crash_pad.pad = dst_pad;

		// The crash pad body must not have itself on the epilogue stack.
		_remove_defers(b.data.crash_pad.pad,dst_pad,blocks,res,epis);
		
		TOP(*epis)=dst_pad;
		_remove_defers(b.data.crash_pad.body,dst_next,blocks,res,epis);
		break;
	}

	case BLOCK_MANY:{
		count_t len = b.data.many.len;
		count_t start = b.data.many.start;
		count_t dst_start = res->len;

		res->data[dst].data.many.start = dst_start;
		res->data[dst].data.many.len = len;

		EXTEND_HEAP(*res,len);
		for(count_t i = 0; i<len;i++){
			_remove_defers(start+i,dst_start+i,blocks,res,epis);
		}
		break;
	}
		

	case BLOCK_CRASH: break;
	case BLOCK_HARD_CRASH: break;
	
    
    case BLOCK_BRANCH: {
    	count_t yes = res->len;
    	count_t no = res->len+1;
    	EXTEND_HEAP(*res,2);
    	res->data[dst].data.branch.yes = yes;
    	res->data[dst].data.branch.no = no;
	    	_remove_defers(b.data.branch.yes,yes,blocks,res,epis);
	    	_remove_defers(b.data.branch.no,no,blocks,res,epis);
    	break;
    }
    case BLOCK_LOOP: {
    	count_t body = res->len;
    	EXTEND_HEAP(*res,1);
	    	res->data[dst].data.loop.body = body;
	    	_remove_defers(b.data.loop.body,body,blocks,res,epis);
    	break;
    }

    case BLOCK_BREAK: {
    	EpiStack stack = *epis;
    	EpiStack actual = {0};

	    	for(count_t i=0;i<b.data.level;i++){
    		assert(stack.len);

    		count_t d = TOP(stack);
    		stack.len--;

    		if(d) PUSH_HEAP(actual,d);
    	}

    	if(!actual.len) break;

    	//we move to many{..defers.. break}
    	//this is legal since defers may not break out of the many blocks
    		
    	res->data[dst].kind = BLOCK_MANY;
    	count_t len = actual.len+1;
    	count_t start = res->len;
		res->data[dst].data.many.start = start;
		res->data[dst].data.many.len = len;
		EXTEND_HEAP(*res,len);

		for(count_t i = 0; i<len-1;i++){
			res->data[start+i] = res->data[actual.data[i]];
		}

		res->data[start+len-1].kind = BLOCK_BREAK;
		res->data[start+len-1].data.level = b.data.level+1;

		free(actual.data);
		break;
    }
    case BLOCK_VAR: {
    	count_t body = res->len;
    	EXTEND_HEAP(*res,1);
    	res->data[dst].data.var.body = body;
    	_remove_defers(b.data.var.body,body,blocks,res,epis);
    	break;
    }
	}

	epis->len--;
}

void remove_defers(BlockS* blocks){
	EpiStack epis = {0};
	BlocksBuilder res = {0};
	EXTEND_HEAP(res,1);
	_remove_defers(0,0,blocks,&res,&epis);

	free(epis.data);

	free(blocks->data);
	blocks->data=res.data;
	blocks->len=res.len;
}
