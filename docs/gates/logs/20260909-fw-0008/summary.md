# FW-0008 native test expectation synchronization

- Date: 2026-09-09; branch: `feat/davincioo-rob`.
- Base: `a44611e0ed70765019e914d63d37e91c0600cd25`.
- Fix: current local uncommitted patch; no commit, push or upstream resolution claimed.
- Decision: 0221. No compiler semantics or public Python API changed.
- Environment: `/home/lc/.codex/skills/pyc6/scripts/run.sh`; all binaries were
  rebuilt from this checkout.

## Cause and fix

Commit `9582e4e4` added `ac.var.invariant` and `ac.var.invariant.yield` without
updating two exact native operation inventories. Commit `924daa28` admitted
nominal enum Table entries without updating one exact diagnostic expectation.

`OpsTest.cpp` now includes both operations in both inventories, expects 142
registered AC operations, and matches the current complete Table entry diagnostic.
The exact inventory and invalid-type rejection checks remain strict.

## Verification

All commands use the fixed-environment wrapper.

- Build targets `ACIROpsTests`, `acir-opt`, and `acir-opt-internal`: passed.
  Evidence: `build.log`.
- Complete `ACIROpsTests`: **1844 passed, 0 failed**.
  Evidence: `native-unit.log`.
- FW-0007 Rule/Firing regression selection: **28 passed**.
  Evidence: `lit-focused.log`.
- Complete ACIR lit: **194 passed, 2 unsupported, 1 failed**.
  The sole failure is `CodeGen/emit-cxx-current.mlir`, caused by the already
  recorded [FW-0004](../../../framework-issues/FW-0004-acir-build-link.md)
  generated-program link dependency issue. No new failure was discovered.
  Evidence: `lit-full-summary.log`.

Changed-file pre-commit, strict decision metadata validation and
`git diff --check` passed.
