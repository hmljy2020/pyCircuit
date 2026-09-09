#include "gfsim/core.h"
#include "model.cpp"
#include <iostream>
#include <stdexcept>
using namespace ac_generated;
using Model = Fields;
constexpr size_t N = std::tuple_size_v<decltype(std::declval<Model>().dispatch_rows())>;
#define CHECK(x) do { if (!(x)) throw std::runtime_error("line " + std::to_string(__LINE__) + ": " #x); } while (false)
class Clock final : public gfsim::SimObject {
  gfsim::SimSystem &system;
  bool scan;
public:
  Clock(gfsim::SimSystem &s, bool full)
      : SimObject(gfsim::ObjectKind::Process, "clock", N), system(s), scan(full) {}
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
  gfsim::SimSystem system{"fields"};
  Clock clock;
  std::array<gfsim::DispatchRow, N + 1> rows;
  std::vector<uint32_t> ao, co;
  const decltype(Model::activation_targets()) at = Model::activation_targets();
  const decltype(Model::work_closure_targets()) ct = Model::work_closure_targets();
  Lane(bool scan) : clock(system, scan) {
    auto base = model.dispatch_rows();
    std::copy(base.begin(), base.end(), rows.begin());
    for (auto &r : base) r.reset(r.object);
    rows.back() = gfsim::makeDispatchRow(&clock);
    CHECK(system.setDispatchTable(rows));
    system.setBuildProfile(gfsim::BuildProfile::Validated);
    if (scan) {
      for (unsigned i = 0; i < N; ++i) CHECK(system.scheduleWork(i, {0, 0}));
    } else {
      auto a = Model::activation_offsets(), c = Model::work_closure_offsets();
      ao.assign(a.begin(), a.end()); co.assign(c.begin(), c.end());
      ao.push_back(ao.back()); co.push_back(co.back());
      CHECK(system.setActivationPlan(ao, at));
      CHECK(system.setWorkClosurePlan(co, ct));
      CHECK(Model::schedule_initial_work(system));
    }
    CHECK(system.scheduleWork(N, {0, 0}));
  }
  const Entry &entry(unsigned owner, unsigned index = 0) {
    return static_cast<gfsim::SimTable<Entry> *>(
        static_cast<gfsim::SimObject *>(rows.at(owner).object))->at(index);
  }
  auto state() { return std::array<Entry, 3>{entry(3), entry(4), entry(4, 1)}; }
};
Entry value(unsigned x, bool valid) { return Entry{gfsim::UInt<8>{x}, gfsim::UInt<1>{valid}}; }
int main() {
  Lane scan(true), active(false);
  CHECK(N == 5);
  auto step = [&](unsigned count = 1) {
    for (unsigned i = 0; i < count; ++i) {
      CHECK(scan.system.step()); CHECK(active.system.step());
      CHECK(scan.system.currentEpoch() == active.system.currentEpoch());
      CHECK(scan.state() == active.state());
      CHECK(scan.model.command().committedValues() == active.model.command().committedValues());
      CHECK(scan.model.result_0().committedValues() == active.model.result_0().committedValues());
      CHECK(scan.system.commitTimeline() == active.system.commitTimeline());
      // A committed input must expose the complete state change.
      auto pops = scan.model.command().totalPops();
      if (pops == 1) CHECK(scan.entry(3) == value(7, true) && scan.entry(4) == value(7, true));
      if (pops == 2) CHECK(scan.entry(3) == value(255, false) && scan.entry(4, 1) == value(255, false));
      if (pops == 3) CHECK(scan.entry(3) == value(0, false) && scan.entry(4) == value(0, false));
    }
  };
  auto offer = [&](unsigned index, unsigned x, bool enabled) {
    for (auto *lane : {&scan, &active})
      CHECK(lane->model.offer_command(lane->system, Command{gfsim::UInt<1>{index}, gfsim::UInt<8>{x}, gfsim::UInt<1>{enabled}}));
  };
  auto take = [&](const Report &expected) {
    for (auto *lane : {&scan, &active}) {
      CHECK(lane->model.result_0().committedValues().front() == expected);
      CHECK(lane->model.try_take_result_0(lane->system).has_value());
    }
    step(20);
  };
  offer(0, 7, true); step(30);
  CHECK(scan.model.command().totalPops() == 1);
  const auto blocked = scan.state();
  offer(1, 255, false); step(30);
  CHECK(scan.state() == blocked && scan.model.command().totalPops() == 1);
  take(Report{value(7, true), value(0, false), value(0, false), value(7, true), value(0, false)});
  CHECK(scan.model.command().totalPops() == 2);
  take(Report{value(255, true), value(7, true), value(0, false), value(255, false), value(7, true)});
  offer(0, 0, false); step(30);
  take(Report{value(0, true), value(255, false), value(7, true), value(0, false), value(255, false)});
  CHECK(scan.model.command().totalPops() == 3);
  std::cout << "field assignment expected results and parity passed\n";
}
