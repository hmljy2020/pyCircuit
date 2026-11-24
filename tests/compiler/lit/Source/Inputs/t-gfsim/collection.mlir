// This wrapper replaces the standalone FeedbackCell root of the linked unit.
#family_origin = {site = {definition = @linked_gfsim.designs.CollectionFamily, ast_path = []}, expansion = []}
#family_span = {path = "designs.py", line = 1 : i64, column = 1 : i64, end_line = 1 : i64, end_column = 2 : i64}
#family_one = #ac.static_expr<{kind = "literal", location = #family_span, origin = #family_origin, value = {kind = "integer", value = #ac.math_int<1>}}>
#family_two = #ac.static_expr<{kind = "literal", location = #family_span, origin = #family_origin, value = {kind = "integer", value = #ac.math_int<2>}}>
#family_eight = #ac.static_expr<{kind = "literal", location = #family_span, origin = #family_origin, value = {kind = "integer", value = #ac.math_int<8>}}>
!family_bit = !ac.bits<#family_one>
!family_word = !ac.bits<#family_eight>
!family_row = !ac.struct<"linked_gfsim.designs.TableRow">
!family_rows = !ac.table<[#family_two], !family_row>
!family_bits = !ac.table<[#family_two], !family_bit>

// Body
  "ac.module"() ({
  ^bb0(%first: !family_word, %second: !family_word, %enable: !family_bits, %index: !family_bit, %clock: !family_bit, %reset: !family_bit):
    %clock_lanes = "ac.table.splat"(%clock) {shape = [#family_two]} : (!family_bit) -> !family_bits
    %reset_lanes = "ac.table.splat"(%reset) {shape = [#family_two]} : (!family_bit) -> !family_bits
    %family = "ac.collection"(%rows, %enable, %clock_lanes, %reset_lanes) {instance_name = "cells", callee = @linked_gfsim.designs.FeedbackCell, parameters = [], type_arguments = [], shape = [#family_two], occurrence = #family_origin} : (!family_rows, !family_bits, !family_bits, !family_bits) -> !family_rows
    %selected = "ac.table.read"(%family, %index) : (!family_rows, !family_bit) -> !family_row
    %feedback = ac.struct.get %selected["right"] : (!family_row) -> !family_word
    %row0 = ac.struct.create(%feedback, %first) : (!family_word, !family_word) -> !family_row
    %row1 = ac.struct.create(%feedback, %second) : (!family_word, !family_word) -> !family_row
    %rows = "ac.table.create"(%row0, %row1) : (!family_row, !family_row) -> !family_rows
    "ac.yield"(%family) : (!family_rows) -> ()
  }) {sym_name = "linked_gfsim.designs.CollectionFamily", source_owner = {package = "linked_gfsim", path = "designs.py"}, parameters = [], type_parameters = [], function_type = (!family_word, !family_word, !family_bits, !family_bit, !family_bit, !family_bit) -> !family_rows, input_names = ["first", "second", "enable", "index", "pyc_clk", "pyc_rst"], output_names = ["result"], ac.domain_inputs = {clock = 4 : i64, reset = 5 : i64}} : () -> ()
