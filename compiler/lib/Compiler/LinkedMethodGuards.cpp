#include "Dialect/ACIR/ACIRSourceContracts.h"
#include "LinkedSourceACIR.h"
#include "mlir/IR/Builders.h"
#include "mlir/IR/IRMapping.h"
#include "mlir/IR/OperationSupport.h"
#include "mlir/Pass/Pass.h"
#include "mlir/Transforms/RegionUtils.h"
#include "pycircuit/Dialect/ACIR/SourceMethodAnalysis.h"
#include "pycircuit/Dialect/ACIR/SourceTransactionStateAnalysis.h"
#include "llvm/ADT/APSInt.h"
#include "llvm/ADT/DenseSet.h"
#include "llvm/ADT/SetVector.h"

using namespace mlir;

namespace acir::compiler {
namespace {

constexpr StringLiteral inferredGuards = "ac.linked_inferred_guards";

using ac::LexicalInstanceId;
using ac::SourceBranchOp;
using ac::SourceMethodCallOp;
using ac::SourceMethodGuardQueryOp;
using ac::SourceMethodOp;
using ac::SourceStateReadOp;
using ac::YieldOp;

struct CallTarget {
  SourceMethodOp method;
  bool queue;
  LexicalInstanceId receiver;
};
using BoundCall = std::pair<LexicalInstanceId, Operation *>;

struct GuardPlan {
  Value explicitPredicate;
  SmallVector<unsigned> usedFormals;
  SmallVector<SourceStateReadOp> explicitReads;
  SmallVector<SourceMethodCallOp> calls;
};

struct GuardArguments {
  SmallVector<Value> actuals;
  SmallVector<Value> slice;
};

ac::BitsType predicateType(SourceMethodOp method) {
  Builder builder(method.getContext());
  auto one =
      ac::MathIntAttr::get(method.getContext(), llvm::APSInt::getUnsigned(1));
  auto width = builder.getDictionaryAttr({
      builder.getNamedAttr("kind", builder.getStringAttr("literal")),
      builder.getNamedAttr(
          "value",
          builder.getDictionaryAttr(
              {builder.getNamedAttr("kind", builder.getStringAttr("integer")),
               builder.getNamedAttr("value", one)})),
      builder.getNamedAttr("origin", method.getOccurrence()),
      builder.getNamedAttr("location", method->getAttr("ac.guard_location")),
  });
  return ac::BitsType::get(method.getContext(),
                           ac::StaticExprAttr::get(method.getContext(), width));
}

Value branchYield(Region &arm, unsigned ordinal) {
  return cast<YieldOp>(arm.front().back()).getValues()[ordinal];
}

void addDependencies(Value value, SmallVectorImpl<Value> &pending) {
  Operation *op = value.getDefiningOp();
  if (!op)
    return;
  if (auto branch = dyn_cast<SourceBranchOp>(op)) {
    auto index = cast<OpResult>(value).getResultNumber();
    pending.push_back(branch.getCondition());
    pending.push_back(branchYield(branch.getThenRegion(), index));
    pending.push_back(branchYield(branch.getElseRegion(), index));
    return;
  }
  llvm::append_range(pending, op->getOperands());
  if (op->getNumRegions()) {
    llvm::SetVector<Value> captures;
    getUsedValuesDefinedAbove(op->getRegions(), captures);
    llvm::append_range(pending, captures);
  }
}

// The postorder includes only values needed by the returned predicate or
// queried actuals. In particular a branch result uses only its own arm yields.
SmallVector<Value> backwardSlice(ValueRange roots) {
  llvm::DenseSet<Value> seen;
  SmallVector<Value> ordered;
  SmallVector<Value> pending(roots);
  SmallVector<std::pair<Value, size_t>> active;
  while (!pending.empty()) {
    Value value = pending.pop_back_val();
    if (!active.empty() && active.back() == std::pair{value, pending.size()}) {
      ordered.push_back(value);
      active.pop_back();
      continue;
    }
    if (!seen.insert(value).second)
      continue;
    active.emplace_back(value, pending.size());
    pending.push_back(value);
    addDependencies(value, pending);
  }
  return ordered;
}

struct StateAccess {
  LexicalInstanceId instance;
  StringAttr state;
  ArrayAttr selectors;
  bool operator==(const StateAccess &) const = default;
};

bool overlaps(const StateAccess &left, const StateAccess &right) {
  if (left.instance != right.instance || left.state != right.state)
    return false;
  for (auto [lhs, rhs] : llvm::zip(left.selectors, right.selectors)) {
    auto a = cast<DictionaryAttr>(lhs).getAs<StringAttr>("field");
    auto b = cast<DictionaryAttr>(rhs).getAs<StringAttr>("field");
    if (a && b && a != b)
      return false;
  }
  return true;
}

using Writes = SmallVector<StateAccess>;

class LinkedMethodGuards {
public:
  LinkedMethodGuards(ModuleOp package, bool infer)
      : package(package), infer(infer) {}

  LogicalResult run() {
    auto visitModule = [](ac::ModuleOp module, LexicalInstanceId) {
      for (auto transaction :
           module.getBody().front().getOps<ac::SourceTransactionOp>()) {
        auto walk = transaction.walk([](SourceMethodCallOp call) {
          call.emitOpError()
              << "linked guard inference requires simplified Rule "
                 "registrations and does not support Method calls in "
                 "a general ac.transaction";
          return WalkResult::interrupt();
        });
        if (walk.wasInterrupted())
          return failure();
      }
      return success();
    };
    auto visitCall = [&](SourceMethodCallOp call, LexicalInstanceId owner,
                         LexicalInstanceId receiver,
                         const ac::SourceMethodView &method) {
      BoundCall key{owner, call};
      targets.try_emplace(key,
                          CallTarget{method.definition,
                                     method.isNativeQueueTerminal(), receiver});
      lexicalTargets.try_emplace(call, key);
      if (isa<ac::ModuleOp>(call->getParentOp()))
        registrations.push_back(key);
      return success();
    };
    ac::LinkedSourceBindingVisitor visitor{visitModule, visitCall};
    if (failed(ac::verifyLinkedSourceMethodBindings(package, &visitor)))
      return failure();

    for (const auto &key : registrations) {
      auto call = cast<SourceMethodCallOp>(key.second);
      if (failed(prepare(targets.find(key)->second.method, call, 0)))
        return failure();
    }
    auto queries = package.walk([&](SourceMethodGuardQueryOp query) {
      if (plans.contains(query->getParentOp()))
        return WalkResult::advance();
      query.emitOpError() << "guard query requires a reachable Method";
      return WalkResult::interrupt();
    });
    if (queries.wasInterrupted())
      return failure();

    // Validate reads under actual receiver identities and original body order.
    // Every registration starts from committed state, independently of others.
    for (const auto &key : registrations) {
      auto call = cast<SourceMethodCallOp>(key.second);
      Writes written;
      if (failed(checkInvocation(targets.find(key)->second, call,
                                 FormalOwners(), written)))
        return failure();
    }
    return success();
  }

private:
  Value buildGuard(SourceMethodOp method, const GuardPlan &plan,
                   OpBuilder &builder) {
    Type type = predicateType(method);
    IRMapping mapping;
    for (auto [body, guard] :
         llvm::zip_equal(method.getBody().front().getArguments(),
                         method.getGuard().front().getArguments()))
      mapping.map(body, guard);

    Value predicate = plan.explicitPredicate;
    for (auto call : plan.calls) {
      const auto &argumentPlan = arguments.find(call)->second;
      for (Value value : argumentPlan.slice) {
        if (mapping.contains(value))
          continue;
        Operation *op = value.getDefiningOp();
        if (auto branch = dyn_cast<SourceBranchOp>(op)) {
          auto index = cast<OpResult>(value).getResultNumber();
          auto condition = mapping.lookup(branch.getCondition());
          auto whenTrue =
              mapping.lookup(branchYield(branch.getThenRegion(), index));
          auto whenFalse =
              mapping.lookup(branchYield(branch.getElseRegion(), index));
          auto select = ac::ValueMergeOp::create(
              builder, branch.getLoc(), value.getType(), condition.getType(),
              whenFalse, ValueRange{condition}, ValueRange{whenTrue},
              builder.getArrayAttr({builder.getArrayAttr({})}));
          mapping.map(value, select.getNext());
        } else {
          builder.clone(*op, mapping);
        }
      }
      SmallVector<Value> actuals;
      for (Value actual : argumentPlan.actuals)
        actuals.push_back(mapping.lookup(actual));
      Value query = SourceMethodGuardQueryOp::create(
          builder, call.getLoc(), type, actuals, call.getOccurrence());
      if (predicate)
        predicate = ac::BitsBinaryOp::create(builder, call.getLoc(), type,
                                             predicate, query, "and");
      else
        predicate = query;
    }
    return predicate;
  }

  LogicalResult verifyGuard(SourceMethodOp method, GuardPlan &plan,
                            YieldOp yield) {
    Value value =
        yield.getValues().empty() ? Value() : yield.getValues().front();
    plan.explicitPredicate = value;
    if (plan.calls.empty()) {
      for (auto query :
           method.getGuard().front().getOps<SourceMethodGuardQueryOp>())
        return query.emitOpError() << "no matching Method body call";
      return success();
    }
    auto error = [&] {
      return yield.emitOpError() << "invalid inferred Method guard: ";
    };
    auto matches = [](Value result, SourceMethodCallOp call) {
      auto query = result ? result.getDefiningOp<SourceMethodGuardQueryOp>()
                          : SourceMethodGuardQueryOp();
      return query && query.getOccurrence() == call.getOccurrence();
    };
    for (auto call : llvm::reverse(plan.calls)) {
      auto andOp =
          value ? value.getDefiningOp<ac::BitsBinaryOp>() : ac::BitsBinaryOp();
      if (andOp && andOp.getOpcode() == "and" &&
          matches(andOp.getRhs(), call)) {
        value = andOp.getLhs();
      } else if (call == plan.calls.front() && matches(value, call) &&
                 ac::detail::allowsImplicitSourceConditions(method)) {
        value = {};
      } else {
        return error() << "yield must conjoin each original call occurrence";
      }
    }
    plan.explicitPredicate = value;

    Block expected;
    OpBuilder builder(package.getContext());
    builder.setInsertionPointToStart(&expected);
    Value predicate = buildGuard(method, plan, builder);
    auto actual = method.getGuard().front().without_terminator();
    auto count = std::distance(expected.begin(), expected.end());
    if (std::distance(actual.begin(), actual.end()) < count)
      return error() << "missing query operand computations";
    auto generated = std::prev(actual.end(), count);
    for (auto it = actual.begin(); it != generated; ++it)
      if (isa<SourceMethodGuardQueryOp>(*it))
        return it->emitOpError() << "unexpected guard query";
    IRMapping mapping;
    auto equivalent = [&](Value from, Value to) {
      return success(mapping.lookupOrDefault(from) == to);
    };
    for (auto [from, to] :
         llvm::zip(expected, llvm::make_range(generated, actual.end())))
      if (!OperationEquivalence::isEquivalentTo(
              &from, &to, equivalent,
              [&](Value a, Value b) { mapping.map(a, b); },
              OperationEquivalence::IgnoreLocations))
        return to.emitOpError() << "inferred Method guard differs from its "
                                   "original call and argument bindings";
    if (failed(equivalent(predicate, yield.getValues().front())))
      return error() << "yield does not use the verified conjunction";
    return success();
  }

  LogicalResult prepare(SourceMethodOp method, SourceMethodCallOp site,
                        unsigned depth) {
    if (!method)
      return site.emitOpError() << "guard inference requires an ordinary "
                                   "Method body or native Queue";
    if (plans.contains(method))
      return success();
    if (!active.insert(method).second)
      return site.emitOpError()
             << "recursive Method call in linked guard inference";
    if (depth >= 128)
      return site.emitOpError() << "linked Method guard depth exceeded";

    GuardPlan plan;
    llvm::DenseSet<Attribute> occurrences;
    auto calls = method.getBody().walk<WalkOrder::PreOrder>(
        [&](SourceMethodCallOp call) {
          if (!occurrences.insert(call.getOccurrence()).second) {
            call.emitOpError() << "Method body call occurrence must be unique "
                                  "for guard inference";
            return WalkResult::interrupt();
          }
          plan.calls.push_back(call);
          return WalkResult::advance();
        });
    if (calls.wasInterrupted())
      return failure();
    for (auto call : plan.calls) {
      auto target = targets.find(lexicalTargets.find(call)->second)->second;
      GuardArguments argumentPlan;
      if (!target.queue) {
        if (failed(prepare(target.method, call, depth + 1)))
          return failure();
        for (unsigned ordinal : plans.find(target.method)->second.usedFormals)
          argumentPlan.actuals.push_back(call.getArguments()[ordinal]);
        argumentPlan.slice = backwardSlice(argumentPlan.actuals);
        for (Value value : argumentPlan.slice) {
          if (isa<BlockArgument>(value))
            continue;
          Operation *op = value.getDefiningOp();
          if (isa<SourceMethodCallOp>(op))
            return call.emitOpError()
                   << "guard argument depends on a prior Method result";
          if (!isa<SourceStateReadOp, SourceBranchOp>(op) &&
              !ac::detail::isCombinationalHardwareOperation(op))
            return call.emitOpError()
                   << "guard argument cannot be recomputed at admission";
        }
      }
      arguments.try_emplace(call, std::move(argumentPlan));
    }

    auto yield = cast<YieldOp>(method.getGuard().front().back());
    if (infer) {
      if (!yield.getValues().empty())
        plan.explicitPredicate = yield.getValues().front();
      OpBuilder builder(yield);
      if (Value predicate = buildGuard(method, plan, builder))
        yield->setOperands(predicate);
    } else if (failed(verifyGuard(method, plan, yield))) {
      return failure();
    }

    if (plan.explicitPredicate)
      for (Value value : backwardSlice(ValueRange{plan.explicitPredicate}))
        if (auto read = value.getDefiningOp<SourceStateReadOp>())
          plan.explicitReads.push_back(read);
    llvm::SmallDenseSet<unsigned> used;
    for (Value value : backwardSlice(yield.getValues()))
      if (auto arg = dyn_cast<BlockArgument>(value);
          arg && arg.getArgNumber() < method.getFunctionType().getNumInputs())
        used.insert(arg.getArgNumber());
    plan.usedFormals.assign(used.begin(), used.end());
    llvm::sort(plan.usedFormals);
    plans.try_emplace(method, std::move(plan));
    active.erase(method);
    return success();
  }

  StateAccess stateRead(SourceStateReadOp read, LexicalInstanceId anchor) {
    return {anchor, read.getStateAttr(), read.getSelectors()};
  }

  bool wasWritten(const StateAccess &read, const Writes &written) {
    return llvm::any_of(written, [&](const StateAccess &write) {
      return overlaps(read, write);
    });
  }

  void addWrite(Writes &writes, StateAccess write) {
    if (!llvm::is_contained(writes, write))
      writes.push_back(write);
  }

  using FormalOwners = llvm::DenseMap<unsigned, StateAccess>;

  LogicalResult checkInvocation(CallTarget target, SourceMethodCallOp site,
                                const FormalOwners &formals, Writes &written) {
    if (!target.method)
      return success(); // Queue queries use committed state.
    const auto &plan = plans.find(target.method)->second;
    for (auto read : plan.explicitReads)
      if (wasWritten(stateRead(read, target.receiver), written))
        return site.emitOpError()
               << "Method guard reads a prior transaction state candidate";
    llvm::DenseSet<Operation *> candidateReads;
    return checkBody(target.method.getBody().front(), target.receiver, formals,
                     candidateReads, written);
  }

  LogicalResult checkBody(Block &block, LexicalInstanceId anchor,
                          const FormalOwners &formals,
                          llvm::DenseSet<Operation *> &candidateReads,
                          Writes &written) {
    for (Operation &op : block.without_terminator()) {
      if (remainingWork == 0)
        return op.emitOpError()
               << "linked Method guard analysis budget exceeded";
      --remainingWork;
      if (auto read = dyn_cast<SourceStateReadOp>(op)) {
        if (wasWritten(stateRead(read, anchor), written))
          candidateReads.insert(read);
      } else if (auto write = dyn_cast<ac::SourceStateWriteOp>(op)) {
        addWrite(written, {anchor, write.getStateAttr(), write.getSelectors()});
      } else if (auto write = dyn_cast<ac::SourceFormalWriteOp>(op)) {
        auto owner = formals.find(write.getFormal());
        if (owner != formals.end()) {
          auto selected = owner->second;
          selected.selectors = write.getSelectors();
          addWrite(written, selected);
        }
      } else if (auto branch = dyn_cast<SourceBranchOp>(op)) {
        auto alternate = written;
        if (failed(checkBody(branch.getThenRegion().front(), anchor, formals,
                             candidateReads, written)) ||
            failed(checkBody(branch.getElseRegion().front(), anchor, formals,
                             candidateReads, alternate)))
          return failure();
        for (const auto &write : alternate)
          addWrite(written, write);
      } else if (auto call = dyn_cast<SourceMethodCallOp>(op)) {
        for (Value dep : arguments.find(call)->second.slice)
          if (candidateReads.contains(dep.getDefiningOp()))
            return call.emitOpError() << "guard argument reads a prior "
                                         "transaction state candidate";
        FormalOwners nested;
        if (auto bindings = call.getFormalOwnerBindings())
          for (Attribute raw : bindings) {
            auto row = cast<DictionaryAttr>(raw);
            auto index = row.getAs<IntegerAttr>("formal").getInt();
            if (auto state = row.getAs<StringAttr>("state"))
              nested.try_emplace(
                  index, StateAccess{anchor, state,
                                     ArrayAttr::get(op.getContext(), {})});
            else if (auto owner = formals.find(
                         row.getAs<IntegerAttr>("caller_formal").getInt());
                     owner != formals.end())
              nested.try_emplace(index, owner->second);
          }
        if (failed(checkInvocation(targets.find({anchor, call})->second, call,
                                   nested, written)))
          return failure();
      }
    }
    return success();
  }

  ModuleOp package;
  bool infer;
  uint64_t remainingWork = 1024 * 1024;
  llvm::DenseMap<BoundCall, CallTarget> targets;
  llvm::DenseMap<Operation *, BoundCall> lexicalTargets;
  SmallVector<BoundCall> registrations;
  llvm::DenseMap<Operation *, GuardPlan> plans;
  llvm::DenseMap<Operation *, GuardArguments> arguments;
  llvm::DenseSet<Operation *> active;
};

struct InferLinkedMethodGuardsPass
    : PassWrapper<InferLinkedMethodGuardsPass, OperationPass<ModuleOp>> {
  MLIR_DEFINE_EXPLICIT_INTERNAL_INLINE_TYPE_ID(InferLinkedMethodGuardsPass)

  StringRef getArgument() const final {
    return "ac-infer-linked-method-guards";
  }
  StringRef getDescription() const final {
    return "Infer Method admission guards for the selected linked-source "
           "hierarchy";
  }

  void runOnOperation() final {
    ModuleOp package = getOperation();
    if (failed(verifyLinkedSourceACIR(package)))
      return signalPassFailure();
    if (package->hasAttr(inferredGuards))
      return;

    OwningOpRef<ModuleOp> candidate(cast<ModuleOp>(package->clone()));
    auto marker = UnitAttr::get(package.getContext());
    (*candidate)->setAttr(inferredGuards, marker);
    if (failed(LinkedMethodGuards(*candidate, true).run()) ||
        failed(verifyLinkedSourceACIR(*candidate)))
      return signalPassFailure();
    package->setAttr(inferredGuards, marker);
    package.getBodyRegion().takeBody(candidate->getBodyRegion());
  }
};

} // namespace

LogicalResult verifyLinkedMethodGuards(ModuleOp package) {
  if (!package->hasAttrOfType<UnitAttr>(inferredGuards))
    return ac::verifyLinkedSourceMethodBindings(package);
  return LinkedMethodGuards(package, false).run();
}

void registerInferLinkedMethodGuardsPass() {
  PassRegistration<InferLinkedMethodGuardsPass>();
}

} // namespace acir::compiler
