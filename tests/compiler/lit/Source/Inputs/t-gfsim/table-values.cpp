#include "model.h"

#include <cassert>
#include <initializer_list>

template <class Table>
void expect(const Table &table, std::initializer_list<unsigned> expected) {
  assert(table.size == expected.size());
  std::size_t ordinal = 0;
  for (auto value : expected)
    assert(table[ordinal++].value() == value);
}

int main() {
  using gfsim::Bits;
  gfsim::t_gfsim::Model model;
  for (std::size_t i = 0; i < 6; ++i)
    model.in_grid[i] = Bits<8>{i + 1};
  assert(gfsim::hardware_traits<decltype(model.in_grid)>::pack(model.in_grid)
             .value() == 0x010203040506ULL);
  model.in_threshold = Bits<8>{4};
  model.in_fallback = Bits<8>{9};
  model.in_row_index = Bits<70>{1};
  model.in_column_index = Bits<70>{2};
  model.in_lookup = Bits<70>{3};
  model.in_update = Bits<1>{1};
  assert(model.Work());
  assert(model.cycle() == 0);

  assert(model.out_updated_word.value() == 9);
  assert(model.out_candidate_sum.value() == 22);
  expect(model.out_replicated, {4, 9, 4, 4, 9, 4});
  expect(model.out_transposed, {1, 4, 2, 5, 3, 6});
  expect(model.out_mapped, {9, 15, 11, 12, 18, 14});
  expect(model.out_ordinals, {0, 1, 2, 3, 4, 5});
  assert(model.out_coordinate.value() == 5);
  assert(model.out_valid.toBool() && model.out_selected.value() == 4);
  assert(model.out_checked.value() == 2);
  assert(model.out_protected_word.value() == 9);
  assert(model.out_membership.toBool() && model.out_read.value() == 9);
  assert(model.out_mask.value() == 56);
  assert(model.out_low0.value() == 3 && model.out_low1.value() == 4);
  assert(model.out_high0.value() == 5 && model.out_high1.value() == 4);
  assert(model.out_low_valid0.toBool() && model.out_low_valid1.toBool());
  assert(model.out_high_valid0.toBool() && model.out_high_valid1.toBool());
  assert(model.out_sum.value() == 17 && model.out_product.value() == 144);
  expect(model.out_row, {9, 9, 9});
  assert(model.out_mode.value() == 3 && model.out_enable.toBool());

  assert(model.Xfer());
  assert(model.cycle() == 1);
  assert(!model.Xfer());

  // Leave the low word unchanged. Any narrowing before the bounds check
  // would select the same row or payload as the first step.
  model.in_row_index = Bits<70>{1, 2};
  model.in_lookup = Bits<70>{3, 2};
  model.in_threshold = Bits<8>{7};
  model.in_update = Bits<1>{0};
  assert(model.Work());
  assert(model.Work());
  assert(model.Xfer());
  assert(model.cycle() == 2);
  assert(model.out_coordinate.value() == 6);
  assert(!model.out_valid.toBool() && model.out_selected.value() == 9);
  assert(model.out_checked.value() == 9);
  assert(model.out_protected_word.value() == 7);
  assert(!model.out_membership.toBool());
  assert(model.out_mask.value() == 0);
  assert(!model.out_low_valid0.toBool() && !model.out_low_valid1.toBool());
  assert(!model.out_high_valid0.toBool() && !model.out_high_valid1.toBool());
  expect(model.out_row, {7, 9, 7});
  assert(model.out_mode.value() == 3 && !model.out_enable.toBool());

  assert(model.out_updated_word.value() == 7); // Disabled, despite a valid index.
  assert(model.out_candidate_sum.value() == 25);

  model.in_update = Bits<1>{1};
  model.in_column_index = Bits<70>{3};
  assert(model.Work());
  assert(model.out_updated_word.value() == 7); // Index 3 matches no lane.
  assert(model.out_candidate_sum.value() == 23);
}
