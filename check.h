#ifndef CHECK_H
#define CHECK_H

#include <assert.h>
#include <memory.h>
#include "ir.h"

typedef struct Param {
    Stack(const Var*) possible;
    bool write;
    bool read_first;
} Param;


typedef struct BlockSig {
    Stack(Param) args;

    size_t will_pop;

    Stack(Param) will_push;//can have temporarily data=NULL for when we dont yet know what will be pushed


    // bool dirty;
} BlockSig;

int verify_func(const Func* func);



#endif // CHECK_H

