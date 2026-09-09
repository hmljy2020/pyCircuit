// Expected-result driver for generated single or dual per-flow ROBs.
#include "gfsim/replay_session.h"
#include "model.cpp"
#include <fstream>
#include <iostream>
#include <stdexcept>
using namespace ac_generated;
using Model = DualCmtSystem;
constexpr unsigned Flows = 2;
constexpr unsigned Inputs = 6;
constexpr unsigned Queues = 14;
constexpr unsigned StateBase = Queues + 7;
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
    CHECK(e.time < 10000);
    if (scan)
      for (unsigned id = 0; id < N; ++id)
        CHECK(system.scheduleWork(id, {e.time + 1, 0}));
    CHECK(system.scheduleWork(N, {e.time + 1, 0}));
  }
};
struct Lane {
  Model model;
  gfsim::SimSystem system{"cmt"};
  Clock clock;
  std::array<gfsim::DispatchRow, N + 1> rows;
  std::unique_ptr<gfsim::ReplaySession> replay;
  std::vector<uint32_t> ao, co;
  const decltype(Model::activation_targets()) at = Model::activation_targets();
  const decltype(Model::work_closure_targets()) ct =
      Model::work_closure_targets();
  Lane(bool scan) : clock(system, scan) {
    auto base = model.dispatch_rows();
    std::copy(base.begin(), base.end(), rows.begin());
    rows.back() = gfsim::makeDispatchRow(&clock);
    // Reset is complete before observation registration and recording.
    for (auto &row : base)
      row.reset(row.object);
    CHECK(system.setDispatchTable(rows));
    system.setBuildProfile(gfsim::BuildProfile::Validated);
    if (scan) {
      for (unsigned id = 0; id < N; ++id)
        CHECK(system.scheduleWork(id, {0, 0}));
    } else {
      auto a = Model::activation_offsets();
      auto c = Model::work_closure_offsets();
      ao.assign(a.begin(), a.end());
      co.assign(c.begin(), c.end());
      ao.push_back(ao.back());
      co.push_back(co.back());
      CHECK(system.setActivationPlan(ao, at));
      CHECK(system.setWorkClosurePlan(co, ct));
      CHECK(Model::schedule_initial_work(system));
    }
    CHECK(system.scheduleWork(N, {0, 0}));
  }
  void record(const std::string &path) {
    replay =
        std::make_unique<gfsim::ReplaySession>(path, std::span(rows).first(N));
    model.registerObservations(*replay);
    replay->start();
    replay->attach(system);
  }
  template <class T> T &object(unsigned id) {
    return *static_cast<T *>(
        static_cast<gfsim::SimObject *>(rows.at(id).object));
  }
  template <class T> bool offer(unsigned id, const T &value) {
    auto &queue = object<gfsim::SimQueue<T>>(id);
    return queue.canProposePush() && system.scheduleExternalXfer(id) &&
           queue.proposePush(value);
  }
  template <unsigned W> uint64_t scalar(unsigned id) {
    return object<gfsim::SimTable<gfsim::UInt<W>>>(id).at(0).value();
  }
  bool busy(unsigned side = 0) { return scalar<1>(StateBase + 17 * side); }
  unsigned count(unsigned side = 0) {
    return scalar<4>(StateBase + 17 * side + 5);
  }
  const RobEvent &retained(unsigned side = 0) {
    return object<gfsim::SimTable<RobEvent>>(StateBase + 17 * side + 1).at(0);
  }
  template <class T> gfsim::ReplayValue queue(unsigned id) {
    auto &q = object<gfsim::SimQueue<T>>(id);
    return gfsim::ReplayValue::Array{gfsim::replayValue(q.committedValues()),
                                     gfsim::replayValue(q.delayedValues()),
                                     gfsim::replayValue(q.totalPushes()),
                                     gfsim::replayValue(q.totalPops())};
  }
  // Full registered Queue/Table projection, sampled independently of
  // ReplaySession.
  gfsim::ReplayValue projection() {
    gfsim::ReplayValue::Array result;
    for (unsigned side = 0; side < 2; ++side) {
      result.push_back(queue<RobEvent>(3 * side));
      result.push_back(queue<MpqHandoffAck>(3 * side + 1));
      result.push_back(queue<BrobHandoffAck>(3 * side + 2));
      result.push_back(queue<RobEvent>(6 + 4 * side));
      result.push_back(queue<RobEvent>(7 + 4 * side));
      result.push_back(queue<RobHandoff>(8 + 4 * side));
      result.push_back(queue<HandoffDiagnostic>(9 + 4 * side));
      auto b = StateBase + 17 * side;
      result.push_back(gfsim::replayValue(busy(side)));
      result.push_back(gfsim::replayValue(retained(side)));
      for (unsigned j = 2; j < 5; ++j)
        result.push_back(gfsim::replayValue(scalar<1>(b + j)));
      result.push_back(gfsim::replayValue(count(side)));
      for (unsigned j = 0; j < 16; ++j)
        result.push_back(gfsim::replayValue(
            object<gfsim::SimTable<ScalarRenameHistory>>(b + 6).at(j)));
      for (unsigned j = 0; j < 3; ++j) {
        result.push_back(gfsim::replayValue(
            object<gfsim::SimTable<HandoffDiagnostic>>(b + 7).at(j)));
        result.push_back(gfsim::replayValue(
            object<gfsim::SimTable<gfsim::UInt<1>>>(b + 8).at(j)));
        result.push_back(gfsim::replayValue(
            object<gfsim::SimTable<gfsim::UInt<16>>>(b + 9).at(j)));
      }
    }
    return result;
  }
};
// Exact typed encoding for the independent per-boundary parity artifact.
void writeProjection(std::ostream &out, const gfsim::ReplayValue &value) {
  out << value.data.index() << ':';
  std::visit(
      [&](const auto &v) {
        using T = std::decay_t<decltype(v)>;
        if constexpr (std::is_same_v<T, gfsim::ReplayValue::Array>) {
          out << v.size() << '[';
          for (const auto &e : v)
            writeProjection(out, e);
          out << ']';
        } else if constexpr (std::is_same_v<T, gfsim::ReplayValue::Object>) {
          out << v.size() << '{';
          for (const auto &[k, e] : v) {
            out << k.size() << ':' << k;
            writeProjection(out, e);
          }
          out << '}';
        } else if constexpr (std::is_same_v<T, gfsim::ReplayValue::Integer>)
          out << v.width << ',' << v.isSigned << ',' << v.bits;
        else if constexpr (std::is_same_v<T, gfsim::ReplayValue::FloatBits>)
          out << v.bits;
        else if constexpr (std::is_same_v<T, std::string>)
          out << v.size() << ':' << v;
        else if constexpr (std::is_same_v<T, bool>)
          out << v;
      },
      value.data);
  out << ';';
}
struct Pair {
  Lane scan{true}, active{false};
  std::ofstream projection_file{"projection.bin", std::ios::binary};
  Pair(bool record) {
    CHECK(scan.projection() == active.projection());
    CHECK(!scan.busy() && !scan.busy(1));
    if (record) {
      scan.record("execution.pyctrace");
      active.record("activation.pyctrace");
    }
  }
  void step(unsigned n = 1) {
    for (unsigned i = 0; i < n; ++i) {
      CHECK(scan.system.step());
      CHECK(active.system.step());
      CHECK(scan.system.currentEpoch() == active.system.currentEpoch());
      auto projection = scan.projection();
      CHECK(projection == active.projection());
      CHECK(scan.system.commitTimeline() == active.system.commitTimeline());
      writeProjection(projection_file, projection);
      projection_file << '\n';
    }
  }
  template <class T> void offer(unsigned id, const T &value) {
    for (auto *lane : {&scan, &active}) {
      auto &q = lane->object<gfsim::SimQueue<T>>(id);
      CHECK(q.canProposePush());
      CHECK(lane->system.scheduleExternalXfer(id));
      CHECK(q.proposePush(value));
    }
  }
  template <class T>
  const gfsim::SimQueue<T> &output(unsigned index, unsigned side = 0) {
    return scan.object<gfsim::SimQueue<T>>(Inputs + 4 * side + index);
  }
  template <class T> T take(unsigned index, unsigned side = 0) {
    for (unsigned i = 0; output<T>(index, side).isEmpty() && i < 32; ++i)
      step();
    CHECK(!output<T>(index, side).isEmpty());
    auto expected = output<T>(index, side).committedValues().front();
    for (auto *lane : {&scan, &active}) {
      unsigned id = Inputs + 4 * side + index;
      auto &q = lane->object<gfsim::SimQueue<T>>(id);
      CHECK(q.committedValues().front() == expected);
      CHECK(lane->system.scheduleExternalXfer(id));
      CHECK(q.proposePop().has_value());
    }
    step();
    return expected;
  }
  void quiet(unsigned side = 0) {
    CHECK(output<HandoffDiagnostic>(3, side).isEmpty());
    for (unsigned j = 0; j < 3; ++j) {
      CHECK(!scan.object<gfsim::SimTable<HandoffDiagnostic>>(StateBase +
                                                             17 * side + 7)
                 .at(j)
                 .valid);
      CHECK(!scan.object<gfsim::SimTable<gfsim::UInt<1>>>(StateBase +
                                                          17 * side + 8)
                 .at(j)
                 .value());
    }
  }
  void finish() {
    if (scan.replay) {
      scan.replay->finish();
      active.replay->finish();
    }
    CHECK(active.system.activationTraversalCount() > 0);
    CHECK(active.system.workInvocationCount() <
          scan.system.workInvocationCount());
    std::cout << "PASS scan_work=" << scan.system.workInvocationCount()
              << " activation_work=" << active.system.workInvocationCount()
              << '\n';
  }
};

RobEvent transaction(unsigned seq = 1, unsigned count = 2, unsigned mask = 3,
                     unsigned side = 0, unsigned epoch = 0) {
  RobEvent r{};
  FlowKey f{};
  f.stid = side;
  r.epoch.flow = f;
  r.epoch.recovery_epoch = epoch;
  r.inst.flow = f;
  r.inst.instruction_sequence = seq;
  r.inst.original_pc = 0xfedcba9876540000ULL + seq;
  r.block.flow = f;
  r.block.block_sequence = seq / 4;
  r.block.generation = 9;
  r.rob.flow = f;
  r.rob.slot = seq % 16;
  r.rob.generation = seq / 16 + 1;
  r.kind = RobEventKind::MICROCOMMIT;
  r.valid = 1;
  r.done = 1;
  r.handoff_pending = 1;
  r.handoff_required_mask = mask;
  r.mpq_history_record_count = count;
  r.result = 0xffffeeeeaaaabbbbULL;
  r.result_valid = 1;
  r.status = TerminalStatus::FAULT;
  r.fault_code = 0xdeadbeef;
  r.fault_arg0 = 0x1000000000000001ULL;
  r.fault_valid = 1;
  r.fault_bi = 1;
  return r;
}
MpqHandoffAck mpq(const RobEvent &r, unsigned seq) {
  MpqHandoffAck a{};
  a.request = r;
  a.valid = 1;
  a.accepted = 1;
  a.history.epoch = r.epoch;
  a.history.inst = r.inst;
  a.history.block = r.block;
  a.history.rob = r.rob;
  a.history.history_sequence = seq;
  a.history.valid = 1;
  a.history.durable = 1;
  return a;
}
BrobHandoffAck brob(const RobEvent &r) {
  BrobHandoffAck a{};
  a.epoch = r.epoch;
  a.inst = r.inst;
  a.block = r.block;
  a.rob = r.rob;
  a.valid = 1;
  a.durable = 1;
  return a;
}
void released(Pair &p, const RobEvent &r, unsigned side = 0) {
  RobHandoff expected{};
  expected.epoch = r.epoch;
  expected.inst = r.inst;
  expected.block = r.block;
  expected.rob = r.rob;
  expected.required_owner_mask = r.handoff_required_mask;
  expected.received_owner_mask = r.handoff_required_mask;
  expected.mpq_histories_required = r.mpq_history_record_count;
  expected.mpq_histories_acked = r.mpq_history_record_count;
  expected.valid = 1;
  expected.durable = 1;
  CHECK(p.take<RobHandoff>(2, side) == expected);
}
void start(Pair &p, const RobEvent &r, unsigned side = 0) {
  p.offer(3 * side, r);
  if (r.handoff_required_mask.value() & 1)
    CHECK(p.take<RobEvent>(0, side) == r);
  if (r.handoff_required_mask.value() & 2)
    CHECK(p.take<RobEvent>(1, side) == r);
}
void normal(Pair &p) {
  for (unsigned mask = 0; mask < 4; ++mask) {
    auto r = transaction(mask + 1, (mask & 1) ? 2 : 0, mask);
    start(p, r);
    if (mask & 2) {
      p.offer(2, brob(r));
      p.step(3);
    }
    if (mask & 1) {
      p.offer(1, mpq(r, 90));
      p.step(3);
      CHECK(p.scan.count() == 1);
      CHECK(p.output<RobHandoff>(2).isEmpty());
      p.offer(1, mpq(r, 17));
    }
    released(p, r);
    p.step(3);
    CHECK(!p.scan.busy());
    p.quiet();
    CHECK(p.output<RobEvent>(0).isEmpty() && p.output<RobEvent>(1).isEmpty() &&
          p.output<RobHandoff>(2).isEmpty());
  }
  auto zero = transaction(8, 0, 1);
  start(p, zero);
  released(p, zero);
  p.quiet();
}
void capacity(Pair &p) {
  for (unsigned turn = 0; turn < 2; ++turn) {
    auto r = transaction(20 + turn, 15, 3);
    start(p, r);
    for (unsigned i = 0; i < 15; ++i) {
      p.offer(1, mpq(r, 100 - i));
      p.step(3);
      CHECK(p.scan.count() == i + 1);
      CHECK(p.output<RobHandoff>(2).isEmpty());
    }
    p.offer(1, mpq(r, 999));
    CHECK(p.take<HandoffDiagnostic>(3).capacity_error);
    CHECK(p.scan.count() == 15);
    p.offer(1, mpq(r, 100));
    CHECK(p.take<HandoffDiagnostic>(3).duplicate);
    CHECK(p.scan.count() == 15);
    p.offer(2, brob(r));
    released(p, r);
    p.quiet();
  }
}
void invalid(Pair &p) {
  auto r = transaction();
  start(p, r);
  for (unsigned i = 0; i < 12; ++i) {
    auto a = mpq(r, 42);
    switch (i) {
    case 0:
      a.valid = 0;
      break;
    case 1:
      a.accepted = 0;
      break;
    case 2:
      a.stale = 1;
      break;
    case 3:
      a.history.valid = 0;
      break;
    case 4:
      a.history.durable = 0;
      break;
    case 5:
      a.history.rob.generation = 8;
      break;
    case 6:
      a.history.inst.original_pc = 0;
      break;
    case 7:
      a.history.block.generation = 1;
      break;
    case 8:
      a.history.epoch.recovery_epoch = 1;
      break;
    case 9:
      a.request.result = 0;
      break;
    case 10:
      a.request.kind = RobEventKind::ALLOCATED;
      break;
    case 11:
      a.history.rob.flow.stid = 1;
      break;
    }
    p.offer(1, a);
    auto d = p.take<HandoffDiagnostic>(3);
    CHECK(d.valid);
    if (i == 4)
      CHECK(d.durability_missing);
    else if (i >= 5 && i <= 8 || i == 11)
      CHECK(d.identity_mismatch);
    else
      CHECK(d.invalid_response);
    CHECK(p.scan.count() == 0 && p.scan.busy());
    CHECK(p.output<RobHandoff>(2).isEmpty());
  }
  p.offer(1, mpq(r, 42));
  p.step(3);
  p.offer(1, mpq(r, 42));
  CHECK(p.take<HandoffDiagnostic>(3).duplicate);
  CHECK(p.scan.count() == 1);
  for (unsigned i = 0; i < 5; ++i) {
    auto a = brob(r);
    if (i == 0)
      a.valid = 0;
    if (i == 1)
      a.durable = 0;
    if (i == 2)
      a.rob.generation = 9;
    if (i == 3)
      a.epoch.flow.stid = 1;
    if (i == 4)
      a.inst.original_pc = 0;
    p.offer(2, a);
    auto d = p.take<HandoffDiagnostic>(3);
    CHECK(d.valid);
    CHECK(p.output<RobHandoff>(2).isEmpty());
  }
  p.offer(2, brob(r));
  p.step(3);
  p.offer(2, brob(r));
  CHECK(p.take<HandoffDiagnostic>(3).duplicate);
  p.offer(1, mpq(r, 43));
  released(p, r);
  p.offer(2, brob(r));
  CHECK(p.take<HandoffDiagnostic>(3).unsolicited);
  auto bad = transaction(3);
  bad.kind = RobEventKind::ALLOCATE;
  p.offer(0, bad);
  CHECK(p.take<HandoffDiagnostic>(3).kind_mismatch);
  bad = transaction(4, 2, 2);
  p.offer(0, bad);
  CHECK(p.take<HandoffDiagnostic>(3).capacity_error);
  bad = transaction(5, 0, 0, 1);
  p.offer(0, bad);
  CHECK(p.take<HandoffDiagnostic>(3).identity_mismatch);
  CHECK(!p.scan.busy());
}
void backpressure(Pair &p) {
  // Host inserts blockers only to emulate unavailable downstream storage.
  auto blocker = transaction(99, 0, 0);
  p.offer(6, blocker);
  p.step();
  auto r = transaction();
  p.offer(0, r);
  CHECK(p.take<RobEvent>(1) == r);
  p.offer(2, brob(r));
  p.step(5);
  CHECK(p.scan.busy() && p.scan.count() == 0);
  p.offer(1, mpq(r, 1));
  CHECK(p.take<HandoffDiagnostic>(3).unsolicited);
  CHECK(p.take<RobEvent>(0) == blocker);
  CHECK(p.take<RobEvent>(0) == r);
  p.offer(1, mpq(r, 1));
  p.step(3);
  p.offer(1, mpq(r, 2));
  // Keep release output full: pending state and next input must be retained.
  RobHandoff block{};
  p.offer(8, block);
  p.step(5);
  auto next = transaction(2, 0, 0);
  p.offer(0, next);
  p.step(6);
  CHECK(p.scan.busy() && p.scan.retained() == r);
  CHECK(!p.scan.object<gfsim::SimQueue<RobEvent>>(0).isEmpty());
  CHECK(p.take<RobHandoff>(2) == block);
  released(p, r);
  released(p, next);
  // Mirror the independent request test with BROB blocked.
  p.offer(7, blocker);
  p.step();
  auto t = transaction(3, 1, 3);
  p.offer(0, t);
  CHECK(p.take<RobEvent>(0) == t);
  p.offer(1, mpq(t, 1));
  p.step(5);
  CHECK(p.scan.count() == 1 && p.scan.busy());
  CHECK(p.output<RobHandoff>(2).isEmpty());
  CHECK(p.take<RobEvent>(1) == blocker);
  CHECK(p.take<RobEvent>(1) == t);
  p.offer(2, brob(t));
  released(p, t);
}
void diagnostics(Pair &p) {
  auto r = transaction();
  start(p, r);
  auto a = mpq(r, 1);
  a.history.durable = 0;
  for (unsigned i = 0; i < 8; ++i) {
    p.offer(1, a);
    p.step(4);
  }
  CHECK(p.scan.object<gfsim::SimTable<gfsim::UInt<1>>>(StateBase + 8)
            .at(1)
            .value());
  CHECK(p.scan.object<gfsim::SimTable<gfsim::UInt<16>>>(StateBase + 9)
            .at(1)
            .value() == 6);
  CHECK(p.scan.count() == 0);
  p.offer(1, mpq(r, 1));
  p.step(3);
  p.offer(1, mpq(r, 2));
  p.offer(2, brob(r));
  released(p, r);
  auto first = p.take<HandoffDiagnostic>(3);
  auto rest = p.take<HandoffDiagnostic>(3);
  CHECK(first.coalesced_count == 1 && rest.coalesced_count == 7);
  CHECK(first.durability_missing && rest.durability_missing);
}
void recovery(Pair &p) {
  auto old = transaction(1, 1, 3, 0, 7);
  start(p, old);
  // External recovery advances producers only: CMT retains epoch 7.
  auto future = transaction(2, 0, 0, 0, 8);
  p.offer(0, future);
  p.step(4);
  CHECK(p.scan.retained() == old);
  auto wrong = brob(old);
  wrong.epoch.recovery_epoch = 8;
  p.offer(2, wrong);
  CHECK(p.take<HandoffDiagnostic>(3).identity_mismatch);
  p.offer(1, mpq(old, 1));
  p.offer(2, brob(old));
  released(p, old);
  released(p, future);
}
void isolation(Pair &p) {
  auto l = transaction(1, 1, 3), r = transaction(1, 1, 3, 1);
  start(p, l);
  start(p, r, 1);
  p.offer(1, mpq(r, 1));
  CHECK(p.take<HandoffDiagnostic>(3).identity_mismatch);
  p.offer(4, mpq(r, 1));
  p.offer(5, brob(r));
  released(p, r, 1);
  CHECK(p.scan.busy() && p.scan.count() == 0);
  p.quiet(1);
  p.offer(1, mpq(l, 1));
  p.offer(2, brob(l));
  released(p, l);
}
void reset(Pair &p) {
  // Reset from populated state before registering any replay observer.
  for (bool scan : {true, false}) {
    Lane probe(scan);
    const auto initial = probe.projection();
    auto r = transaction();
    CHECK(probe.offer(0, r));
    for (unsigned i = 0; i < 8; ++i)
      CHECK(probe.system.step());
    CHECK(probe.busy());
    CHECK(probe.offer(1, mpq(r, 1)));
    for (unsigned i = 0; i < 4; ++i)
      CHECK(probe.system.step());
    CHECK(probe.count() == 1);
    for (auto &row : probe.model.dispatch_rows())
      row.reset(row.object);
    CHECK(probe.projection() == initial);
  }
  auto fresh = transaction(8, 0, 0);
  start(p, fresh);
  released(p, fresh);
  p.quiet();
}
int main(int argc, char **argv) {
  try {
    CHECK(argc == 2);
    Pair p(std::getenv("PYC_RECORD_REPLAY") != nullptr);
    std::string s = argv[1];
    if (s == "normal")
      normal(p);
    else if (s == "capacity")
      capacity(p);
    else if (s == "invalid")
      invalid(p);
    else if (s == "backpressure")
      backpressure(p);
    else if (s == "diagnostics")
      diagnostics(p);
    else if (s == "recovery")
      recovery(p);
    else if (s == "isolation")
      isolation(p);
    else if (s == "reset")
      reset(p);
    else
      CHECK(false);
    p.finish();
    return 0;
  } catch (const std::exception &e) {
    std::cerr << e.what() << '\n';
    return 1;
  }
}
