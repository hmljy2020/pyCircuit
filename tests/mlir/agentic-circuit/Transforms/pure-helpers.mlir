// RUN: %split_file %s %t
// RUN: %acir_opt --pass-pipeline='builtin.module(ac-verify-pure-helpers,ac-inline-pure-helpers)' %t/valid.mlir | %FileCheck %s
// RUN: %not %acir_opt --pass-pipeline='builtin.module(ac-verify-pure-helpers)' %t/recursive.mlir 2>&1 | %FileCheck %s --check-prefix=RECURSIVE
// RUN: %not %acir_opt --pass-pipeline='builtin.module(ac-verify-pure-helpers)' %t/non-helper-call.mlir 2>&1 | %FileCheck %s --check-prefix=NON-HELPER
// RUN: %not %acir_opt --pass-pipeline='builtin.module(ac-verify-pure-helpers)' %t/no-input.mlir 2>&1 | %FileCheck %s --check-prefix=NO-INPUT
// RUN: %not %acir_opt --pass-pipeline='builtin.module(ac-verify-pure-helpers)' %t/no-result.mlir 2>&1 | %FileCheck %s --check-prefix=NO-RESULT

//--- valid.mlir
module attributes {ac.contract_epoch = "0.5"} {
  func.func private @plus_one(%arg0: !ac.var<i8>) -> !ac.var<i8> attributes {ac.helper = true, ac.inline = false} {
    %one = ac.var.constant 1 : i8 as !ac.var<i8>
    %result = ac.var.add %arg0, %one : !ac.var<i8>
    return %result : !ac.var<i8>
  }
  func.func private @twice(%arg0: !ac.var<i8>) -> !ac.var<i8> attributes {ac.helper = true, ac.inline = true} {
    %adjusted = func.call @plus_one(%arg0) : (!ac.var<i8>) -> !ac.var<i8>
    %result = ac.var.add %adjusted, %adjusted : !ac.var<i8>
    return %result : !ac.var<i8>
  }
  func.func private @classify(%arg0: !ac.var<i8>) -> (!ac.var<i8>, !ac.var<i1>) attributes {ac.helper = true, ac.inline = true} {
    %valid = ac.var.constant true as !ac.var<i1>
    return %arg0, %valid : !ac.var<i8>, !ac.var<i1>
  }
  %input = "builtin.unrealized_conversion_cast"() : () -> !ac.var<i8>
  %result = func.call @twice(%input) : (!ac.var<i8>) -> !ac.var<i8>
  %classified, %valid = func.call @classify(%input) : (!ac.var<i8>) -> (!ac.var<i8>, !ac.var<i1>)
  %used_value = ac.var.add %classified, %classified : !ac.var<i8>
  %used_valid = ac.var.not %valid : !ac.var<i1> -> !ac.var<i1>
}

// CHECK: func.func private @plus_one
// CHECK-NOT: func.func private @twice
// CHECK-NOT: func.func private @classify
// CHECK: %[[INPUT:.*]] = unrealized_conversion_cast
// CHECK: %[[ADJUSTED:.*]] = func.call @plus_one
// CHECK: ac.var.add %[[ADJUSTED]], %[[ADJUSTED]]
// CHECK: %[[VALID:.*]] = ac.var.constant true
// CHECK: ac.var.add %[[INPUT]], %[[INPUT]]
// CHECK: ac.var.not %[[VALID]]

//--- recursive.mlir
module attributes {ac.contract_epoch = "0.5"} {
  func.func private @left(%arg0: !ac.var<i8>) -> !ac.var<i8> attributes {ac.helper = true, ac.inline = false} {
    %result = func.call @right(%arg0) : (!ac.var<i8>) -> !ac.var<i8>
    return %result : !ac.var<i8>
  }
  func.func private @right(%arg0: !ac.var<i8>) -> !ac.var<i8> attributes {ac.helper = true, ac.inline = false} {
    %result = func.call @left(%arg0) : (!ac.var<i8>) -> !ac.var<i8>
    return %result : !ac.var<i8>
  }
}

// RECURSIVE: recursive pure helper call graph

//--- non-helper-call.mlir
module attributes {ac.contract_epoch = "0.5"} {
  func.func private @ordinary(%arg0: !ac.var<i8>) -> !ac.var<i8> {
    return %arg0 : !ac.var<i8>
  }
  func.func private @helper(%arg0: !ac.var<i8>) -> !ac.var<i8> attributes {ac.helper = true, ac.inline = false} {
    %result = func.call @ordinary(%arg0) : (!ac.var<i8>) -> !ac.var<i8>
    return %result : !ac.var<i8>
  }
}

// NON-HELPER: pure helper callee '@ordinary' is unresolved or not a pure helper

//--- no-input.mlir
module attributes {ac.contract_epoch = "0.5"} {
  func.func private @constant() -> !ac.var<i8> attributes {ac.helper = true, ac.inline = false} {
    %result = ac.var.constant 1 : i8 as !ac.var<i8>
    return %result : !ac.var<i8>
  }
}

// NO-INPUT: pure helper requires ac.var parameters and one or more ac.var results

//--- no-result.mlir
module attributes {ac.contract_epoch = "0.5"} {
  func.func private @discard(%arg0: !ac.var<i8>) attributes {ac.helper = true, ac.inline = false} {
    return
  }
}

// NO-RESULT: pure helper requires ac.var parameters and one or more ac.var results
