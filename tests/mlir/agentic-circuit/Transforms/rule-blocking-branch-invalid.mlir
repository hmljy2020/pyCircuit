// RUN: not %acir_opt %s 2>&1 | %FileCheck %s
// CHECK: state proposal presence must imply the rule condition

module attributes {ac.contract_epoch = "0.5", ac.model_kind = "queue_graph", ac.queue_graph_domain = "cycle", ac.system = "branch_local"} {
  ac.type_scope @types {
    ac.struct @Command fields [{name = "select_right", type = i1}, {name = "value", type = i8}]
  } {dlti.dl_spec = #dlti.dl_spec<!ac.struct<@types::@Command> = {abi_alignment = 1 : i64, endianness = "little", preferred_alignment = 1 : i64, size = 2 : i64}>}
  ac.table @left entry i8 entries 1 init 0 owner "/" stable_id "table/left"
  ac.table @right entry i8 entries 1 init 0 owner "/" stable_id "table/right"
  %input = ac.source depth 1 latency 1 {ac.name = "input"} : !ac.queue<!ac.struct<@types::@Command>>
  ac.rule %input depths [] latencies [] name "route" stable_id "route_0"
      domain "cycle" type exact {
  ^body(%item: !ac.var<!ac.struct<@types::@Command>>):
    %zero = ac.var.constant 0 : i8 as !ac.var<i8>
    %select_right = ac.var.get %item field "select_right" : !ac.var<!ac.struct<@types::@Command>> -> !ac.var<i1>
    %value = ac.var.get %item field "value" : !ac.var<!ac.struct<@types::@Command>> -> !ac.var<i8>
    %false = ac.var.constant false as !ac.var<i1>
    %select_left = ac.var.cmp "eq" %select_right, %false : !ac.var<i1> -> !ac.var<i1>
    %candidate = ac.var.cmp "ne" %value, %zero : !ac.var<i8> -> !ac.var<i1>
    %right_present = ac.var.and %candidate, %select_right : !ac.var<i1>
    %left_present = ac.var.and %candidate, %select_left : !ac.var<i1>
    ac.rule.condition %candidate : !ac.var<i1>
    ac.table.propose @right[%false] = %value when %select_right : !ac.var<i1>
        mode "replace" write_fields ["$entry"] : !ac.var<i1>, !ac.var<i8>
    ac.table.propose @left[%false] = %value when %left_present : !ac.var<i1>
        mode "replace" write_fields ["$entry"] : !ac.var<i1>, !ac.var<i8>
    ac.rule.return
  } : (!ac.queue<!ac.struct<@types::@Command>>) -> ()
}
