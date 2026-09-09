# FW-0005: field assignment shorthand

- Date: 2026-09-09; branch: `feat/davincioo-rob`.
- Base commit: `0e11f0e93bde4668f8a5b43c48b644e0864fbc7f`.
- Local uncommitted changes; depends on existing uncommitted FW-0001/FW-0002.
- Decisions: 0221 frontend/value semantics; 0222 CMT design application.
- Fixed environment: `/home/lc/.codex/skills/pyc6/scripts/run.sh`.
- Native tools: `.pycircuit_out/local-clang22/build/bin`; current-checkout build
  reports no work needed. No native backend implementation changed in FW-0005.
- No commit, remote issue, push or upstream resolution claimed.

## Behavior

Local field assignment produces a new value and preserves prior snapshots.
Captured record/list field assignment proposes a source-ordered update within
one atomic rule. Whole-record assignment and `with_fields` use the same path.
List write values are materialized once as typed SSA; subsequent indexed reads
select preceding proposals without changing committed storage. Equal resolved
indices join into one proposal. Existing bounds, disjointness and predicate
verifiers remain authoritative; there is no C++-only semantic patch.

The generic fixture independently checks local snapshot isolation, retained
fields, repeated dynamic-index updates, a later index rebind, both branch arms,
0/255 values, output backpressure, all committed state and scan/activation
commit parity. Both direct stores and explicit `with_fields` versions execute
against the same expected results. Malformed field names/types are rejected by
native verification. CMT clears histories through static-loop field assignment;
its NDF example is synchronized after functional validation.

## Commands and results

Final source validation: AC G0 **372 passed, 4 skipped**; generic AC G1 **3 passed**;
CMT **9 passed**, dual/single ROB **9 passed**. Four browser replay scenarios
passed (55/47/53/19 commits, exact integer values, forward/back seeking,
play/pause and offline-only requests, no browser errors).
Expanded native rule tests: **24 passed, 1 failed**. The malformed output-presence
fixture expects an i1 type error but receives an earlier implication error;
invalid IR remains rejected. See [FW-0007](../../../framework-issues/FW-0007-output-presence-diagnostic.md).
This is not a fully green native/release gate claim.

The four G0 skips are existing optional native/pinned-toolchain tests; the new
native field-store fixture ran without skips using the environment-selected tools.
Exploratory regressions in literal-context routing and unconditional presence
emission were corrected before this final run; existing tests are retained.
`baseline.log` reproduces rejection of all three target forms with the HEAD
Python frontend loaded separately; it does not claim a clean native baseline build.
Changed-file pre-commit and `git diff --check` passed. Decision status metadata
checks passed with no deferred/non-verified rows and concrete evidence fields;
existing-evidence closure was not rerun because its previously recorded historical
missing files are unrelated. All seven CMT NDF annotations and code excerpts
match source exactly. Logs are adjacent to this summary; `source-sha256.txt`
records the final changed-file snapshot.

All commands below use the fixed-environment wrapper.

- Build: `cmake --build .pycircuit_out/local-clang22/build --target acir-opt acir-opt-internal acir-queue-plan acir-queue-cxxgen -j 4`.
- AC G0: `python -m pytest -q tests/python/agentic-circuit/python_frontend tests/python/agentic-circuit/contracts tests/python/agentic-circuit/cli`.
- Focused AC G1: `python -m pytest -q tests/integration/agentic-circuit/e2e/test_field_assignment.py tests/integration/agentic-circuit/e2e/test_blocking_branch.py`.
- Existing MLIR rules: `lit -v .pycircuit_out/local-clang22/build/compiler/acir/tests/mlir --filter=rule-`.
- Designs: `python -m pytest -q designs/davincioo/tests/spe/ooo/test_cmt.py designs/davincioo/tests/spe/ooo/test_rob.py designs/davincioo/tests/spe/ooo/test_rob_single.py`.
- CMT artifacts: `.pycircuit_out/davincioo-cmt/20260909-field-assignment/`.
- ROB artifacts: `.pycircuit_out/fw-0005/rob-dual/` and `rob-single/`.
- Generic generated MLIR/C++ and executables: `.pycircuit_out/fw-0005/native-verified/`.

## Boundaries

Nested field/slice/augmented targets and direct Queue-input mutation remain
unsupported. Different dynamic write indices still need the existing disjointness
or mutual-exclusion proof. This task does not add pure helper functions (FW-0006).
No PYC lowering changed; Verilator/G2 and full release closure were not run.
Previously recorded FW-0003 reset-recording and FW-0004 linker limitations are
not claimed resolved. Existing historical decision-evidence gaps are separate
from this focused run.

Archive preparation strips trailing whitespace from text logs; diagnostic and result content is unchanged.
