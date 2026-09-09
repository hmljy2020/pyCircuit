# FW-0007: type diagnostics before predicate implication

- Date: 2026-09-09; branch: `feat/davincioo-rob`.
- Base: `a44611e0ed70765019e914d63d37e91c0600cd25`.
- Fix: current local uncommitted patch; no commit/push/upstream resolution claimed.
- Decision: 0221. No valid-program semantics or public Python API changes.
- Environment: `/home/lc/.codex/skills/pyc6/scripts/run.sh`; native tools and tests
  rebuilt from `.pycircuit_out/local-clang22/build` in this checkout.

## Cause and fix

FW-0002 switched to typed predicate implication in commit
`135dd90a078201a29baa807771db95e520a63d96`. Parent Rule/Firing verification could
therefore reject a non-Boolean presence as a failed implication before the child
operation reported its type error. This was a regression introduced by that
change, not an unrelated pre-existing test failure.

Parent verification now reuses `verifyI1VarCondition` on candidate, output
presence and non-null Table proposal presence before implication. The shared
proof remains unchanged and strict. The original negative test is unchanged;
a new split-file test covers all six Rule/Firing and predicate-position cases.

## Verification

All commands use the fixed-environment wrapper.

- Before fix: `lit -v .pycircuit_out/local-clang22/build/compiler/acir/tests/mlir --filter='ACIR/(rule-invalid|predicate-type-invalid)'`:
  both selected tests fail (`before.log`).
- Build: `cmake --build .pycircuit_out/local-clang22/build --target acir-opt acir-opt-internal acir-queue-plan acir-queue-cxxgen -j 4`:
  passed (`build.log`).
- Rule/Firing regression: `lit -v .pycircuit_out/local-clang22/build/compiler/acir/tests/mlir --filter='(rule-|firing-|predicate-type-invalid)'`:
  **28 passed**, including the original failing test and six new negative cases (`lit.log`).
- Generated gfsim: `python -m pytest -q tests/integration/agentic-circuit/e2e/test_field_assignment.py tests/integration/agentic-circuit/e2e/test_blocking_branch.py`:
  **3 passed** (`e2e.log`). Includes valid atomic transitions, backpressure,
  scheduling parity and malformed native IR rejection.
- CMT: `PYC_DAVINCIOO_CMT_OUT=.pycircuit_out/davincioo-cmt/20260909-fw-0007 python -m pytest -q designs/davincioo/tests/spe/ooo/test_cmt.py`:
  **9 passed** (`cmt.log`), including recording parity and HTML generation.
- The initial extended native unit run exposed three stale expectations, recorded
  as [FW-0008](../../../framework-issues/FW-0008-native-test-expectations.md). After
  synchronizing those expectations, the complete `ACIROpsTests` run is
  **1844 passed, 0 failed**. FW-0007's focused result remains **28 passed**.

No complete native/release gate claim is made. G0 and G2 were not rerun: no
Python frontend, PYC lowering, runtime or hardware behavior changed.
Text log archives strip trailing whitespace only.

Changed-file pre-commit, decision metadata validation and `git diff --check` passed.
Final source hashes are recorded in `source-sha256.txt`. Historical missing-evidence closure was not rerun.
