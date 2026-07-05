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
bool par_idx_table_get(const ParIdxTable* table, Par key, par_idx* out);
void par_idx_table_put(ParIdxTable* table, Par key, par_idx val);

// static inline int unborrow(HandleS handles,par_idx me,bool mut);
// static inline int borrow(HandleS handles,par_idx me,bool mut){
// 	for()
// }

typedef STACK(par_idx) VirtualStack;





typedef struct CompileContext {
	GlobalS globals;
	Funcs funcs;
	VmFuncS code;//code is malloced

	HandleS handles;
	HoldS holds;
	ParIdxTable par_idxs;
	VirtualStack pars;
} CompileContext;

par_idx comp_context_intern_par(CompileContext* ctx, Par par);

typedef int (*ProcessCall)(
	void* user,
	CompileContext* ctx,
	func_idx fid,
	CodeLoc loc
);

int comp_run_calls(CompileContext* ctx,ProcessCall f,void* user);



typedef enum PortalErrorKind : char {
	PORTAL_ERROR_NON_LOCAL,
	PORTAL_ERROR_BACKING_SCOPE,
} PortalErrorKind;

typedef struct PortalError {
	PortalErrorKind kind;
	par_idx portal;
	par_idx backing;
	CodeLoc loc;
} PortalError;

typedef int (*PortalErrorReporter)(
	void* user,
	CompileContext* ctx,
	const PortalError error
);

int gather_portal_regions(PortalErrorReporter reporter,CompileContext* ctx);


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
