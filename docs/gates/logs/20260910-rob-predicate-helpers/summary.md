# ROB predicate helper refactor

- Date: 2026-09-10; branch: `feat/davincioo-rob`.
- Base commit: `f83dd23ef8e2dc1a43afc8ec7f701a9550e22508`.
- Decisions: 0222 DavinciOO design program and 0230 pure helper semantics.
- Environment: `/home/lc/.codex/skills/pyc6/scripts/run.sh`.

The ROB flush, CMT acknowledgement, completion and allocation eligibility
predicates moved into four typed pure helpers. The five rules retain all Queue
consumption, Table access, state writes, arbitration and atomic commit effects.
Generated ACIR contains one `func.call` per predicate use, and generated gfsim
C++ contains one ordinary helper call per use.

## Results

- Single ROB: 4 scenarios passed in 31.08 seconds.
- Dual ROB: 5 scenarios passed in 44.30 seconds.
- Capacity, backpressure, invalid response, recovery, conflict and flow
  isolation behavior remained unchanged.
- Both runs repeated their observations with recording enabled and generated
  offline replay pages.
- Changed-file pre-commit and repository read-only contract check passed.

Replay indexes are disposable outputs at
`.pycircuit_out/rob-predicate-helpers/single/index.html` and
`.pycircuit_out/rob-predicate-helpers/dual/index.html`.
