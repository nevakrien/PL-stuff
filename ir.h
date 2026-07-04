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
#endif

#ifndef LIFE_TYPE
#define LIFE_TYPE ssize_t
#endif


typedef NUMERIC_TYPE num_t;
typedef OFFSET_TYPE offset_t;
typedef OFFSET_UNSIGNED_TYPE uoffset_t;
typedef COUNT_TYPE count_t;
typedef LIFE_TYPE life_t;

typedef count_t var_idx;
typedef count_t block_idx;
typedef count_t type_idx;

static const type_idx TYPE_INT_ID = 0;
static const type_idx TYPE_BYTE_ID = 1;
static const type_idx TYPE_INVALID_ID = (type_idx)-1;

typedef union Cell {
	life_t life;
	void* ptr;
	const void* cptr;
	size_t size;
	ssize_t ssize;
	num_t num;
} Cell;

static const size_t CELL_ALIGN = alignof(Cell);

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
    TYPE_STRUCT,
} TYPE_KIND;

typedef struct TypeField {
    const char* name;
    type_idx tid;
    size_t offset;
} TypeField;

typedef SLICE(TypeField) TypeFieldS;

typedef struct Type {
    TYPE_KIND kind;
    const char* name;
    size_t payload_size;
    size_t size;
    size_t align;
    union {
        // Stack arrays are fixed-capacity values laid out as [len][data...].
        struct {type_idx elem; count_t capacity; size_t data_offset;} array;
        TypeFieldS fields;
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
    OP_NULL=0,//null-terminator

    OP_CALL,//extra=id

    OP_ASSIGN,
    OP_ADD_ASSIGN,
    OP_SUB_ASSIGN,
    OP_MUL_ASSIGN,
    OP_DIV_ASSIGN,

    OP_AND_ASSIGN,
    OP_OR_ASSIGN,
    OP_XOR_ASSIGN,
    OP_BIT_NOT_ASSIGN,

    OP_DROP,//extra=how many

    OP_PUSH_VAR,//extra=idx    
    OP_PUSH_ARG,//extra=idx 
    OP_PUSH_GLOBAL,//extra=idx

    OP_ARR_PUSH,
    OP_ARR_AT,
    OP_ARR_DROP,
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



typedef struct Var {
    type_idx tid;//TYPE_INVALID_ID means no var
	const char* name;
} Var;

typedef SLICE(Var) VarS;
typedef SLICE(OP) OPS;

typedef struct Sig {
    SLICE(Var) ins;
    SLICE(Var) outs;

    bool can_crash;
} Sig;

//this is a tree stored as a flat DAG 
//it can be loaded directly from disk
typedef struct Block {    
	BLOCK_KIND kind;
    union {
        struct {block_idx start; count_t len;} basic;
        struct {block_idx start; count_t len;} many;
        struct {block_idx next; block_idx defer;} defer;//same as crash_pad
        struct {block_idx body; block_idx pad;} crash_pad;
        struct {var_idx cond; block_idx yes; block_idx no;} branch;
        struct {var_idx cond; block_idx body;} loop;
        count_t level;
        struct {var_idx var; block_idx body;} var;
    } data;
} Block;

typedef SLICE(Block) BlockS;
typedef STACK(Block) BlocksBuilder;


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


static inline Type type_int(void){
    return (Type){.kind=TYPE_INT,.name="int",.payload_size=sizeof(num_t),.align=alignof(num_t)};
}

static inline Type type_byte(void){
    return (Type){.kind=TYPE_BYTE,.name="byte",.payload_size=1,.align=1};
}

static inline Type type_array(type_idx elem,count_t capacity){
    return (Type){.kind=TYPE_ARRAY,.size=0,.align=0,.data.array={.elem=elem,.capacity=capacity}};
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


bool type_layout_all(TypeS types);



//move all defer statments to be inlined in the correct places and in crash pads.
void remove_defers(BlockS* blocks);

#endif // IR_H
