// RUN: %split_file %s %t
// RUN: %not %acir_opt %t/rule-candidate.mlir 2>&1 | %FileCheck %s --check-prefix=RULE-CANDIDATE
// RUN: %not %acir_opt %t/rule-output.mlir 2>&1 | %FileCheck %s --check-prefix=RULE-OUTPUT
// RUN: %not %acir_opt %t/rule-proposal.mlir 2>&1 | %FileCheck %s --check-prefix=RULE-PROPOSAL
// RUN: %not %acir_opt %t/firing-candidate.mlir 2>&1 | %FileCheck %s --check-prefix=FIRING-CANDIDATE
// RUN: %not %acir_opt %t/firing-output.mlir 2>&1 | %FileCheck %s --check-prefix=FIRING-OUTPUT
// RUN: %not %acir_opt %t/firing-proposal.mlir 2>&1 | %FileCheck %s --check-prefix=FIRING-PROPOSAL

//--- rule-candidate.mlir
module attributes {ac.contract_epoch = "0.5"} {
  %input = "builtin.unrealized_conversion_cast"() : () -> !ac.queue<i8>
  %output = ac.rule %input depths [1] latencies [1] name "bad" stable_id "bad" domain "cycle" type exact {
  ^body(%item: !ac.var<i8>):
    %true = ac.var.constant true as !ac.var<i1>
    ac.rule.condition %item : !ac.var<i8>
    ac.rule.output %item when %true ordinal 0 : !ac.var<i8>, !ac.var<i1>
    ac.rule.return %item : !ac.var<i8>
  } : (!ac.queue<i8>) -> !ac.queue<i8>
}
// RULE-CANDIDATE: 'ac.rule.condition' op condition must be !ac.var<i1>

//--- rule-output.mlir
module attributes {ac.contract_epoch = "0.5"} {
  %input = "builtin.unrealized_conversion_cast"() : () -> !ac.queue<i8>
  %output = ac.rule %input depths [1] latencies [1] name "bad" stable_id "bad" domain "cycle" type exact {
  ^body(%item: !ac.var<i8>):
    %true = ac.var.constant true as !ac.var<i1>
    ac.rule.condition %true : !ac.var<i1>
    ac.rule.output %item when %item ordinal 0 : !ac.var<i8>, !ac.var<i8>
    ac.rule.return %item : !ac.var<i8>
  } : (!ac.queue<i8>) -> !ac.queue<i8>
}
// RULE-OUTPUT: 'ac.rule.output' op condition must be !ac.var<i1>

//--- rule-proposal.mlir
module attributes {ac.contract_epoch = "0.5"} {
  ac.table @state entry i8 entries 1 init 0 owner "/" stable_id "table/state"
  %input = "builtin.unrealized_conversion_cast"() : () -> !ac.queue<i8>
  ac.rule %input depths [] latencies [] name "bad" stable_id "bad" domain "cycle" type exact {
  ^body(%item: !ac.var<i8>):
    %true = ac.var.constant true as !ac.var<i1>
    ac.rule.condition %true : !ac.var<i1>
    %index = ac.var.constant false as !ac.var<i1>
    ac.table.propose @state[%index] = %item when %item : !ac.var<i8>
        mode "replace" write_fields ["$entry"] : !ac.var<i1>, !ac.var<i8>
    ac.rule.return
  } : (!ac.queue<i8>) -> ()
}
// RULE-PROPOSAL: 'ac.table.propose' op condition must be !ac.var<i1>

//--- firing-candidate.mlir
module attributes {ac.contract_epoch = "0.5"} {
  %input = "builtin.unrealized_conversion_cast"() : () -> !ac.queue<i8>
  %output = ac.firing %input depths [1] latencies [1] stable_id "bad" domain "cycle" {
  ^body(%item: !ac.var<i8>):
    %true = ac.var.constant true as !ac.var<i1>
    ac.firing.condition %item : !ac.var<i8>
    ac.firing.output %item when %true ordinal 0 : !ac.var<i8>, !ac.var<i1>
    ac.firing.yield %item : !ac.var<i8>
  } : (!ac.queue<i8>) -> !ac.queue<i8>
}
// FIRING-CANDIDATE: 'ac.firing.condition' op condition must be !ac.var<i1>

//--- firing-output.mlir
module attributes {ac.contract_epoch = "0.5"} {
  %input = "builtin.unrealized_conversion_cast"() : () -> !ac.queue<i8>
  %output = ac.firing %input depths [1] latencies [1] stable_id "bad" domain "cycle" {
  ^body(%item: !ac.var<i8>):
    %true = ac.var.constant true as !ac.var<i1>
    ac.firing.condition %true : !ac.var<i1>
    ac.firing.output %item when %item ordinal 0 : !ac.var<i8>, !ac.var<i8>
    ac.firing.yield %item : !ac.var<i8>
  } : (!ac.queue<i8>) -> !ac.queue<i8>
}
// FIRING-OUTPUT: 'ac.firing.output' op condition must be !ac.var<i1>

//--- firing-proposal.mlir
module attributes {ac.contract_epoch = "0.5"} {
  ac.table @state entry i8 entries 1 init 0 owner "/" stable_id "table/state"
  %input = "builtin.unrealized_conversion_cast"() : () -> !ac.queue<i8>
  ac.firing %input depths [] latencies [] stable_id "bad" domain "cycle" {
  ^body(%item: !ac.var<i8>):
    %true = ac.var.constant true as !ac.var<i1>
    ac.firing.condition %true : !ac.var<i1>
    %index = ac.var.constant false as !ac.var<i1>
    ac.table.propose @state[%index] = %item when %item : !ac.var<i8>
        mode "replace" write_fields ["$entry"] : !ac.var<i1>, !ac.var<i8>
    ac.firing.yield
  } : (!ac.queue<i8>) -> ()
}
// FIRING-PROPOSAL: 'ac.table.propose' op condition must be !ac.var<i1>
