#span = {path = "table_values.py", line = 1 : i64, column = 1 : i64, end_line = 1 : i64, end_column = 2 : i64}
#origin = {site = {definition = @table_values.Top, ast_path = []}, expansion = []}
#branch_site = {site = {definition = @table_values.Top, ast_path = [{kind = "field", name = "body"}]}, expansion = []}
#w0 = #ac.static_expr<{kind = "literal", location = #span, origin = #origin, value = {kind = "integer", value = #ac.math_int<0>}}>
#w1 = #ac.static_expr<{kind = "literal", location = #span, origin = #origin, value = {kind = "integer", value = #ac.math_int<1>}}>
#w2 = #ac.static_expr<{kind = "literal", location = #span, origin = #origin, value = {kind = "integer", value = #ac.math_int<2>}}>
#w3 = #ac.static_expr<{kind = "literal", location = #span, origin = #origin, value = {kind = "integer", value = #ac.math_int<3>}}>
#w6 = #ac.static_expr<{kind = "literal", location = #span, origin = #origin, value = {kind = "integer", value = #ac.math_int<6>}}>
#w8 = #ac.static_expr<{kind = "literal", location = #span, origin = #origin, value = {kind = "integer", value = #ac.math_int<8>}}>
#w70 = #ac.static_expr<{kind = "literal", location = #span, origin = #origin, value = {kind = "integer", value = #ac.math_int<70>}}>
!b1 = !ac.bits<#w1>
!b2 = !ac.bits<#w2>
!b3 = !ac.bits<#w3>
!b6 = !ac.bits<#w6>
!b8 = !ac.bits<#w8>
!b70 = !ac.bits<#w70>
!grid = !ac.table<[#w2, #w3], !b8>
!transposed = !ac.table<[#w3, #w2], !b8>
!row = !ac.table<[#w3], !b8>
!flags = !ac.table<[#w3], !b1>
!indices = !ac.table<[#w3], !b2>
!record = !ac.struct<"table_values.Record">
!mode = !ac.enum<"table_values.Mode">
!modes = !ac.table<[#w2], !mode>
!records = !ac.table<[#w2], !record>
#owner = {package = "", path = "table_values.py"}

// Exercise linked-IR primitives directly, including raw get and choose.
module attributes {ac.linked_implicit_guards, ac.source_units = [#owner], ac.stage = "linked-source"} {
  "ac.enum"() {sym_name = "table_values.Mode", width = #ac.math_int<2>, encoding = "explicit", members = [{name = "idle", code = #ac.math_int<1>}, {name = "ready", code = #ac.math_int<3>}], ac.source_owner = #owner, ac.declaration_role = "definition"} : () -> ()
  ac.struct "table_values.Record" fields [{name = "row", type = !row}, {name = "mode", type = !mode}] {ac.source_owner = #owner, ac.declaration_role = "definition"}

  "ac.module"() ({
  ^bb0(%grid: !grid, %threshold: !b8, %row_index: !b70, %column_index: !b70, %lookup: !b70, %update: !b1, %fallback: !b8, %clk: !b1, %reset: !b1):
    %row = "ac.table.create"(%threshold, %fallback, %threshold) : (!b8, !b8, !b8) -> !row
    %alternate = "ac.table.splat"(%fallback) {shape = [#w3]} : (!b8) -> !row
    %write_index = "ac.table.index"(%column_index) {shape = [#w3]} : (!b70) -> !b2
    %indices = "ac.table.splat"(%write_index) {shape = [#w3]} : (!b2) -> !indices
    %candidate_row = "ac.table.map"(%row, %indices, %alternate) ({
    ^bb0(%i: !b2, %old: !b8, %target: !b2, %replacement: !b8):
      %hit = "ac.bits.compare"(%i, %target) {predicate = "eq"} : (!b2, !b2) -> !b1
      %word = "ac.bits.select"(%hit, %replacement, %old) : (!b1, !b8, !b8) -> !b8
      "ac.yield"(%word) : (!b8) -> ()
    }) {shape = [#w3], operandSegmentSizes = array<i32: 3, 0>} : (!row, !indices, !row) -> !row
    %write_flags = "ac.table.splat"(%update) {shape = [#w3]} : (!b1) -> !flags
    %updated_row = "ac.table.map"(%write_flags, %candidate_row, %row) ({
    ^bb0(%i: !b2, %enable_write: !b1, %candidate: !b8, %previous: !b8):
      %chosen = "ac.bits.select"(%enable_write, %candidate, %previous) : (!b1, !b8, !b8) -> !b8
      "ac.yield"(%chosen) : (!b8) -> ()
    }) {shape = [#w3], operandSegmentSizes = array<i32: 3, 0>} : (!flags, !row, !row) -> !row
    %last_lane = "ac.bits.constant"() {value = #w2} : () -> !b2
    %updated_word = "ac.table.read"(%updated_row, %last_lane) : (!row, !b2) -> !b8
    %candidate_sum = "ac.table.fold"(%candidate_row) {kind = "add"} : (!row) -> !b8
    %replicated = "ac.table.splat"(%row) {shape = [#w2]} : (!row) -> !grid
    %transposed = "ac.table.view"(%grid) {kind = "transpose", parameters = {axes = [#w1, #w0]}} : (!grid) -> !transposed
    %mapped, %ordinals, %unused_table = "ac.table.map"(%grid, %replicated, %threshold, %lookup) ({
    ^bb0(%ordinal: !b3, %left: !b8, %right: !b8, %capture: !b8, %capture_index: !b70):
      %total = "ac.bits.binary"(%left, %right) {opcode = "add"} : (!b8, !b8) -> !b8
      %r = "ac.bits.binary"(%total, %capture) {opcode = "add"} : (!b8, !b8) -> !b8
      %i = "ac.bits.resize"(%ordinal) {mode = "zext"} : (!b3) -> !b8
      %local_table = "ac.table.splat"(%left) {shape = [#w3]} : (!b8) -> !row
      %unused_entry, %unused_range = "ac.table.get"(%local_table, %capture_index) : (!row, !b70) -> (!b8, !b1)
      "ac.yield"(%r, %i, %unused_entry) : (!b8, !b8, !b8) -> ()
    }) {shape = [#w2, #w3], operandSegmentSizes = array<i32: 2, 2>} : (!grid, !grid, !b8, !b70) -> (!grid, !grid, !grid)
    %coordinate = "ac.table.index"(%row_index, %column_index) {shape = [#w2, #w3]} : (!b70, !b70) -> !b3
    %payload, %valid = "ac.table.get"(%grid, %lookup) : (!grid, !b70) -> (!b8, !b1)
    %selected = "ac.bits.select"(%valid, %payload, %fallback) : (!b1, !b8, !b8) -> !b8
    %dead, %membership = "ac.table.get"(%grid, %row_index) : (!grid, !b70) -> (!b8, !b1)
    %discarded = "ac.bits.binary"(%dead, %fallback) {opcode = "xor"} : (!b8, !b8) -> !b8
    %lane = "ac.bits.constant"() {value = #w1} : () -> !b2
    %read = "ac.table.read"(%row, %lane) : (!row, !b2) -> !b8
    %mask = "ac.table.match"(%grid, %threshold) ({
    ^bb0(%entry: !b8, %capture: !b8):
      %hit = "ac.bits.compare"(%entry, %capture) {predicate = "uge"} : (!b8, !b8) -> !b1
      "ac.yield"(%hit) : (!b1) -> ()
    }) : (!grid, !b8) -> !b6
    %low0, %low1, %low_valid0, %low_valid1 = "ac.table.choose"(%grid, %mask) {count = 2 : i64, policy = "first", order = "low"} : (!grid, !b6) -> (!b3, !b3, !b1, !b1)
    %high0, %high1, %high_valid0, %high_valid1 = "ac.table.choose"(%grid, %mask) {count = 2 : i64, policy = "first", order = "high"} : (!grid, !b6) -> (!b3, !b3, !b1, !b1)
    %sum = "ac.table.fold"(%row) {kind = "add"} : (!row) -> !b8
    %product = "ac.table.fold"(%row) {kind = "mul"} : (!row) -> !b8
    %idle = "ac.enum.create"() {member = "idle"} : () -> !mode
    %ready = "ac.enum.create"() {member = "ready"} : () -> !mode
    %modes = "ac.table.create"(%idle, %ready) : (!mode, !mode) -> !modes
    %mode, %unused = "ac.table.get"(%modes, %lane) : (!modes, !b2) -> (!mode, !b1)
    %base = ac.struct.create(%row, %mode) : (!row, !mode) -> !record
    %merged, %enable = "ac.value.merge"(%base, %update, %alternate) {paths = [["row"]], operandSegmentSizes = array<i32: 1, 1, 1>} : (!record, !b1, !row) -> (!record, !b1)
    %new_row = ac.struct.get %merged["row"] : (!record) -> !row
    %new_mode = ac.struct.get %merged["mode"] : (!record) -> !mode
    %mode_bits = "ac.enum.to_bits"(%new_mode) : (!mode) -> !b2
    %records = "ac.table.create"(%base, %merged) : (!record, !record) -> !records
    %picked, %picked_valid = "ac.table.get"(%records, %row_index) : (!records, !b70) -> (!record, !b1) loc("table_values.py":10:1)
    %picked_row = ac.struct.get %picked["row"] : (!record) -> !row
    %picked_word = "ac.table.read"(%picked_row, %lane) : (!row, !b2) -> !b8
    %protected_word = "ac.bits.select"(%picked_valid, %picked_word, %threshold) : (!b1, !b8, !b8) -> !b8
    %checked = "ac.method.call"() {callee = @table_values.Top::@checked_lookup, instance_path = [], occurrence = #origin, ac.implicit_path, ac.registration_name = "checked_lookup"} : () -> !b8
    "ac.method"(%grid, %lookup, %fallback) ({
    ^bb0(%guard_grid: !grid, %guard_index: !b70, %guard_fallback: !b8):
      "ac.yield"() : () -> ()
    }, {
    ^bb0(%body_grid: !grid, %body_index: !b70, %body_fallback: !b8):
      %divisor, %in_range = "ac.table.get"(%body_grid, %body_index) : (!grid, !b70) -> (!b8, !b1)
      %result = "ac.branch"(%in_range) ({
        %quotient = "ac.bits.binary"(%body_fallback, %divisor) {opcode = "udiv"} : (!b8, !b8) -> !b8
        "ac.yield"(%quotient) : (!b8) -> ()
      }, {
        "ac.yield"(%body_fallback) : (!b8) -> ()
      }) {occurrence = #branch_site, ac.condition_location = #span} : (!b1) -> !b8
      "ac.yield"(%result) : (!b8) -> ()
    }) {sym_name = "checked_lookup", sym_visibility = "private", function_type = () -> !b8, argument_names = [], occurrence = #origin, ac.guard_location = #span, ac.parameters = [], ac.result_constraints = [{kind = "hardware", source_kind = "fixed_bits", type = !b8}], ac.return_form = "single"} : (!grid, !b70, !b8) -> ()
    "ac.yield"(%mapped, %ordinals, %transposed, %replicated, %coordinate, %selected, %valid, %membership, %read, %mask, %low0, %low1, %low_valid0, %low_valid1, %high0, %high1, %high_valid0, %high_valid1, %sum, %product, %new_row, %mode_bits, %enable, %checked, %protected_word, %updated_word, %candidate_sum) : (!grid, !grid, !transposed, !grid, !b3, !b8, !b1, !b1, !b8, !b6, !b3, !b3, !b1, !b1, !b3, !b3, !b1, !b1, !b8, !b8, !row, !b2, !b1, !b8, !b8, !b8, !b8) -> () loc("table_values.py":12:1)
  }) {sym_name = "table_values.Top", source_owner = #owner, parameters = [], type_parameters = [], ac.domain_inputs = {clock = 7 : i64, reset = 8 : i64}, function_type = (!grid, !b8, !b70, !b70, !b70, !b1, !b8, !b1, !b1) -> (!grid, !grid, !transposed, !grid, !b3, !b8, !b1, !b1, !b8, !b6, !b3, !b3, !b1, !b1, !b3, !b3, !b1, !b1, !b8, !b8, !row, !b2, !b1, !b8, !b8, !b8, !b8), input_names = ["grid", "threshold", "row_index", "column_index", "lookup", "update", "fallback", "pyc_clk", "pyc_rst"], output_names = ["mapped", "ordinals", "transposed", "replicated", "coordinate", "selected", "valid", "membership", "read", "mask", "low0", "low1", "low_valid0", "low_valid1", "high0", "high1", "high_valid0", "high_valid1", "sum", "product", "row", "mode", "enable", "checked", "protected_word", "updated_word", "candidate_sum"]} : () -> ()
  "ac.system"() {entry = {callee = @table_values.Top, parameters = [], type_arguments = []}, domain = "default"} : () -> ()
}
