#ifndef FRONT_END_H
#define FRONT_END_H

#include "ir.h"

typedef enum ParType : count_t{
	PAR_LOCAL,
	PAR_ARG,
} ParType;

typedef struct Handle {
	count_t idx;
	ParType loc;
} Handle;

typedef STACK(Handle) VirtualStack;


#endif // FRONT_END_H

