// RUN: %pycircuit_opt %s -o %t.mlir
// RUN: %pycircuit_opt %t.mlir --ac-lower-source-integer-expressions | %FileCheck %s
// The second invocation reparses the transport in a fresh compiler context.
#span = {path = "integer.py", line = 1 : i64, column = 1 : i64, end_line = 1 : i64, end_column = 2 : i64}
#occ = {site = {definition = @runtime, ast_path = []}, expansion = []}
#w8 = #ac.static_expr<{kind = "literal", value = {kind = "integer", value = #ac.math_int<8>}, origin = #occ, location = #span}>
#w16 = #ac.static_expr<{kind = "literal", value = {kind = "integer", value = #ac.math_int<16>}, origin = #occ, location = #span}>
!b8 = !ac.bits<#w8>
!b16 = !ac.bits<#w16>
module attributes {ac.stage = "source-i64"} {
  // CHECK-LABEL: func.func @runtime
  // CHECK: ac.bits.binary
  // CHECK-NOT: arith.
  // CHECK-NOT: ac.source.integer
  func.func @runtime(%input: !b8) -> !b16 {
    %x = "ac.source.integer_from_bits"(%input) {domain = {lower = #ac.math_int<0>, upper = #ac.math_int<256>}, is_signed = false, occurrence = #occ, source_location = #span} : (!b8) -> i64
    %one = arith.constant 1 : i64
    %sum = arith.addi %x, %one : i64
    %product = arith.muli %sum, %one : i64
    %result = "ac.source.integer_to_bits"(%product) {occurrence = #occ, source_location = #span} : (i64) -> !b16
    return %result : !b16
  }
  // CHECK-LABEL: func.func @static_shift
  // CHECK: ac.bits.constant
  // CHECK-NOT: arith.
  // CHECK-NOT: ac.source.integer
  func.func @static_shift() -> !b8 {
    %value = arith.constant 128 : i64
    %count = arith.constant 3 : i64
    %shifted = arith.shrsi %value, %count : i64
    %result = "ac.source.integer_to_bits"(%shifted) {occurrence = #occ, source_location = #span} : (i64) -> !b8
    return %result : !b8
  }
}
