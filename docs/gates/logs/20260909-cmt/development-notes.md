# CMT development findings

- Initial CMT `accept` used an optional nested `if not busy` branch. It retained
  state but consumed a valid next request while occupied. The backpressure and
  recovery expected-result tests detected this design-source bug.
- Expressing the correction as a blocking outer guard with nested conditional
  state effects is rejected by the existing frontend with `ACPY-RULE-011:
  nested conditional state effects inside a blocking guard require explicit
  CFG implication proof`. The final source uses supported value selection and
  one blocking guard, with the same accepted module behavior. The nested-guard authoring limitation remains.
- ReplaySession explicitly rejects resetting registered state during recording
  (`replay: finish recording before reset`). Reset is tested on separate
  unrecorded scan/activation models; the reset scenario replay shows the clean
  post-reset operation only. Continuous recording across reset is outside the
  existing recorder contract, not a viewer rendering defect.

- That supported value-selection form exposed a separate generic CSE bug:
  two live identical Table reads survived pre-inference DCE, but post-lowering
  CSE removed one and left stale per-operation footprints. The minimal
  `rule-cse-footprints.mlir` failed before the fix with footprints=4,
  operations=3. `addRuleLoweringPipeline` now runs CSE before effect inference.
  The verifier is unchanged and still rejects forged/stale evidence.
- Broad ACIR validation reached 191 passes, two unsupported cases and one
  linker failure in `CodeGen/emit-cxx-current.mlir`: the generated acir-build
  link omitted ACIRBindings/LLVM support dependencies required by libgfsim.
  That case runs freeze/codegen rather than the changed rule-lowering pipeline.
  The CMT driver links these dependencies explicitly as the ROB test does.
