#include "ir.h"

typedef STACK(count_t) EpiStack;

static void _remove_defers(count_t src,count_t dst,const BlockS* blocks,BlocksBuilder* res,EpiStack* epis){

	Block b = blocks->data[src];
	res->data[dst] = b;

	PUSH_HEAP(*epis,0);//poped later

	switch(b.kind){
	case BLOCK_BASIC: break;		
    case BLOCK_DEFER: {
    	count_t dst_next = res->len;
    	count_t dst_defer = res->len+1;
    	EXTEND_HEAP(*res,2);


    	res->data[dst].data.defer.next = dst_next;
    	res->data[dst].data.defer.defer = dst_defer;
    	res->data[dst].kind = BLOCK_CRASH_PAD;

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

    case BLOCK_VAR: break; 
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
