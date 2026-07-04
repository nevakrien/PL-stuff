# PL-stuff

# IR
ir is tree based and is moddeled to allow defers to ALWAYS happen even if there is a crash in the middle.
the goal is to allow freeing memory to be done with defers. there are a few core block types.

## Basic
this block can generate function calls and all assigment style operations.
It works on a virtual stack-like machine. where any additional arguments are discarded by the end of the block.
all buildins work as follow 

outputs, inputs, FUNC

then leave just the outputs on the stack after the function wrote to them.
there is aliasing rules specifically outputs may not alias any other argument (be it input or output).
fields of structs are considered aliasing that struct.

## Defer
defer simply specifies a block to excute and a second block to run after the first.
The second part would run regardless of breaks/crashes

## Crash (hard/soft)
a hard crash simply exists the program. 
a soft crash runs all pending defers and then exits the program.
if subsequnt defers (including on every calling function) fail we try runing them upto an implementation defined MAX_CRASH and then hard crash. not some platforms may choose to set that to 0 meaning that all crashes are hard crashes. This allows removing crash pads.

## Crash Pad
runs a body and then on fail runs the crash pad and recrashes. This is intended to be mostly internal. we transform defers into normal control flow and crash pads.

## Break
break simply goes up some number of levels. it can be used to implement continue (by having loop{many{.. break 1}} which exists into the loop again. or to implement return statments by existing the entire function scope. recall that all values are returned via injection.

## Var
allocates a var on the local stack for the duration of the body.