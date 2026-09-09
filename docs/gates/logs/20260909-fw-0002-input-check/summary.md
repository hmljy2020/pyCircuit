# FW-0002 follow-up: count bound Queue inputs

- Date: 2026-09-09; branch: `feat/davincioo-rob`.
- Base commit: `0e11f0e93bde4668f8a5b43c48b644e0864fbc7f`.
- Current uncommitted frontend fix; existing FW-0001/FW-0002 native tools are
  unchanged. All commands use `/home/lc/.codex/skills/pyc6/scripts/run.sh`.
- Decision 0221, CMT application under Decision 0222; no API or timing changes.

## Cause and correction

The last FW-0002 source-parameter check ran before static specialization and
counted CMT's four structural constants as Queue payloads. Both expanded CMT
and its static-loop equivalent failed with `require exactly one Queue input`.
The earlier 8-case CMT run preceded that check and did not validate the final
snapshot. Its original summary now explicitly records that limitation.

The check now runs on bound `rule_input_names` during ACIR emission, after
constants have been specialized away and state owners resolved. A real second
Queue still fails closed. The compiler's Rule/Firing checks remain in place.
CMT implementation and behavior/NDF are unchanged. A design-local test replaces
the 16 expanded history updates with a static loop in memory and compares raw
ACIR against the expanded implementation, ignoring only the source-derived JIT
cache fingerprint; operations, constants, types and bindings must match exactly.

## Validation

Artifacts: `.pycircuit_out/fw-0002-input-check/`; CMT models and offline replay:
`.pycircuit_out/davincioo-cmt/20260909-input-check/`.

- `python -m pytest tests/python/agentic-circuit/python_frontend/test_blocking_branch.py tests/python/agentic-circuit/python_frontend/test_queue_frontend.py tests/integration/agentic-circuit/e2e/test_blocking_branch.py -q --tb=short`: 155 passed, 5.74 seconds.
- CMT: `PYC_DAVINCIOO_CMT_OUT=/home/lc/pyCircuit/.pycircuit_out/davincioo-cmt/20260909-input-check python -m pytest designs/davincioo/tests/spe/ooo/test_cmt.py -q --tb=short`; 8 behavior scenarios passed in 110.95 seconds (`cmt.log`).
  The new loop comparison initially failed only on the source-derived JIT hash.
  After restricting normalization to that provenance field,
  `python -m pytest designs/davincioo/tests/spe/ooo/test_cmt.py -k static_history_loop -q --tb=short` passed (1 test, 2.75 seconds; `loop.log`).
- ROB: `python -m pytest designs/davincioo/tests/spe/ooo/test_rob.py designs/davincioo/tests/spe/ooo/test_rob_single.py -q --tb=short`; output directories overridden to `rob-dual`/`rob-single` under local artifacts; 9 passed, 75.46 seconds (`rob.log`).
- Contract/IR coverage freshness checks: 2 passed, 20.67 seconds (`contracts.log`).
- Changed-file pre-commit and `git diff --check`: passed (`precommit.log`); final source hashes: `source-sha256.txt`.

Offline replay was regenerated. Browser checks for normal, diagnostics,
backpressure and recovery passed with zero errors (`browser-results.json`).

No commit, upstream submission, full integration, or Verilator claim. The older
full-repository strict decision check's missing historical evidence remains
outside this focused frontend correction.
