#ifndef FRONT_END_H
#define FRONT_END_H

#include "ir.h"
#include "io.h"

typedef enum ParKind : count_t{
	PAR_LOCAL,
	PAR_ARG,
	PAR_GLOBAL,

	PAR_STRUCT_FILED,
	PAR_ARR_MEMBER,
} ParKind;

typedef count_t par_idx;

typedef struct Par {
	ParKind kind;
	count_t idx;
	count_t parent;//only important for projections
} Par;

typedef struct CodeLoc {
	block_idx block;
	op_idx op;
}CodeLoc;


typedef count_t hold_idx;
static const hold_idx NO_PARRENT = -1;

typedef struct {
	par_idx par;
	CodeLoc site;//can be a Var or the function base
	hold_idx parent;
	bool just_const;
} HoldChain;

typedef struct Handle {
	Par par;
	life_t life;
	hold_idx holds_start;//pars other than this that are "owned" by this (meaning borrowing this borrows them)
	//this is only for DIRECT ownership, derived ownership is gathered recursivly
	//struct fields are considered heled by the structs
	//and all possible arrays assigned to a Slice or View are also owned
	count_t num_holds;
}Handle;

typedef STACK(Handle) HandleS;
typedef STACK(HoldChain) HoldS;

typedef struct ParIdxTable {
	Par* keys;
	par_idx* vals;
	unsigned char* used;
	size_t len;
	size_t cap;
} ParIdxTable;

static const par_idx PAR_IDX_INVALID = (par_idx)-1;

void par_idx_table_free(ParIdxTable* table);
void par_idx_table_reset(ParIdxTable* table);
bool par_idx_table_get(const ParIdxTable* table, Par key, par_idx* out);
void par_idx_table_put(ParIdxTable* table, Par key, par_idx val);

// static inline int unborrow(HandleS handles,par_idx me,bool mut);
// static inline int borrow(HandleS handles,par_idx me,bool mut){
// 	for()
// }

typedef STACK(par_idx) VirtualStack;

typedef enum SCOPE_KIND {
	SCOPE_MANY,
	
	SCOPE_DEFER,
	SCOPE_FINALLY,//like defer but ordered body,defer not defer,body
	SCOPE_BRANCH,

	SCOPE_LOOP,
	SCOPE_VAR,

	SCOPE_FUNC,

}SCOPE_KIND;

// typedef struct Scope {
// 	SCOPE_KIND kind;
// 	union {
// 		STACK(Block) many;
// 		struct {
// 			Block block;
// 			bool stored;
// 		} pend;
// 		var_idx var;
// 	}data;
// }Scope;

typedef struct CompileContext {
	GlobalS globals;
	Funcs funcs;
	VmFuncS code;//code is malloced

	HandleS handles;
	HoldS holds;
	ParIdxTable par_idxs;
	VirtualStack pars;

	// STACK(OP) ops;
	// STACK(Block) blocks;//position 0 is inserted at function start
	// STACK(Scope) scopes;
} CompileContext;

// typedef enum COMP_ERROR {
// 	COMP_OK,
// 	COMP_NOT_BASIC,
// 	COMP_NO_FUNC,
// 	COMP_NO_SCOPE,
// };

// static inline COMP_ERROR emit_op(CompileContext* ctx,OP op) {
// 	if(!ctx->blocks.len) return COMP_NO_FUNC;
// 	if(TOP(ctx->blocks).kind!=BLOCK_BASIC) return COMP_NOT_BASIC;

// 	PUSH_HEAP(ctx->ops,op);
// 	return COMP_OK;
// }

// static inline COMP_ERROR emit_block(CompileContext* ctx,Block block) {
// 	if(!ctx->blocks.len) return COMP_NO_FUNC;
// 	if(!ctx->scopes.len) return COMP_NO_SCOPE;

// 	switch(TOP(ctx->scopes).kind){
// 	case SCOPE_FUNC:
// 		ctx->scope.len--;
// 		ctx->blocks[0]=block;
// 		return COMP_OK;

// 	case SCOPE_DEFER:
// 	case SCOPE_FINALLY:
// 	case SCOPE_BRANCH:{
// 		if(!TOP(ctx->scopes).pend.stored){
// 			TOP(ctx->scopes).pend.stored = true;
// 			TOP(ctx->scopes).pend.block = block;
// 			return COMP_OK;
// 		}

// 		block_idx first = ctx->blocks.len;
// 		block_idx second = ctx->blocks.len+1;
// 		PUSH_HEAP(ctx->block,TOP(ctx->scopes).pend.block);
// 		PUSH_HEAP(ctx->block,block);

// 		Block parent = {0};
// 		parent.kind = BLOCK_DEFER;

// 		if(TOP(ctx->scopes).kind == SCOPE_BRANCH){
// 			parent.kind = BLOCK_BRANCH;
// 			parent.data.branch.yes=first;
// 			parent.data.branch.no=second;
// 		}else if (TOP(ctx->scopes).kind == SCOPE_BRANCH) {
// 			parent.data.defer.defer=first;
// 			parent.data.defer.body=second;
// 		} else {
// 			parent.data.defer.body=first;
// 			parent.data.defer.defer=second;
// 		}

// 		ctx->scopes.len--;
// 		return emit_block(ctx,parent);
// 	}
// 	}
// }


// static inline COMP_ERROR close_scope(CompileContext* ctx){
// 	if(!ctx->blocks.len) return COMP_NO_FUNC;
// 	switch(TOP(ctx->blocks)) {
// 	case BLOCK_MANY:

// 	}
// }


void comp_context_reset(CompileContext* ctx);
par_idx comp_context_intern_par(CompileContext* ctx, Par par);

typedef int (*ProcessCall)(
	void* user,
	CompileContext* ctx,
	func_idx fid,
	CodeLoc loc
);

int comp_run_calls(CompileContext* ctx,ProcessCall f,void* user);



typedef enum TypeCheckErrorKind : char {
	TYPE_CHECK_ERROR_PORTAL_NON_LOCAL,
	TYPE_CHECK_ERROR_PORTAL_BACKING_SCOPE,
	TYPE_CHECK_ERROR_STACK_UNDERFLOW,
	TYPE_CHECK_ERROR_INVALID_LOCAL,
	TYPE_CHECK_ERROR_INVALID_GLOBAL,
	TYPE_CHECK_ERROR_INVALID_FUNC,
	TYPE_CHECK_ERROR_INVALID_TYPE,
	TYPE_CHECK_ERROR_INVALID_FIELD,
	TYPE_CHECK_ERROR_INVALID_INDEX,
	TYPE_CHECK_ERROR_INVALID_CALL_TARGET,
	TYPE_CHECK_ERROR_TYPE_MISMATCH,
	TYPE_CHECK_ERROR_INVALID_CALL,
} TypeCheckErrorKind;

typedef struct TypeCheckError {
	TypeCheckErrorKind kind;
	CodeLoc loc;
	OP op;
	union {
		struct {
			par_idx portal;
			par_idx backing;
		} portal;
		struct {
			par_idx par;
			type_idx expected;
			type_idx actual;
		} type;
		struct {
			count_t needed;
			count_t actual;
		} stack;
		struct {
			count_t idx;
			count_t len;
		} index;
	} data;
} TypeCheckError;

typedef int (*TypeCheckErrorReporter)(
	void* user,
	CompileContext* ctx,
	const TypeCheckError error
);

int type_check_first_pass(TypeCheckErrorReporter reporter,CompileContext* ctx);


typedef struct AliasError {
	par_idx first_mut;
	par_idx second;

	CodeLoc call;
} AliasError;

typedef int (*AliasErrorReporter)(
	void* user,
	CompileContext* ctx,
	const AliasError error
);


int borrow_check(AliasErrorReporter reporter,CompileContext* ctx);

#endif // FRONT_END_H
