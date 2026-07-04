#ifndef FRONT_END_H
#define FRONT_END_H

#include "ir.h"

typedef enum ParType : count_t{
	PAR_LOCAL,
	PAR_ARG,
	PAR_GLOBAL,
} ParType;

typedef struct Par {
	count_t idx;
	ParType loc;
} Par;

typedef STACK(Par) VirtualStack;

#endif // FRONT_END_H

