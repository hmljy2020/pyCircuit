# FW-0002: candidate-qualified nested state branches

- Date: 2026-09-09; branch: `feat/davincioo-rob`.
- Base: `0e11f0e93bde4668f8a5b43c48b644e0864fbc7f`.
- Local uncommitted framework fix and CMT rewrite; tools rebuilt from this checkout.
- Decisions: 0207 initial restriction extended by 0221; design application under 0222.
- Dependency: existing uncommitted FW-0001 pre-inference CSE/footprint fix.
- No commits, issue submissions, pushes or upstream resolution claimed.

## Correction to final-snapshot validation

The CMT 8-case run below preceded the last source-parameter arity check. That
check incorrectly counted static arguments as Queue inputs and rejected CMT.
The earlier run is historical evidence only, not proof that the final recorded
source snapshot passed CMT. The follow-up moves the check after binding and
reruns CMT plus an expanded-versus-loop ACIR comparison. See
[corrected validation](../20260909-fw-0002-input-check/summary.md).

## Behavior and implementation

A one-input outputless rule retains its blocking candidate and qualifies selected
state writes by candidate AND branch. Live Rule/Firing and frozen QueueGraph
independently prove implication using the same conservative Boolean DAG algorithm.
Unknown proofs fail closed; existing index safety, owner joins and write batches
remain enforced. Python API and CMT port/state contracts are unchanged.

The generic fixture proves invalid input while idle/busy changes only diagnostics,
valid idle input commits business state atomically, and valid busy input remains
queued. It checks all observed state/Queue boundaries and scan/activation commit
parity for both nested list branches. The flat harness supplies explicit activation
edges for its one firing; the CMT tests additionally verify generated structured
module activation and work-closure metadata. CMT accept and NDF changed together.

## Commands and results

All environment-dependent commands use `/home/lc/.codex/skills/pyc6/scripts/run.sh`.
Full local artifacts: `.pycircuit_out/fw-0002/`.

- Before fix: public Python fixture fails with ACPY-RULE-011 (`before.log`).
- Native build: `cmake --build .pycircuit_out/local-clang22/build --target acir-opt acir-opt-internal acir-queue-plan acir-queue-cxxgen CodeGenTests -j 4`.
- Python: `python -m pytest tests/python/agentic-circuit/python_frontend tests/python/agentic-circuit/contracts tests/python/agentic-circuit/cli/test_all_commands.py tests/python/agentic-circuit/cli/test_workspace.py tests/integration/agentic-circuit/e2e/test_blocking_branch.py -q --tb=short`.
  Initial run: 312 passed, 4 skipped, two coverage-ledger freshness failures.
  Regenerated with `tools/check-pyc-inventory.py --write-ledger` and
  `tools/agentic-circuit/check-ir-coverage.py --write-ledger`; follow-up results
  are recorded in `python-final.log`. The two stale ledgers and a corrected
  CMT evidence link pass their follow-up checks (9 passed, 25.40 seconds).
  Four skips belong to optional
  native-tool/pinned-toolchain cases in the existing suite.
- Focused MLIR: new blocking branch positive/negative fixtures, original branch
  fixture and FW-0001 regression: 4 passed. Broader rule fixtures: 20 passed (`lit-final.log`).
- Frozen plan native test: `CodeGenTests --gtest_filter=QueueGraphPlanTest.BlockingBranchPresenceRequiresCandidateConjunct` passed; covers missing qualification,
  OR rejection, false presence, shared DAGs and reassociated conjunctions.
  Final native run also includes the existing shared-DAG write-exclusion test: 2 passed.
- CMT: `PYC_DAVINCIOO_CMT_OUT=.pycircuit_out/davincioo-cmt/20260909-fw-0002 python -m pytest designs/davincioo/tests/spe/ooo/test_cmt.py -q --tb=short`: 8 passed, 110.30 seconds.
- ROB and shared contracts: `python -m pytest designs/davincioo/tests/spe/ooo/test_rob.py designs/davincioo/tests/spe/ooo/test_rob_single.py designs/davincioo/tests/test_contracts.py -q --tb=short`: 13 passed, 81.54 seconds; output directories `rob-dual` and `rob-single` under local artifacts.
- Browser: normal, diagnostics, backpressure, recovery; every committed projection
  checked, exact u64 values, navigation/play/pause/seek and offline operation;
  zero errors. Results: `browser-results.json`.

- Changed-file pre-commit and `git diff --check`: passed.
- Decision metadata checks: passed, 228 rows, no deferred decisions. The stricter
  existing-evidence check remains incomplete for the historical paths below.
- Reviewable source hashes: `source-sha256.txt`; FW-0002 remains pending user review.

## Boundaries

No full multi-module integration or Verilator/G2 claim. Selected outputs,
multi-input blocking branches, early-return combinations and lazy reads remain
outside this extension. FW-0003/FW-0004 are unchanged.

Strict decision-status validation finds missing historical evidence under
`docs/gates/logs/20260906-*/` for Decisions 0176–0210; this checkout does not contain
those artifacts. This is reported as a validation gap, not filled with invented
results. Current FW-0002 evidence is retained here. See `decision.log`.
