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
typedef OFFSET_TYPE uoffset_t;
typedef COUNT_TYPE count_t;
typedef LIFE_TYPE life_t;

typedef union Cell {
	life_t life;
	void* ptr;
	const void* cptr;
	size_t size;
	ssize_t ssize;
	num_t num;
} Cell;

#define STACK(T) struct { \
    T *data;            \
    size_t len;         \
    size_t cap;         \
}

#define SLICE(T) struct { \
    T *data;            \
    size_t len;         \
}

#define TOP(x) (x.data[x.len-1])

typedef enum OP_KIND : uoffset_t {
    OP_CALL,
    OP_RET,

    OP_CRASH,//unwinds the entire stack

    OP_NEST_BLOCK,
    OP_TAIL_BLOCK,
    OP_NEST_BRANCH_BLOCK,
    OP_TAIL_BRANCH_BLOCK,
    OP_RET_BLOCKS,


    OP_ASSIGN,
    OP_ADD_ASSIGN,
    OP_SUB_ASSIGN,
    OP_MUL_ASSIGN,
    OP_DIV_ASSIGN,

    OP_AND_ASSIGN,
    OP_OR_ASSIGN,
    OP_XOR_ASSIGN,
    OP_BIT_NOT_ASSIGN,

    OP_DROP,

    OP_PUSH_VAR,    
    OP_PUSH_ARG,
    OP_PUSH_GLOBAL,

    OP_ARR_PUSH,
    OP_ARR_AT,
    OP_ARR_DROP,
} OP_KIND;

typedef struct OP {
	OP_KIND kind;
	uoffset_t extra;
} OP;

typedef struct Var {
    size_t tid;//-1 means no var
	const char* name;
} Var;

typedef SLICE(OP) OPS;

typedef struct Block {    
	OPS ops;
	count_t body_start;
	count_t body_end;

    Var var;//may be null
} Block;

typedef struct Func {
    //starts always at block 0
    //existing the first block is the same as returning (altogh re entring then exiting is not)
    SLICE(Block) blocks;
    
    SLICE(Var) args;
    SLICE(Var) outs;

    const char* name;
} Func;

#endif // IR_H

