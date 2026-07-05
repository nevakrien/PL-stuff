# PL-stuff

This repository is an experiment in a small tree-shaped IR and VM. The main design goal is to make cleanup reliable: `defer` blocks should run even when execution leaves through a crash or a non-local break.

## IR Model

A function is a tree of `Block`s stored in a flat array. Block `0` is the entry block. Leaving block `0` normally is a return; return values are written through output pointers rather than being returned on the argument stack.

At the call boundary, output pointers are below input pointers on the argument stack. In RPN form this is `outputs, inputs, FUNC`. On normal return, the input pointers are discarded as part of the IR return semantics, leaving only the output pointers on the caller's argument stack.

`BLOCK_DEFER` is a higher-level construct. An implementation may lower defers into ordinary control flow and crash pads before execution.

## Argument Stack

Most operations work on an argument stack containing pointers to values. `OP_PUSH_ARG` and `OP_PUSH_VAR` push pointers. Assignment and arithmetic operations consume those pointers and mutate the destination value.

Basic blocks are stack-neutral from the point of view of surrounding blocks. A block starts with some argument-stack height, runs its operations, and implicitly discards any extra values it produced. IR producers do not need to add that final discard themselves. It is illegal for a basic block to consume more values than it produced inside that block. For example, a block that enters with stack height `N` may temporarily grow to `N + k`, but it may not drop or consume below `N`.

Examples of illegal basic blocks:

- `OP_DROP 1` as the first operation, because it overpops the block's entry stack.
- `OP_ASSIGN` without first pushing both destination and source pointers.
- `OP_ADD_ASSIGN` unless the top two stack entries are both `int` pointers.
- `OP_ARR_AT` unless the stack has an array pointer followed by an `int` index pointer.

Extra values left by a basic block are not visible to following blocks; they are discarded at the end of the basic block.

## Operations

Assignment is destination-first:

```text
PUSH destination
PUSH source
ASSIGN
```

`OP_ASSIGN` requires destination and source to have the same type. It copies the source payload into the destination and leaves the destination pointer on the stack.

Integer assignment operations require integer pointers:

- `OP_ADD_ASSIGN`, `OP_SUB_ASSIGN`, `OP_MUL_ASSIGN`, `OP_DIV_ASSIGN`
- `OP_AND_ASSIGN`, `OP_OR_ASSIGN`, `OP_XOR_ASSIGN`
- `OP_BIT_NOT_ASSIGN`

Array operations work on fixed-capacity stack arrays:

- `OP_ARR_PUSH` expects an array pointer and an element pointer of the array element type. It appends the element and crashes at runtime if the array is full.
- `OP_ARR_AT` expects an array pointer and an `int` index pointer. It replaces those two entries with a pointer to the selected element and crashes at runtime if the index is out of bounds.
- `OP_ARR_DROP` expects an array pointer and an `int` count pointer. It drops that many elements from the end of the array and crashes at runtime if the count is negative or greater than the array length.

Slice and view operations work on reference values laid out as `[ptr][len]`:

- `OP_SLICE_FROM_AR` expects a slice/view destination pointer and an array pointer. It writes a reference to the array data and current array length into the destination.
- `OP_SLICE_AT` expects a slice/view pointer and an `int` index pointer. It replaces those two entries with a pointer to the selected element and crashes at runtime if the index is out of bounds.
- `OP_SLICE_INC` expects a mutable slice pointer and an `int` count pointer. It advances the slice data pointer by that many elements and crashes if the count is negative or greater than the slice length.
- `OP_SLICE_DEC` expects a mutable slice pointer and an `int` count pointer. It reduces the slice length by that many elements and crashes if the count is negative or greater than the slice length.

`OP_PUSH_GLOBAL` pushes a pointer to a global value. The global must exist, have a valid type, and contain non-null storage at runtime.

`OP_CALL` expects output pointers followed by input pointers for the target function. On normal return, input pointers are discarded and output pointers remain on the caller's argument stack.

`OP_CALL_NATIVE_ON_STACK` expects a native function pointer on top of the argument stack. The VM pops that pointer and calls it with the `VM*`; the native function is responsible for any additional stack effect.

## Blocks

### `BLOCK_BASIC`

Runs a contiguous list of operations. It may create temporary stack entries, but it must not underflow the stack relative to its entry height. Any entries above the entry height are discarded after the block finishes.

### `BLOCK_MANY`

Runs a sequence of sibling blocks in order. If one block becomes unreachable, later blocks in the sequence are skipped.

### `BLOCK_DEFER`

Represents `next` plus cleanup code. The defer block must run when `next` exits through normal control flow, `break`, or soft crash. A lowering pass can translate defers into ordinary control flow and crash pads.

A defer body must not exit its own tree. In particular, a `BLOCK_BREAK` inside the defer body is illegal if its target is outside the defer body's root block. This includes `continue`-style breaks to an enclosing loop and early-return-style breaks out of the function. The defer body may only fall through normally, crash, or use breaks that stay entirely inside the defer body.

This rule avoids ambiguous control flow such as `body { continue; } defer { break; }`: the `continue` is trying to leave through the defer machinery, while the defer body is also trying to leave through an outer scope. Defer cleanup code must not choose a new destination outside its own tree.

Breaks that cross a defer from the `next` body are different: they are rewritten so the defer runs first, then the original break continues outward.

### `BLOCK_CRASH` and `BLOCK_HARD_CRASH`

`BLOCK_CRASH` is a soft crash. It jumps to the nearest crash pad and restores the argument stack to exactly the state it had at the start of the protected region. For a function-level crash pad, that means the pad sees the same argument pointers the function started with. If there is no crash pad, the crash is uncaught.

`BLOCK_HARD_CRASH` immediately stops execution and does not run pending crash pads.

### `BLOCK_CRASH_PAD`

Runs a body with a crash handler installed. If the body finishes normally, the pad is skipped. If the body soft-crashes, the pad runs and then the crash is re-raised.

Crash pads are mostly an internal lowering target for defers.

### `BLOCK_BRANCH`

Branches on a live `int` local variable. Both reachable arms must leave the IR state identical: same stack types, same live variables, same local-storage size, and same crash-pad depth. If the states differ, the IR is illegal.

### `BLOCK_LOOP`

Loops while a live `int` local variable is nonzero. If the body can fall through to the next iteration, it must restore the exact state from the loop head. This prevents loop bodies from accumulating stack entries, leaking locals, or changing active crash pads between iterations.

### `BLOCK_BREAK`

Exits some number of enclosing scopes. `level` must be greater than zero and must not exceed the number of active scopes. Breaking out of all function-level scopes is how the IR represents return-like control flow.

`break` can express `continue` by breaking to a loop's inner sequencing scope, and it can express early return by breaking out of the outer function scope.

### `BLOCK_VAR`

Allocates a local variable for the duration of its body. The variable is live only inside that body. It is illegal to push a variable before it is live, after it is dead, or with an invalid type.

## Invalid IR

Implementations should reject IR that violates static stack, type, or control-flow rules. Runtime checks still exist for conditions that depend on values.

Statically illegal examples:

- A function with no entry block.
- A type table without builtin `int` at `TYPE_INT_ID` and `byte` at `TYPE_BYTE_ID`.
- Recursive or invalid type definitions.
- A basic block that overpops its entry stack.
- An operation whose operands have the wrong type or are missing.
- A branch or loop condition that is not a live `int` variable.
- Branch arms that rejoin with different IR states.
- A loop body that reaches the next iteration with a different IR state than the loop head.
- A `break` with level `0` or with a level greater than the active scope depth.
- Referencing a local variable outside its `BLOCK_VAR` body.
- Executing raw `BLOCK_DEFER` without first giving it semantics directly or lowering it to ordinary control flow.

Runtime crash examples:

- Division by zero.
- Array index out of bounds.
- Pushing into a full fixed-capacity array.
- Dropping more elements from an array than it contains.
- A soft crash with no installed crash pad.
- Stack or local-storage corruption in a concrete implementation.

## Aliasing Rules

Builtin-style calls conceptually take outputs first, then inputs, then the function:

```text
outputs, inputs, FUNC
```

After the call writes to the outputs, only the output pointers remain on the stack. Outputs may not alias any other argument, whether input or output. A struct field aliases the containing struct.

## Types

The IR has these type kinds: `int`, `byte`, fixed-capacity stack arrays, slices, views, structs, and native function pointers.

Arrays are values laid out as `[len][data...]`, where `len` is a `count_t` and `data` has room for the array capacity.

Slices and views are values laid out as `[ptr][len]`. Slice mutation operations only accept `TYPE_SLICE`; indexing accepts both slices and views.

Each type records three sizes:

- `payload_size`: compact in-value size.
- `align`: natural alignment.
- `size`: stack allocation size.

Stack allocation size is rounded up to `alignof(Cell)`, not `sizeof(Cell)`. On a 32-bit target with 4-byte cell alignment, a 3-byte payload occupies 4 stack bytes rather than a full 8-byte cell.
