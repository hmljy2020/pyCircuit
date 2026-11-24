#include "model.h"

#include <cassert>
#include <cstdio>
#include <string>

using gfsim::Bits;
using gfsim::t_gfsim::Model;

static void step(Model &model) {
  if (!model.Step()) {
    std::fprintf(stderr, "%s\n", model.error.c_str());
    assert(false);
  }
  assert(model.error.empty());
}

#if !defined(MULTI_OUTPUTS) && !defined(GROUPED_OUTPUTS) && !defined(FANOUT) && \
    !defined(INTERLEAVED)
static void edge(Model &model, bool reset = false) {
  model.in_pyc_rst = Bits<1>{reset};
  model.in_pyc_clk = Bits<1>{0};
  step(model);
  model.in_pyc_clk = Bits<1>{1};
  step(model);
}
#endif

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
  Model model;
  model.in_update = Bits<1>{1};
  model.in_value = Bits<8>{23};
  edge(model, true);
  assert(model.out_result_0 == Bits<8>{9});

  model.in_pyc_rst = Bits<1>{0};
  model.in_pyc_clk = Bits<1>{0};
  step(model);
  assert(model.out_result_0 == Bits<8>{9});
  model.in_pyc_clk = Bits<1>{1};
  step(model);
  assert(model.out_result_0 == Bits<8>{23});
  assert(model.out_result_1 == Bits<8>{9 ^ 255});

  model.in_value = Bits<8>{42};
  step(model);
  assert(model.out_result_0 == Bits<8>{23});
  assert(model.out_result_1 == Bits<8>{23 ^ 255});
  model.in_update = Bits<1>{0};
  edge(model);
  assert(model.out_result_0 == Bits<8>{23});
  model.in_pyc_rst = Bits<1>{1};
  model.in_pyc_clk = Bits<1>{0};
  step(model);
  assert(model.out_result_0 == Bits<8>{23});
  model.in_pyc_clk = Bits<1>{1};
  step(model);
  assert(model.out_result_0 == Bits<8>{9});
}
#elif defined(METHODS)
int main() {
  Model model;
  edge(model, true);
  assert(model.out_result_0 == Bits<8>{4});
  assert(model.out_result_1 == Bits<8>{4});

  model.in_limit = Bits<8>{6};
  model.in_value = Bits<8>{7};
  model.in_select = Bits<1>{0};
  edge(model);
  // The method on the other arm also participates in admission.
  assert(model.out_result_1 == Bits<8>{4});

  model.in_limit = Bits<8>{8};
  edge(model);
  assert(model.out_result_0 == Bits<8>{4});
  // The second call reads the transaction candidate written by the first.
  assert(model.out_result_1 == Bits<8>{8});

  model.in_value = Bits<8>{4};
  model.in_select = Bits<1>{1};
  edge(model);
  assert(model.out_result_0 == Bits<8>{5});
  assert(model.out_result_1 == Bits<8>{8});

  model.in_limit = Bits<8>{15};
  model.in_select = Bits<1>{0};
  model.in_value = Bits<8>{11};
  edge(model);
  assert(model.out_result_0 == Bits<8>{5});
  assert(model.out_result_1 == Bits<8>{12});

  model.in_select = Bits<1>{1};
  model.in_value = Bits<8>{2};
  edge(model);
  // Only the right receiver's committed state now closes admission.
  assert(model.out_result_0 == Bits<8>{5});
  assert(model.out_result_1 == Bits<8>{12});
}
#elif defined(QUEUE_ADD)
int main() {
  Model model;
  model.in_left = Bits<16>{7};
  model.in_right = Bits<16>{35};
  edge(model, true);
  assert(model.out_result == Bits<16>{0});
  edge(model); // Packet pair enters empty inputs.
  assert(model.out_result == Bits<16>{0});

  model.in_left = Bits<16>{9};
  model.in_right = Bits<16>{51};
  edge(model); // Full inputs cannot admit replacement; old pair transfers.
  assert(model.out_result == Bits<16>{0});
  edge(model); // New pair enters; first sum pops.
  assert(model.out_result == Bits<16>{42});
  edge(model); // New pair transfers; no new result yet.
  assert(model.out_result == Bits<16>{42});
  edge(model); // Second sum pops.
  assert(model.out_result == Bits<16>{60});

  edge(model, true);
  assert(model.out_result == Bits<16>{0});
}
#elif defined(CHECKED_QUEUE_ADD)
int main() {
  Model model;
  edge(model, true);
  for (unsigned i = 0; i < 7; ++i)
    edge(model);
}
#elif defined(TIMED_QUEUE)
int main() {
  Model model;
  edge(model, true);
  model.in_value = Bits<8>{10};
  model.in_issue = Bits<1>{1};
  model.in_inspect = Bits<1>{1};
  model.in_take = Bits<1>{1};
  edge(model);
  assert(model.out_result_0 == Bits<8>{0});
  assert(model.out_result_1 == Bits<8>{0});

  model.in_issue = Bits<1>{0};
  model.in_inspect = Bits<1>{0};
  model.in_take = Bits<1>{0};
  edge(model);
  edge(model); // Latency advances across idle edges.
  model.in_inspect = Bits<1>{1};
  edge(model);
  assert(model.out_result_0 == Bits<8>{10});
  assert(model.out_result_1 == Bits<8>{0});

  model.in_issue = Bits<1>{1};
  model.in_take = Bits<1>{1};
  model.in_value = Bits<8>{20};
  edge(model); // Peek and pop read the same head; push is blocked.
  assert(model.out_result_1 == Bits<8>{10});
  model.in_value = Bits<8>{30};
  edge(model); // Push again once there is committed capacity.
  assert(model.out_result_1 == Bits<8>{10});

  model.in_issue = Bits<1>{0};
  edge(model);
  assert(model.out_result_1 == Bits<8>{10});
  edge(model);
  assert(model.out_result_1 == Bits<8>{10});
  edge(model);
  assert(model.out_result_0 == Bits<8>{30});
  assert(model.out_result_1 == Bits<8>{30});
}
#elif defined(CONFLICTING_POPS)
int main() {
  Model model;
  model.in_healthy = Bits<1>{1};
  edge(model, true);
  model.in_issue = Bits<1>{1};
  edge(model);
  assert(model.out_result_2 == Bits<8>{1});
  assert(model.out_result_3 == Bits<8>{0});

  model.in_pyc_clk = Bits<1>{0};
  model.in_issue = Bits<1>{0};
  step(model);
  model.in_pyc_clk = Bits<1>{1};
  model.in_take_first = Bits<1>{1};
  model.in_take_second = Bits<1>{1};
  assert(!model.Step());
  assert(!model.error.empty());
  assert(model.out_result_0 == Bits<8>{0});
  assert(model.out_result_1 == Bits<8>{0});
  assert(model.out_result_2 == Bits<8>{1});

  model.in_take_second = Bits<1>{0};
  model.in_overwrite = Bits<1>{1};
  assert(!model.Step());
  assert(!model.error.empty());
  assert(model.out_result_0 == Bits<8>{0});
  assert(model.out_result_2 == Bits<8>{1});

  model.in_overwrite = Bits<1>{0};
  model.in_healthy = Bits<1>{0};
  assert(!model.Step());
  assert(model.error.find("injected check failure") != std::string::npos);
  assert(model.out_result_0 == Bits<8>{0});
  assert(model.out_result_2 == Bits<8>{1});

  // Retry the failed rising sample. Its proposal and the clock did not commit.
  model.in_healthy = Bits<1>{1};
  step(model);
  assert(model.out_result_0 == Bits<8>{37});
  assert(model.out_result_1 == Bits<8>{0});
  assert(model.out_result_2 == Bits<8>{2});
  assert(model.out_result_3 == Bits<8>{1});
}
#endif
