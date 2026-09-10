# Structured pure helper functions

- Date: 2026-09-10; branch: `feat/davincioo-rob`.
- Tested worktree base: `c533f3858ce2c68132e05a8fb66ed24c3dbf9fbc`.
- Implementation commit: `b5cc964a313d63cca66ba2a7fd2f92d3c97ae888`.
- Decisions: 0230 structured and multi-result helpers; 0222 DavinciOO CMT application.
- Framework issue: FW-0006.
- Environment: `/home/lc/.codex/skills/pyc6/scripts/run.sh`.

## Verified behavior

Pure helpers now accept typed local assignment and rebinding, immutable record-field updates,
finite nested branches and one final return. Branch joins lower to `ac.var.select`. A fixed
`tuple[T0, ...]` annotation lowers to multiple SSA results and requires exact direct unpacking
in helpers and rules.

ACIR verifies every result and replaces every result of an inline call. QueueGraph preserves an
ordinary multi-result helper as one C++ call using `std::tuple`; PYC expands the helper once and
maps every yield. A forged QueueGraph result type is rejected before code generation.

DavinciOO CMT uses a three-result helper for diagnostic count, overflow and dropped-count updates.
All nine module scenarios passed and generated replay pages under
`.pycircuit_out/helper-upgrade/cmt/`.

## Results

- AC G0: 383 passed, 4 skipped.
- Focused helper frontend: 11 passed.
- Focused pure-helper MLIR: 1 passed, including positive multi-result inline and zero-result
  rejection.
- ACIR operation unit suite: 1844 passed.
- QueueGraph plan/backend suite: 77 passed; generated multi-result C++ compiled.
- Focused PYC G2: canonical PYC, C++ and Verilog generated; no residual `func.call`; the Clang 22
  C++ model transformed input 7 to output 8.
- DavinciOO CMT: 9 passed in 109.28 seconds with replay output.
- Changed-file pre-commit and `mkdocs build` passed.
- Non-strict decision status passed with 230 covered rows and no deferred decisions.

The strict decision-status check failed only because Decisions 0176--0210 reference absent
historical `20260906-*` evidence paths. The report is preserved in
`decision_status_report.json`; Decision 0230 itself has concrete existing evidence here.

The broad `check-acir` lane reported 196 passed, 2 unsupported and one known failure in
`CodeGen/emit-cxx-current.mlir`. It reaches the pre-existing FW-0004 `acir-build` host-link
failure. Focused helper MLIR, native, CMT and PYC lanes are green.

See `commands.txt` for the exact final gate commands and the adjacent bounded logs for results.
