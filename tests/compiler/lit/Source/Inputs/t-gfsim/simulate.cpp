#include "model.h"

#include <cassert>
#include <cstdint>
#include <cstdio>
#include <string>

using gfsim::Bits;
using gfsim::t_gfsim::Model;

template <class T>
concept PhysicalClock = requires(T &model) { model.in_pyc_clk; };
template <class T>
concept PhysicalReset = requires(T &model) { model.in_pyc_rst; };
static_assert(!PhysicalClock<Model> && !PhysicalReset<Model>);

static void step(Model &model) {
  if (!model.Step()) {
    std::fprintf(stderr, "%s\n", model.error.c_str());
    assert(false);
  }
  assert(model.error.empty());
}

static void work(Model &model) {
  assert(model.Work());
  assert(model.error.empty());
}

static void cycle(Model &model, bool reset = false) {
  if (reset) {
    model.Reset();
    assert(model.cycle() == 0);
    assert(!model.Xfer());
    work(model);
  } else {
    const auto previous = model.cycle();
    work(model);
    work(model);
    assert(model.cycle() == previous);
    assert(model.Xfer());
    assert(model.cycle() == previous + 1);
    assert(!model.Xfer());
    assert(model.cycle() == previous + 1);
  }
}

#if defined(MULTI_OUTPUTS) || defined(GROUPED_OUTPUTS)
int main() {
  Model model;
  for (unsigned value : {0x03u, 0x6fu, 0xb8u}) {
    const auto enable = value & 1u;
    model.in_word = Bits<8>{value};
    model.in_enable = Bits<1>{enable};
    step(model);
    assert(model.out_result_0 == Bits<4>{(value + 1u) & 15u});
    assert(model.out_result_1 == Bits<8>{value ^ 255u});
    assert(model.out_result_2 == Bits<1>{enable});
  }
#ifdef MULTI_OUTPUTS
  // Decode directly forwards enable through two output endpoints.
  assert(&model.out_result_2 == &model.in_enable);
#endif
}
#elif defined(FANOUT)
int main() {
  Model model;
  model.in_value = Bits<8>{254};
  step(model);
  assert(model.out_result_0 == Bits<8>{255});
  assert(model.out_result_1 == Bits<8>{0});
  assert(model.out_result_2 == Bits<8>{0});

  model.in_value = Bits<8>{66};
  step(model);
  assert(model.out_result_0 == Bits<8>{67});
  assert(model.out_result_1 == Bits<8>{68});
  assert(model.out_result_2 == Bits<8>{188});
}
#elif defined(INTERLEAVED)
int main() {
  Model model;
  for (unsigned value : {0u, 66u, 254u, 255u}) {
    model.in_value = Bits<8>{value};
    step(model);
    const auto incrementedTwice = (value + 2u) & 255u;
    assert(model.out_result == Bits<8>{incrementedTwice ^ 255u});
  }
}
#elif defined(STAGE)
int main() {
  Model manual, automatic;
  assert(!manual.Xfer());
  assert(manual.cycle() == 0);
  manual.in_update = automatic.in_update = Bits<1>{1};
  manual.in_value = Bits<8>{23};
  work(manual);
  assert(manual.out_result_0 == Bits<8>{9});
  assert(manual.cycle() == 0);
  manual.in_value = automatic.in_value = Bits<8>{42};
  work(manual); // Replace the uncommitted 23 with 42.
  assert(manual.out_result_0 == Bits<8>{9});
  assert(manual.Xfer());
  step(automatic);
  assert(manual.cycle() == 1 && automatic.cycle() == 1);
  assert(!manual.Xfer());
  assert(manual.cycle() == 1);
  manual.in_update = automatic.in_update = Bits<1>{0};
  work(manual);
  work(automatic);
  assert(manual.out_result_0 == Bits<8>{42});
  assert(manual.out_result_1 == Bits<8>{42 ^ 255});
  assert(manual.out_result_0 == automatic.out_result_0);
  assert(manual.Xfer());
  manual.in_value = Bits<8>{77};
  step(manual); // A disabled update holds committed state across cycles.
  assert(manual.out_result_0 == Bits<8>{42});
  manual.in_update = Bits<1>{1};
  manual.in_value = Bits<8>{77};
  work(manual);
  manual.Reset(); // Discard pending 77 and restore the initial state.
  assert(manual.cycle() == 0);
  assert(!manual.Xfer());
  manual.in_update = Bits<1>{0};
  work(manual);
  assert(manual.out_result_0 == Bits<8>{9});
}
#elif defined(SWAP_STATE)
int main() {
  Model model;
  auto check = [&](unsigned left, unsigned right) {
    assert(model.out_result_0.field_0[0] == Bits<8>{left});
    assert(model.out_result_1[0] == Bits<8>{right});
  };
  model.in_divisor = Bits<8>{1};
  model.in_healthy = Bits<1>{1};
  model.in_word = Bits<8>{64};
  work(model);
  work(model); // Reusing successful Work must leave aggregate Q untouched.
  check(9, 23);
  model.in_divisor = Bits<8>{0};
  assert(!model.Work());
  assert(!model.error.empty());
  assert(!model.Xfer());
  assert(model.cycle() == 0);
  check(9, 23);
  model.in_divisor = Bits<8>{1};
  model.in_healthy = Bits<1>{0};
  assert(!model.Step());
  assert(!model.Xfer());
  check(9, 23);
  model.in_healthy = Bits<1>{1};
  step(model);
  check(23, 9); // Direct state outputs reflect the committed Q.
  work(model);
  work(model);
  check(23, 9);
  assert(model.out_result_0.field_0[1] == Bits<8>{31});
  assert(model.out_result_1[1] == Bits<8>{12});
  assert(model.out_result_2 == Bits<8>{64 ^ 9});
  assert(model.Xfer());
  work(model);
  check(9, 23); // Aggregate updates swap snapshots, never aliases.
  model.Reset();
  assert(model.cycle() == 0);
  assert(!model.Xfer());
  work(model);
  check(9, 23);
}
#elif defined(CACHED_WORK)
int main() {
  Model model, other;
  assert(&model.out_result_2 != &other.out_result_2);
  auto mix = [](std::uint32_t word) {
    std::uint32_t left = word, right = ~word;
    for (unsigned i = 0; i < 24; ++i) {
      left = (left << 1) ^ (left >> 3) ^ 17;
      right = (right << 2) ^ (right >> 1) ^ 41;
    }
    return left ^ right;
  };
  struct Inputs { std::uint32_t first, second, noise; };
  constexpr Inputs samples[] = {
      {3, 17, 0}, {57, 91, 9}, {42, 41, 3},
      {42, 41, 19}, {101, 41, 19}, {101, 202, 19},
  };
  std::uint32_t first = 3, second = 5;
  for (const auto &sample : samples) {
    model.in_next_word = Bits<32>{sample.first};
    model.in_next_other = Bits<32>{sample.second};
    model.in_noise = Bits<32>{sample.noise};
    work(model);
    work(model); // Re-evaluation does not advance state.
    assert(model.out_result_0.field_0 == Bits<32>{mix(first)});
    assert(model.out_result_0.field_1 == Bits<32>{mix(second) ^ sample.noise});
    assert(model.out_result_1.field_0 == Bits<32>{first});
    assert(model.out_result_1.field_1 == Bits<32>{second});
    assert(model.out_result_2 == Bits<8>{51});
    assert(model.Xfer());
    first = sample.first;
    second = sample.second;
    // Identical inputs in the next epoch must observe the newly committed Q.
    work(model);
    work(model);
    assert(model.out_result_0.field_0 == Bits<32>{mix(first)});
    assert(model.out_result_0.field_1 == Bits<32>{mix(second) ^ sample.noise});
    assert(model.out_result_1.field_0 == Bits<32>{first});
    assert(model.out_result_1.field_1 == Bits<32>{second});
  }
  model.Reset();
  assert(!model.Xfer());
  work(model); // The same inputs must now observe reset Q, not cached Q.
  work(model);
  assert(model.out_result_0.field_0 == Bits<32>{mix(3)});
  assert(model.out_result_0.field_1 == Bits<32>{mix(5) ^ 19});
  assert(model.out_result_1.field_0 == Bits<32>{3});
  assert(model.out_result_1.field_1 == Bits<32>{5});
}
#elif defined(METHODS)
int main() {
  Model model;
  cycle(model, true);
  assert(model.out_result_0 == Bits<8>{4});
  assert(model.out_result_1 == Bits<8>{4});

  model.in_limit = Bits<8>{6};
  model.in_value = Bits<8>{7};
  model.in_select = Bits<1>{0};
  cycle(model);
  // The method on the other arm also participates in admission.
  assert(model.out_result_1 == Bits<8>{4});

  model.in_limit = Bits<8>{8};
  cycle(model);
  assert(model.out_result_0 == Bits<8>{4});
  // The second call reads the transaction candidate written by the first.
  assert(model.out_result_1 == Bits<8>{8});

  model.in_value = Bits<8>{4};
  model.in_select = Bits<1>{1};
  cycle(model);
  assert(model.out_result_0 == Bits<8>{5});
  assert(model.out_result_1 == Bits<8>{8});

  model.in_limit = Bits<8>{15};
  model.in_select = Bits<1>{0};
  model.in_value = Bits<8>{11};
  cycle(model);
  assert(model.out_result_0 == Bits<8>{5});
  assert(model.out_result_1 == Bits<8>{12});

  model.in_select = Bits<1>{1};
  model.in_value = Bits<8>{2};
  cycle(model);
  // Only the right receiver's committed state now closes admission.
  assert(model.out_result_0 == Bits<8>{5});
  assert(model.out_result_1 == Bits<8>{12});
}
#elif defined(QUEUE_ADD)
int main() {
  Model model;
  model.in_left = Bits<16>{7};
  model.in_right = Bits<16>{35};
  cycle(model, true);
  assert(model.out_result == Bits<16>{0});
  model.in_left = Bits<16>{1};
  model.in_right = Bits<16>{2};
  work(model); // This pending packet must be replaced, not queued.
  model.in_left = Bits<16>{7};
  model.in_right = Bits<16>{35};
  cycle(model); // Only the latest packet pair enters empty inputs.
  assert(model.out_result == Bits<16>{0});

  model.in_left = Bits<16>{9};
  model.in_right = Bits<16>{51};
  cycle(model); // Full inputs cannot admit replacement; old pair transfers.
  assert(model.out_result == Bits<16>{0});
  cycle(model); // New pair enters; first sum pops.
  assert(model.out_result == Bits<16>{42});
  cycle(model); // New pair transfers; no new result yet.
  assert(model.out_result == Bits<16>{42});
  cycle(model); // Second sum pops.
  assert(model.out_result == Bits<16>{60});

  cycle(model, true);
  assert(model.out_result == Bits<16>{0});
}
#elif defined(CHECKED_QUEUE_ADD)
int main() {
  Model model;
  cycle(model, true);
  for (unsigned i = 0; i < 7; ++i)
    cycle(model);
}
#elif defined(TABLE_STATE)
int main() {
  Model model;
  auto check = [&](unsigned lane, unsigned left, unsigned right) {
    const auto &row = model.out_result_0[lane];
    assert(row.field_0 == Bits<8>{left});
    assert(row.field_1 == Bits<8>{right});
  };

  model.in_healthy = Bits<1>{1};
  cycle(model, true);
  check(0, 1, 2);
  check(1, 3, 4);
  check(2, 5, 6);

  model.in_index = Bits<1>{1};
  model.in_value = Bits<8>{21};
  model.in_other = Bits<8>{34};
  cycle(model);
  check(0, 1, 2);
  check(1, 21, 34);
  check(2, 34, 21);
  assert(model.out_result_1 == Bits<8>{21}); // Own candidate.
  assert(model.out_result_2 == Bits<8>{3}); // Independent snapshot.
  work(model);
  work(model);
  assert(model.out_result_2 == Bits<8>{21}); // Same inputs, newly committed Q.
  check(1, 21, 34);

  model.in_index = Bits<1>{0};
  model.in_value = Bits<8>{55};
  model.in_other = Bits<8>{89};
  model.in_healthy = Bits<1>{0};
  assert(!model.Step());
  check(0, 1, 2);
  check(1, 21, 34);
  check(2, 34, 21);

  model.in_healthy = Bits<1>{1};
  step(model);
  check(0, 55, 89);
  check(1, 21, 34);
  check(2, 89, 55);
  cycle(model, true);
  check(0, 1, 2);
  check(1, 3, 4);
  check(2, 5, 6);
}
#elif defined(TABLE_QUEUE)
int main() {
  Model model;
  cycle(model, true);
  model.in_value = Bits<8>{40};
  model.in_issue = Bits<1>{1};
  model.in_take = Bits<1>{1};
  cycle(model);
  assert(model.out_result.field_1 == Bits<8>{0});

  model.in_value = Bits<8>{89};
  model.in_issue = Bits<1>{0};
  cycle(model);
  assert(model.out_result.field_1 == Bits<8>{0});

  cycle(model);
  const auto &packet = model.out_result;
  assert(packet.field_0[0].field_0 == Bits<8>{40});
  assert(packet.field_0[0].field_1 == Bits<8>{2});
  assert(packet.field_0[1].field_0 == Bits<8>{3});
  assert(packet.field_0[1].field_1 == Bits<8>{4});
  assert(packet.field_0[2].field_0 == Bits<8>{5});
  assert(packet.field_0[2].field_1 == Bits<8>{41});
  assert(packet.field_1 == Bits<8>{42});

  cycle(model, true);
  assert(model.out_result.field_0[0].field_0 == Bits<8>{0});
  assert(model.out_result.field_1 == Bits<8>{0});
}
#elif defined(COLLECTION_FAMILY)
int main() {
  Model model;
  model.in_first = Bits<8>{10};
  model.in_second = Bits<8>{20};
  auto check = [&](unsigned left, unsigned right) {
    assert(model.out_result[0].field_0 == Bits<8>{left});
    assert(model.out_result[1].field_0 == Bits<8>{right});
  };

  cycle(model, true);
  check(3, 3);
  model.in_enable[0] = Bits<1>{1};
  cycle(model);
  check(3, 3); // The assembled Table samples committed child outputs in Work.
  model.in_enable[0] = Bits<1>{0};
  model.in_enable[1] = Bits<1>{1};
  model.in_index = Bits<1>{1};
  cycle(model);
  check(14, 3);
  cycle(model);
  check(14, 24);
  assert(model.out_result[0].field_1 == Bits<8>{11});
  assert(model.out_result[1].field_1 == Bits<8>{21});
  work(model);
  work(model);
  check(14, 45); // Each committed cycle adds 21 exactly once to the second cell.
  model.Reset();
  assert(!model.Xfer());
  work(model);
  work(model);
  check(3, 3); // Reset invalidates pending work with identical collection inputs.
}
#elif defined(TIMED_QUEUE)
int main() {
  Model model;
  cycle(model, true);
  model.in_value = Bits<8>{10};
  model.in_issue = Bits<1>{1};
  model.in_inspect = Bits<1>{1};
  model.in_take = Bits<1>{1};
  cycle(model);
  assert(model.out_result_0 == Bits<8>{0});
  assert(model.out_result_1 == Bits<8>{0});

  model.in_issue = Bits<1>{0};
  model.in_inspect = Bits<1>{0};
  model.in_take = Bits<1>{0};
  cycle(model);
  cycle(model); // Latency advances across idle cycles.
  model.in_inspect = Bits<1>{1};
  cycle(model);
  assert(model.out_result_0 == Bits<8>{10});
  assert(model.out_result_1 == Bits<8>{0});

  model.in_issue = Bits<1>{1};
  model.in_take = Bits<1>{1};
  model.in_value = Bits<8>{20};
  cycle(model); // Peek and pop read the same head; push is blocked.
  assert(model.out_result_1 == Bits<8>{10});
  model.in_value = Bits<8>{30};
  cycle(model); // Push again once there is committed capacity.
  assert(model.out_result_1 == Bits<8>{10});

  model.in_issue = Bits<1>{0};
  cycle(model);
  assert(model.out_result_1 == Bits<8>{10});
  cycle(model);
  assert(model.out_result_1 == Bits<8>{10});
  cycle(model);
  assert(model.out_result_0 == Bits<8>{30});
  assert(model.out_result_1 == Bits<8>{30});
}
#elif defined(CONFLICTING_POPS)
int main() {
  Model model;
  model.in_healthy = Bits<1>{1};
  cycle(model, true);
  assert(model.observations.empty()); // Reset and Work do not publish.
  model.in_issue = Bits<1>{1};
  work(model);
  assert(model.observations.empty());
  assert(model.Xfer());
  auto checkObservation = [&](unsigned issue) {
    assert(model.observations.size() == 1);
    assert(model.observations[0].event == "issue");
    assert(model.observations[0].value ==
           gfsim::t_gfsim::Format(Bits<1>{issue}));
  };
  checkObservation(1); // Latest Work replaces the earlier issue=0 sample.
  assert(model.out_result_2 == Bits<8>{1});
  assert(model.out_result_3 == Bits<8>{0});

  model.in_issue = Bits<1>{0};
  model.in_take_first = Bits<1>{1};
  work(model);
  work(model); // A successful pending pop must be invalidated by later failure.
  checkObservation(1);
  assert(model.out_result_0 == Bits<8>{0});
  assert(model.out_result_2 == Bits<8>{1});
  model.in_take_second = Bits<1>{1};
  const auto beforeFailure = model.cycle();
  assert(!model.Step());
  checkObservation(1);
  assert(!model.Xfer());
  assert(model.cycle() == beforeFailure);
  assert(!model.error.empty());
  assert(model.out_result_0 == Bits<8>{0});
  assert(model.out_result_1 == Bits<8>{0});
  assert(model.out_result_2 == Bits<8>{1});

  model.in_take_second = Bits<1>{0};
  model.in_overwrite = Bits<1>{1};
  assert(!model.Step());
  checkObservation(1);
  assert(!model.error.empty());
  assert(model.out_result_0 == Bits<8>{0});
  assert(model.out_result_2 == Bits<8>{1});

  model.in_overwrite = Bits<1>{0};
  model.in_healthy = Bits<1>{0};
  assert(!model.Step());
  checkObservation(1);
  assert(model.error.find("injected check failure") != std::string::npos);
  assert(model.out_result_0 == Bits<8>{0});
  assert(model.out_result_2 == Bits<8>{1});

  // Failed Work leaves both queue and state unchanged.
  model.in_healthy = Bits<1>{1};
  work(model);
  checkObservation(1); // Successful Work also waits for Xfer to publish.
  assert(model.Xfer());
  checkObservation(0);
  assert(model.out_result_0 == Bits<8>{37});
  assert(model.out_result_1 == Bits<8>{0});
  assert(model.out_result_2 == Bits<8>{2});
  assert(model.out_result_3 == Bits<8>{1});
  model.Reset();
  assert(model.observations.empty());
  assert(model.cycle() == 0);
  assert(!model.Xfer());
}
#endif
