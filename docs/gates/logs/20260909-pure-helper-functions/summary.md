# Typed pure helper functions

- Date: 2026-09-09; branch: `feat/davincioo-rob`.
- Base commit: `d491f936ee6cca1be83274216cac5173255eec30`.
- Decisions: 0229 helper semantics; 0222 DavinciOO CMT application.
- Framework issue: FW-0006.
- Environment: `/home/lc/.codex/skills/pyc6/scripts/run.sh`.
- All tools and libraries were built from this checkout under
  `.pycircuit_out/local-clang22/build`; no artifacts were copied from another
  worktree.

## Verified behavior

An ordinary top-level typed helper lowers to private `func.func` plus typed
`func.call`, survives QueueGraph planning, and becomes a real typed gfsim C++
helper call. `@ac.inline` records mandatory compiler intent; the ACIR pass
expands all calls and removes the helper before topology freeze. The PYC
generator recursively expands ordinary helper expressions and emits no
software call. Frontend and native verification reject malformed types,
non-helper callees and recursive source or forged plan call graphs.

DavinciOO CMT now shares its handoff identity predicate and saturating
diagnostic arithmetic through helpers. Its nine single-module tests pass with
normal, invalid, backpressure, capacity, diagnostics, recovery, reset and dual
flow isolation scenarios. Recording parity and generated offline replay HTML
remain part of the test output under `.pycircuit_out/helper-dev/cmt/`.

## Results

- AC G0: 376 Agentic Circuit frontend, contract and CLI tests passed; 4 were
  skipped.
- Focused frontend helper tests passed, including public API, nested keyword
  calls, explicit inline metadata and recursion/body rejection.
- Focused MLIR: 2 tests passed for verifier/inline behavior and PYC expansion.
- ACIR operation unit suite: 1844 tests passed.
- QueueGraph plan/backend suite: 76 tests passed; generated helper C++ also
  compiled inside the focused test.
- Focused PYC G2: helper expansion produced canonical PYC, pyCircuit 6 C++ and
  Verilog; the emitted C++ model ran and transformed input 7 to output 16.
- DavinciOO CMT: 9 tests passed in 109.38 seconds and generated replay pages.
- Changed-file pre-commit and non-strict `mkdocs build` passed.

The strict decision-status checker accepted Decision 0229 and its concrete
evidence, then failed because Decisions 0176--0210 refer to historical
`20260906-*` evidence paths that are absent from this checkout. Strict MkDocs
likewise stopped on 17 pre-existing repository-link warnings. The bounded logs
and generated decision-status report preserve both repository-level gaps.

The broad `check-acir` lane reported 196 passed, 2 unsupported and one known
failure in `CodeGen/emit-cxx-current.mlir`. That test reaches the pre-existing
FW-0004 `acir-build` host-link command, which omits ACIRBindings and LLVM
support libraries. The focused helper MLIR, QueueGraph, gfsim and PYC lanes are
green; this evidence does not claim FW-0004 or the complete AC G1 lane fixed.

The generated helper PYC and Verilog were checked without Verilator, consistent
with the current task scope. CMT remains a provisional-Table design and does
not claim PYC/RTL support.

See `commands.txt` for the exact lane commands and the adjacent bounded logs
for their outputs.
