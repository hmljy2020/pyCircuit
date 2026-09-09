// RUN: %acir_opt --pass-pipeline='builtin.module(ac-verify-pure-helpers,ac-freeze-topology)' %s -o %t.frozen.mlir
// RUN: %acir_queue_pycgen %t.frozen.mlir | %FileCheck %s

module attributes {ac.contract_epoch = "0.5", ac.model_kind = "queue_graph", ac.queue_graph_domain = "cycle", ac.system = "helper_pipeline"} {
  func.func private @plus_one(%arg0: !ac.var<i8>) -> !ac.var<i8> attributes {ac.helper = true, ac.inline = false} {
    %one = ac.var.constant 1 : i8 as !ac.var<i8>
    %result = ac.var.add %arg0, %one : !ac.var<i8>
    return %result : !ac.var<i8>
  }
  %input = ac.source depth 1 latency 1 {ac.name = "input"} : !ac.queue<i8>
  %output = ac.transform %input depths [1] latencies [1] {
  ^body(%item: !ac.var<i8>):
    %result = func.call @plus_one(%item) : (!ac.var<i8>) -> !ac.var<i8>
    ac.transform.yield %result : !ac.var<i8>
  } {ac.name = "output"} : (!ac.queue<i8>) -> !ac.queue<i8>
  ac.sink %output {ac.name = "sink"} : !ac.queue<i8>
}

// CHECK: pyc.constant 1 : i8
// CHECK: pyc.add
// CHECK-NOT: func.call
// CHECK-NOT: plus_one
