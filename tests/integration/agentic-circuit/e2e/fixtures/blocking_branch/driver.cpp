#include "gfsim/core.h"
#include "model.cpp"
#include <array>
#include <iostream>
#include <stdexcept>
#include <tuple>
#include <vector>
using Model = ac_generated::BlockingBranch;
using Command = ac_generated::Command;
constexpr size_t N =
    std::tuple_size_v<decltype(std::declval<Model>().dispatch_rows())>;
#define CHECK(x)                                                               \
  do {                                                                         \
    if (!(x))                                                                  \
      throw std::runtime_error("line " + std::to_string(__LINE__) + ": " #x);  \
  } while (false)
class Clock final : public gfsim::SimObject {
  gfsim::SimSystem &system;
  bool scan;

public:
  Clock(gfsim::SimSystem &s, bool full)
      : SimObject(gfsim::ObjectKind::Process, "clock", N), system(s),
        scan(full) {}
  void doWork(gfsim::Epoch e) override {
    CHECK(e.time < 1000);
    if (scan)
      for (unsigned i = 0; i < N; ++i)
        CHECK(system.scheduleWork(i, {e.time + 1, 0}));
    CHECK(system.scheduleWork(N, {e.time + 1, 0}));
  }
};
struct Lane {
  Model model;
  gfsim::SimSystem system{"blocking-branch"};
  Clock clock;
  std::array<gfsim::DispatchRow, N + 1> rows;
  std::vector<uint32_t> ao, co;
  std::vector<gfsim::ObjectId> at, ct;
  Lane(bool scan) : clock(system, scan) {
    auto base = model.dispatch_rows();
    std::copy(base.begin(), base.end(), rows.begin());
    for (auto &row : base)
      row.reset(row.object);
    rows.back() = gfsim::makeDispatchRow(&clock);
    CHECK(system.setDispatchTable(rows));
    system.setBuildProfile(gfsim::BuildProfile::Validated);
    if (scan) {
      for (unsigned i = 0; i < N; ++i)
        CHECK(system.scheduleWork(i, {0, 0}));
    } else {
      // Flat fixture: input 0 and four state owners wake firing 1.
      // Its work closure includes the input and all state owners. CMT also
      // verifies the generated structured-module activation metadata.
      CHECK(N == 6);
      for (unsigned i = 0; i <= N; ++i) {
        ao.push_back(at.size());
        co.push_back(ct.size());
        if (i < N && i != 1)
          at.push_back(1);
        if (i == 1)
          for (unsigned j = 0; j < N; ++j)
            if (j != 1)
              ct.push_back(j);
      }
      ao.push_back(at.size());
      co.push_back(ct.size());
      CHECK(system.setActivationPlan(ao, at));
      CHECK(system.setWorkClosurePlan(co, ct));
      CHECK(system.scheduleWork(1, {0, 0}));
    }
    CHECK(system.scheduleWork(N, {0, 0}));
  }
  auto state() {
    return std::array<uint64_t, 7>{model.table_busy().at(0).value(),
                                   model.table_retained().at(0).value(),
                                   model.table_diagnostics().at(0).value(),
                                   model.table_entries().at(0).value(),
                                   model.table_entries().at(1).value(),
                                   model.command().totalPushes(),
                                   model.command().totalPops()};
  }
  void offer(bool bad, unsigned value) {
    CHECK(system.scheduleExternalXfer(0));
    CHECK(model.command().proposePush(
        Command{gfsim::UInt<1>{bad}, gfsim::UInt<8>{value}}));
  }
};
int main() {
  for (unsigned value : {0u, 7u}) {
    Lane scan(true), active(false);
    auto step = [&]() {
      CHECK(scan.system.step());
      CHECK(active.system.step());
      CHECK(scan.system.currentEpoch() == active.system.currentEpoch());
      CHECK(scan.state() == active.state());
      CHECK(scan.model.command().committedValues() ==
            active.model.command().committedValues());
      CHECK(scan.model.command().delayedValues() ==
            active.model.command().delayedValues());
      CHECK(scan.system.commitTimeline() == active.system.commitTimeline());
      const auto s = scan.state();
      // Every observed boundary is either before or after the whole business
      // commit.
      if (s[0])
        CHECK(s[1] == value && s[3] == value &&
              s[4] == (value ? value + 1 : 0));
    };
    auto offer = [&](bool bad, unsigned v) {
      scan.offer(bad, v);
      active.offer(bad, v);
    };
    auto settle = [&]() {
      for (unsigned i = 0; i < 8; ++i)
        step();
    };
    offer(true, 99);
    settle(); // Invalid while idle: diagnostics only.
    CHECK((scan.state() == std::array<uint64_t, 7>{0, 0, 1, 0, 0, 1, 1}));
    offer(false, value);
    settle(); // Valid while idle: exactly one atomic commit.
    CHECK(
        (scan.state() == std::array<uint64_t, 7>{1, value, 1, value,
                                                 value ? value + 1 : 0, 2, 2}));
    offer(true, 88);
    settle(); // Invalid while busy: preserve retained transaction.
    CHECK(
        (scan.state() == std::array<uint64_t, 7>{1, value, 2, value,
                                                 value ? value + 1 : 0, 3, 3}));
    offer(false, 42);
    settle(); // Valid while busy: input remains queued indefinitely.
    CHECK(
        (scan.state() == std::array<uint64_t, 7>{1, value, 2, value,
                                                 value ? value + 1 : 0, 4, 3}));
    CHECK(scan.model.command().committedValues().front().value.value() == 42);
    settle();
    CHECK(scan.model.command().totalPops() == 3);
  }
  std::cout << "blocking branches: expected results and scan/activation parity "
               "passed\n";
}
