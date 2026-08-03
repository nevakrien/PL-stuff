#!/bin/sh
set -eu

output=$(printf '2 Print Ret\nFunc Lol (2 Print) Ret\nLol Ret\nFunc　打印二　(2　Print)　Ret\n打印二 Ret\n2　Print　Ret\nVar　Slice　Int　值　Ret\nStart (1 Print) Finally (2 Print) End Ret\nLoop Defer (2 Print) If (1) Break Done Again Ret\nFunc First\nRet\nFunc Second First Ret\nSecond Ret\n:quit\n' | ./pl_repl)
expected='2
ran
defined Lol [0]
2
ran
defined 打印二 [1]
2
ran
2
ran
ran
1
2
ran
2
ran
defined First [2]
defined Second [3]
ran'

if [ "$output" != "$expected" ]; then
	printf 'unexpected REPL output:\n%s\n' "$output" >&2
	exit 1
fi

printf 'repl test passed\n'
