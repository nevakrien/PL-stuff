#ifndef IR_H
#define IR_H

#include <stdbool.h>
#include <stddef.h>
#include <sys/types.h>
#include <stdint.h>

#ifndef NUMERIC_TYPE
#define NUMERIC_TYPE int64_t
#endif

#ifndef OFFSET_TYPE
#define OFFSET_TYPE int16_t
#define OFFSET_UNSIGNED_TYPE uint16_t
#endif

#ifndef COUNT_TYPE
#define COUNT_TYPE unsigned int
#define SCOUNT_TYPE int
#endif

typedef NUMERIC_TYPE num_t;
typedef OFFSET_TYPE offset_t;
typedef OFFSET_UNSIGNED_TYPE uoffset_t;
typedef COUNT_TYPE count_t;
typedef SCOUNT_TYPE scount_t;

typedef enum life_t : char {
    LIFE_FREE,
    LIFE_UNIQUE,
    LIFE_SHARED,
} life_t;

typedef count_t op_idx;
typedef count_t block_idx;
typedef count_t type_idx;

//types used as ops in the bytecode
typedef uoffset_t var_idx;
typedef uoffset_t func_idx;

static const type_idx TYPE_INT_ID = 0;
static const type_idx TYPE_BYTE_ID = 1;
static const type_idx TYPE_INVALID_ID = (type_idx)-1;

typedef union Cell {
	void* ptr;
	const void* cptr;
	size_t size;
	ssize_t ssize;
	num_t num;
} Cell;

#ifndef CELL_ALIGN
#include <stdalign.h>
#define CELL_ALIGN alignof(Cell)
#endif


#define STACK(T) struct { \
    T *data;            \
    size_t len;         \
    size_t cap;         \
}

#define SLICE(T) struct { \
    T *data;            \
    size_t len;         \
}


#define TOP(x) ((x).data[(x).len-1])

typedef enum TYPE_KIND : char {
    TYPE_INT,
    TYPE_BYTE,
    TYPE_ARRAY,
    TYPE_SLICE,
    TYPE_VIEW,
    TYPE_STRUCT,
    TYPE_NATIVE_FUNC_POINTER,// native VM callback carrying an IR Sig in data.sig
} TYPE_KIND;

typedef struct TypeField {
    const char* name;
    type_idx tid;
    size_t offset;
} TypeField;

typedef struct TypeRef {
    type_idx elem;
} TypeRef;

typedef SLICE(TypeField) TypeFieldS;

typedef struct Var {
    type_idx tid;//TYPE_INVALID_ID means no var
	const char* name;
} Var;

typedef struct SigInput {
    Var var;
    bool mut;
} SigInput;

typedef SLICE(Var) VarS;

typedef struct Sig {
    SLICE(SigInput) ins;
    SLICE(Var) outs;
} Sig;

typedef struct Type {
    TYPE_KIND kind;
    const char* name;
    bool is_portal;
    size_t payload_size;
    size_t size;
    size_t align;
    union {
        // Stack arrays are fixed-capacity values laid out as [len][data...].
        struct {type_idx elem; count_t capacity; size_t data_offset;} array;
        // Slices and views are values laid out as [ptr][len].
        TypeRef ref;
        TypeFieldS fields;
        Sig sig;
    } data;
} Type;

typedef SLICE(Type) TypeS;

#include <assert.h>
#include <stdlib.h>


#define EXTEND_HEAP(s,l) do {\
    (s).len+=(l);\
    (s).cap = 8+2*(s).len;\
    (s).data=realloc((s).data,(s).cap * sizeof(*(s).data));\
    assert((s).data);\
} while(0)

#define PUSH_HEAP(s,x) do {\
    size_t _push_heap_idx = (s).len;\
    EXTEND_HEAP(s,1);\
    (s).data[_push_heap_idx]=(x);\
} while(0)

typedef enum OP_KIND : char {
    OP_NULL=0,             // ( -- ) null-terminator

    OP_CALL,               // ( outs..., ins... -- outs... ) extra=func id
    OP_CALL_NATIVE_ON_STACK,// ( outs..., ins..., native_fn -- outs... ) native_fn type supplies Sig

    OP_ASSIGN,             // ( dst src -- dst ) *dst = *src
    OP_ADD_ASSIGN,         // ( dst src -- dst ) *dst += *src
    OP_SUB_ASSIGN,         // ( dst src -- dst ) *dst -= *src
    OP_MUL_ASSIGN,         // ( dst src -- dst ) *dst *= *src
    OP_DIV_ASSIGN,         // ( dst src -- dst ) *dst /= *src

    OP_AND_ASSIGN,         // ( dst src -- dst ) *dst &= *src
    OP_OR_ASSIGN,          // ( dst src -- dst ) *dst |= *src
    OP_XOR_ASSIGN,         // ( dst src -- dst ) *dst ^= *src
    OP_BIT_NOT_ASSIGN,     // ( dst -- dst ) *dst = ~*dst

    OP_DROP,               // ( x... -- ) extra=how many

    OP_PUSH_VAR,           // ( -- var ) extra=local var idx
    OP_PUSH_ARG,           // ( -- arg ) extra=argument idx
    OP_PUSH_GLOBAL,        // ( -- global ) extra=global idx

    OP_ARR_PUSH,           // ( arr elem -- arr ) append elem to stack array
    OP_ARR_AT,             // ( arr idx -- elem ) bounds-checked stack array indexing
    OP_ARR_DROP,           // ( arr n -- arr ) drop n elements from stack array

    OP_SLICE_FROM_AR,      // ( slice_or_view arr -- slice_or_view ) slice/view points at array data
    OP_SLICE_AT,           // ( slice_or_view idx -- elem ) bounds-checked slice/view indexing
    OP_SLICE_INC,          // ( slice n -- slice ) slice.data += n, len unchanged
    OP_SLICE_DEC,          // ( slice n -- slice ) slice.len -= n, data unchanged

    OP_STRUCT_AT,          // ( struct -- field ) extra=field idx
} OP_KIND;

typedef struct OP {
	OP_KIND kind;
	uoffset_t extra;
} OP;


typedef enum BLOCK_KIND : count_t {
    BLOCK_BASIC,
    BLOCK_DEFER,
    BLOCK_MANY,

    BLOCK_CRASH,
    BLOCK_HARD_CRASH,
    BLOCK_CRASH_PAD,
    
    BLOCK_BRANCH,
    BLOCK_LOOP,

    BLOCK_BREAK,//breaking out of all scoeps is return
    //a break may not break out of a defer logic block

    BLOCK_VAR,

} BLOCK_KIND;



typedef SLICE(OP) OPS;

typedef struct OpRange {
	op_idx start;
	count_t len;
} OpRange;

//this is a tree stored as a flat DAG 
//it can be loaded directly from disk
typedef struct Block {    
	BLOCK_KIND kind;
    union {
        OpRange basic;
        struct {block_idx start; count_t len;} many;
        struct {block_idx next; block_idx defer;} defer;//same as crash_pad
        struct {block_idx body; block_idx pad;} crash_pad;
        struct {OpRange cond; block_idx yes; block_idx no;} branch;
        struct {OpRange cond; block_idx body;} loop;
        count_t level;
        struct {var_idx var; block_idx body;} var;
    } data;
} Block;

typedef SLICE(Block) BlockS;
typedef STACK(Block) BlocksBuilder;

// Some VM ops have additional data stored immediately after them.
typedef enum ByteCode : char {
	B_DONE,
	B_RET,
	B_STORAGE_ADD,
	B_PUSH_VAR,
	B_PUSH_ARG,
	B_DROP_N,
	B_PUSH_ARR_AT,//pops an index pointer and an array pointer, pushes element pointer
	B_COPY,
	B_ADD,
	B_SUB,
	B_MUL,
	B_DIV,
	B_AND,
	B_OR,
	B_XOR,
	B_BIT_NOT,
	B_ARR_PUSH,
	B_ARR_DROP,
	B_JUMP,
	B_BRANCH,
	B_BRANCH_TOP,
	B_PUSH_CRASH,
	B_POP_CRASH,
	B_CRASH,
	B_HARD_CRASH,
	B_PUSH_GLOBAL,
	B_CALL,
	B_CALL_NATIVE,
	B_SLICE_FROM_ARR,
	B_PUSH_SLICE_AT,
	B_SLICE_INC,
	B_SLICE_DEC,
	B_PUSH_STRUCT_AT,

	//numeric buildins
} ByteCode;

typedef SLICE(ByteCode) VmCode;
typedef STACK(VmCode) VmFuncS;

void vm_code_free(VmCode* code);
void vm_func_s_free(VmFuncS* funcs);


typedef struct Func {
    const char* name;

    Sig sig;
    TypeS types;
    //starts always at block 0
    //existing the first block is the same as returning (altogh re entring then exiting is not)
    BlockS blocks;//malloced
    OPS ops;
    VarS vars;
} Func;

typedef struct Global{
    Var var;
    void* mem;//note may be not owned like for function pointers.
    void (*free_func)(void*);   // NULL means not owned / no cleanup needed
    bool is_mut;
} Global;

typedef STACK(Global) GlobalS;
typedef STACK(Func) Funcs;


static inline Type type_int(void){
    return (Type){.kind=TYPE_INT,.name="int",.payload_size=sizeof(num_t),.align=alignof(num_t)};
}

static inline Type type_byte(void){
    return (Type){.kind=TYPE_BYTE,.name="byte",.payload_size=1,.align=1};
}

static inline Type type_array(type_idx elem,count_t capacity){
    return (Type){.kind=TYPE_ARRAY,.size=0,.align=0,.data.array={.elem=elem,.capacity=capacity}};
}

static inline Type type_slice(type_idx elem){
    return (Type){.kind=TYPE_SLICE,.name="Slice",.is_portal=true,.size=0,.align=0,.data.ref={.elem=elem}};
}

static inline Type type_view(type_idx elem){
    return (Type){.kind=TYPE_VIEW,.name="View",.is_portal=true,.size=0,.align=0,.data.ref={.elem=elem}};
}

static inline Type type_struct(const char* name,TypeFieldS fields){
    return (Type){.kind=TYPE_STRUCT,.name=name,.size=0,.align=0,.data.fields=fields};
}

static inline bool type_idx_valid(TypeS types,type_idx tid){
    return tid < types.len;
}

static inline bool type_is_builtin(type_idx tid){
    return tid == TYPE_INT_ID || tid == TYPE_BYTE_ID;
}

static inline size_t type_stack_size(TypeS types,type_idx tid){
    assert(type_idx_valid(types,tid));
    return types.data[tid].size;
}

static inline size_t type_slice_len_offset(void){
    size_t align = alignof(count_t);
    return (sizeof(void*) + align - 1) / align * align;
}

static inline size_t type_slice_payload_size(void){
    return type_slice_len_offset() + sizeof(count_t);
}


bool type_layout_all(TypeS types);



//move all defer statments to be inlined in the correct places and in crash pads.
void remove_defers(BlockS* blocks);

#endif // IR_H
