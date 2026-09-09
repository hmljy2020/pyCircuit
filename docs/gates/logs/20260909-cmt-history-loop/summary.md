# CMT static history-clear loop

- Base commit: `0e11f0e93bde4668f8a5b43c48b644e0864fbc7f`.
- Branch: `feat/davincioo-rob`; local uncommitted changes, 2026-09-09.
- Decisions: 0221 (static rule expansion), 0222 (DavinciOO module development).
- Environment: `$pyc6` fixed environment; current checkout and locally built tools,
  including the FW-0002 bound-input-check correction.

CMT accept now clears the 16 history valid bits with `for i in range(16)`.
The loop expands at compile time and remains inside the same atomic transaction;
other fields, module ports and scheduling contracts are unchanged. The accept
NDF explicitly describes the static expansion and one-transaction requirement.
The existing equivalence test now expands the authored loop in memory and compares
all ACIR except the source-derived JIT fingerprint.

Validation command (through `/home/lc/.codex/skills/pyc6/scripts/run.sh`):

```bash
PYC_DAVINCIOO_CMT_OUT=/home/lc/pyCircuit/.pycircuit_out/davincioo-cmt/20260909-history-loop python -m pytest designs/davincioo/tests/spe/ooo/test_cmt.py -q --tb=short
```

Results: all 9 tests passed (`test.log`). This covers 8 behavior/recording scenarios and the static-loop
ACIR equivalence test. Generated models and offline replay are under
`.pycircuit_out/davincioo-cmt/20260909-history-loop/`.
Changed-file checks: `precommit.log`; source revisions: `source-sha256.txt`.
No framework semantics changed; no Verilator or whole-design integration claim.
