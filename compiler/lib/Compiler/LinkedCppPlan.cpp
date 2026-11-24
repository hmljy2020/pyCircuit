#include "LinkedCppPlan.h"

#include "Dialect/ACIR/ACIRSourceContracts.h"
#include "mlir/IR/BuiltinOps.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/SetVector.h"
#include <set>

using namespace mlir;

namespace acir::compiler::linked {
namespace {

ac::YieldOp yield(Block &block) { return cast<ac::YieldOp>(block.back()); }

bool isWire(Operation *op) {
  return ac::detail::isCombinationalHardwareOperation(op);
}

// Mark uses of a capture through live evaluations. Merely appearing in the
// capture list does not read the net. For guard queries use precisely the same
// backward dependencies as the inferred guard proof.
struct Live {
  llvm::DenseSet<Operation *> &operations;
  llvm::DenseSet<Value> &arguments, &values;

  void value(Value val) {
    if (!values.insert(val).second)
      return;
    if (isa<BlockArgument>(val)) {
      arguments.insert(val);
      return;
    }
    if (auto branch = val.getDefiningOp<ac::SourceBranchOp>()) {
      operations.insert(branch);
      value(branch.getCondition());
      auto ordinal = cast<OpResult>(val).getResultNumber();
      value(yield(branch.getThenRegion().front()).getValues()[ordinal]);
      value(yield(branch.getElseRegion().front()).getValues()[ordinal]);
      return;
    }
    operation(val.getDefiningOp());
  }

  void operation(Operation *op) {
    if (!operations.insert(op).second)
      return;
    for (auto operand : op->getOperands())
      value(operand);
  }

  void effects(Block &block) {
    for (Operation &op : block.without_terminator()) {
      if (auto branch = dyn_cast<ac::SourceBranchOp>(op)) {
        auto before = operations.size();
        effects(branch.getThenRegion().front());
        effects(branch.getElseRegion().front());
        if (operations.size() != before) {
          operations.insert(branch);
          value(branch.getCondition());
        }
      } else if (!isWire(&op)) {
        operation(&op);
      }
    }
  }
};

std::optional<unsigned> domainPort(ac::ModuleOp mod, StringRef role) {
  auto domain = mod->getAttrOfType<DictionaryAttr>("ac.domain_inputs");
  if (auto port = domain ? domain.getAs<IntegerAttr>(role) : IntegerAttr())
    return port.getInt();
  return std::nullopt;
}

} // namespace

LogicalResult Plan::build() {
  auto visitor = [&](const ac::TransactionLexicalInstance &bound,
                     InstanceId id) -> LogicalResult {
    auto allocation = bound.allocation;
    auto definition = bound.definition;
    if (bound.parent && !allocation)
      return definition.emitOpError() << "t-gfsim does not support Collections";
    auto &instance = instances[id];
    instance.info = bound;
    instance.path = bound.parent ? instances.at(*bound.parent).path + "." +
                                       allocation.getInstanceName().str()
                                 : definition.getSymName().str();
    return success();
  };
  auto call = [&](ac::SourceMethodCallOp operation, InstanceId owner,
                  InstanceId receiver, const ac::SourceMethodView &method) {
    targets.try_emplace({owner, operation}, Target{receiver, method.definition,
                                                   method.nativeQueueInstance,
                                                   method.nativeQueueAction});
    return success();
  };
  ac::LinkedSourceBindingVisitor hooks{visitor, call};
  if (failed(ac::verifyLinkedSourceMethodBindings(package, &hooks)))
    return failure();
  for (auto &[id, instance] : instances)
    if (failed(connect(id, instance)))
      return failure();
  for (Net net : aliasesToValidate)
    if (!resolve(net).second)
      return emitError(net.second.getLoc(), "cyclic linked port forwarding");
  if (failed(checkDomains()))
    return failure();

  for (auto &[id, instance] : instances) {
    Block &block = instance.info.definition.getBody().front();
    // Quiescent wires and reset expressions use the same typed evaluation
    // as ordinary Work. Always include effect roots even with no SSA users.
    for (Operation &op : block.without_terminator()) {
      if (isa<ac::SourceMethodCallOp, ac::SourceExpectOp, ac::SourceObserveOp>(
              op))
        if (failed(activate({id, &op})))
          return failure();
    }
    for (Net port : llvm::concat<const Net>(instance.inputs, instance.outputs))
      if (failed(require(port, std::nullopt, instance.info.definition)))
        return failure();
  }
  for (const auto &resource : resources) {
    if (resource.kind != Resource::State)
      continue;
    if (failed(require(resource.initial, std::nullopt, module(resource.owner))))
      return failure();
    auto net = resolve(resource.initial);
    auto node = nodeIds.find({net.first, net.second.getDefiningOp()});
    if (node == nodeIds.end() || !nodes[node->second].fixed)
      return module(resource.owner)->emitOpError()
             << "t-gfsim requires a static reset value for " << resource.name;
  }
  scheduleWork();
  return success();
}

Net Plan::resolve(Net net) const {
  // Each step crosses one verified port binding. A cycle consists solely of
  // aliases and therefore has no value source.
  for (size_t count = 0; count <= aliases.size(); ++count) {
    auto next = aliases.find(net);
    if (next == aliases.end())
      return net;
    net = next->second;
  }
  return {};
}

void Plan::addSlot(Net net) {
  if (slotIds.try_emplace(net, slots.size()).second)
    slots.push_back(net);
}

LogicalResult Plan::connect(InstanceId id, Instance &instance) {
  auto mod = instance.info.definition;
  Block &block = mod.getBody().front();
  auto allocation = instance.info.allocation;
  for (auto [index, arg] : llvm::enumerate(block.getArguments())) {
    Net net{id, arg};
    instance.inputs.push_back(net);
    if (allocation) {
      aliases.try_emplace(
          net, Net{*instance.info.parent, allocation.getInputs()[index]});
      aliasesToValidate.push_back(net);
    } else {
      addSlot(net);
    }
  }
  for (auto [index, val] : llvm::enumerate(yield(block).getValues())) {
    Net net{id, val};
    instance.outputs.push_back(net);
    if (allocation) {
      Net alias{*instance.info.parent, allocation.getResult(index)};
      aliases.try_emplace(alias, net);
      aliasesToValidate.push_back(alias);
    }
  }
  for (Operation &op : block.without_terminator()) {
    if (auto child = dyn_cast<ac::InstanceOp>(op)) {
      if (ac::isQueueInstance(child)) {
        if (failed(addQueue(id, child)))
          return failure();
      } else if (!isa<ac::ModuleOp>(hardware.resolveCallee(child))) {
        return child.emitOpError()
               << "unsupported t-gfsim primitive " << child.getCallee();
      }
    } else if (auto state = dyn_cast<ac::SourceStateOp>(op)) {
      if (failed(addResource(id, state)))
        return failure();
    } else if (auto call = dyn_cast<ac::SourceMethodCallOp>(op)) {
      const auto &bound = target({id, call});
      if (!call->hasAttr("ac.registration_name") || !bound.method)
        return call.emitOpError() << "expected a simplified Rule registration";
      if ((!call.getPath() || ac::isSourceLiteralTrue(call.getPath())) &&
          total(bound))
        continue;
      for (auto result : call.getResults()) {
        storedNets.try_emplace({id, result}, resources.size());
        resources.push_back(
            {Resource::Held,
             id,
             result.getType(),
             instance.path + "." +
                 call->getAttrOfType<StringAttr>("ac.registration_name")
                     .getValue()
                     .str(),
             {}});
      }
    } else if (!isa<ac::SourceMethodOp, ac::SourceExpectOp, ac::SourceObserveOp,
                    ac::InstanceRefOp, ac::NumericProofOp>(op) &&
               !isWire(&op)) {
      return op.emitOpError() << "unsupported t-gfsim Module operation";
    }
  }
  auto fourState = mod.walk([](ac::BitsConstant4Op constant) {
    if (constant.getPattern().find_first_of("xz") == StringRef::npos)
      return WalkResult::advance();
    constant.emitOpError() << "X/Z values are unsupported in t-gfsim";
    return WalkResult::interrupt();
  });
  return failure(fourState.wasInterrupted());
}

LogicalResult Plan::addResource(InstanceId id, ac::SourceStateOp state) {
  auto index = resources.size();
  storedNets.try_emplace({id, state.getQ()}, index);
  states.try_emplace({id, state.getInstanceNameAttr()}, index);
  resources.push_back(
      {Resource::State,
       id,
       state.getQ().getType(),
       instances.at(id).path + "." + state.getInstanceName().str(),
       {id, state.getInitial()}});
  return success();
}

LogicalResult Plan::addQueue(InstanceId id, ac::InstanceOp queue) {
  auto owner =
      ac::lookupSourceQueue(module(id), queue.getInstanceName(), queue);
  if (failed(owner))
    return failure();
  if (!owner->isPushUnbound() || !owner->isPopUnbound())
    return queue.emitOpError()
           << "t-gfsim Queue " << instances.at(id).path << '.'
           << queue.getInstanceName() << " has explicit raw "
           << (!owner->isPushUnbound() ? "in_valid/in_data" : "out_ready")
           << " binding";
  auto config = hardware.resolveQueue(queue, bindings(id));
  if (failed(config))
    return failure();
  queues.try_emplace({id, queue}, resources.size());
  resources.push_back(
      {Resource::Queue,
       id,
       config->payloadType,
       instances.at(id).path + "." + queue.getInstanceName().str(),
       {},
       config->depth,
       config->availabilityLatency});
  return success();
}

LogicalResult Plan::checkDomains() {
  auto root = instances.at(0).info.definition;
  if (auto port = domainPort(root, "clock"))
    clock = {0, root.getBody().front().getArgument(*port)};
  if (auto port = domainPort(root, "reset"))
    reset = {0, root.getBody().front().getArgument(*port)};
  for (auto &[id, instance] : instances) {
    auto mod = instance.info.definition;
    for (auto [role, expected] :
         {std::pair{StringRef("clock"), clock}, {StringRef("reset"), reset}}) {
      if (auto port = domainPort(mod, role)) {
        if (resolve(instance.inputs[*port]) != expected)
          return mod.emitOpError() << "t-gfsim requires the root " << role
                                   << " at " << instance.path;
      }
    }
    for (Operation &op : mod.getBody().front()) {
      Value clk, rst;
      if (auto state = dyn_cast<ac::SourceStateOp>(op)) {
        clk = state.getClk();
        rst = state.getRst();
      } else if (auto queue = dyn_cast<ac::InstanceOp>(op);
                 queue && ac::isQueueInstance(queue)) {
        clk = queue.getInputs()[0];
        rst = queue.getInputs()[1];
      }
      if (clk && (resolve({id, clk}) != clock || resolve({id, rst}) != reset))
        return op.emitOpError() << "storage must use the root clock and reset";
    }
  }
  return success();
}

LogicalResult Plan::require(Net net, std::optional<unsigned> consumer,
                            Operation *use) {
  net = resolve(net);
  auto [owner, value] = net;
  if (storedNets.contains(net))
    return success();
  if (isa<BlockArgument>(value))
    return success();
  Operation *producer = value.getDefiningOp();
  if (auto queue = dyn_cast<ac::InstanceOp>(producer)) {
    auto index = cast<OpResult>(value).getResultNumber();
    auto callee = cast<ac::ModuleImportOp>(hardware.resolveCallee(queue));
    return use->emitOpError()
           << "t-gfsim Queue " << instances.at(owner).path << '.'
           << queue.getInstanceName() << " has a live raw output "
           << cast<StringAttr>(callee.getOutputNames()[index]).getValue();
  }
  Site site{owner, producer};
  if (failed(activate(site)))
    return failure();
  if (consumer)
    nodes[*consumer].inputs.push_back(nodeIds.find(site)->second);
  return success();
}

Method &Plan::prepareMethod(ac::SourceMethodOp method) {
  auto [entry, fresh] = methods.try_emplace(method);
  if (fresh) {
    auto &info = entry->second;
    Live guard{info.liveGuard, info.guardArguments, info.guardValues};
    for (auto val : yield(method.getGuard().front()).getValues())
      guard.value(val);
    Live body{info.liveBody, info.bodyArguments, info.bodyValues};
    for (auto val : yield(method.getBody().front()).getValues())
      body.value(val);
    body.effects(method.getBody().front());
    method.getBody().walk([&](ac::SourceMethodCallOp call) {
      info.calls.try_emplace(call.getOccurrence(), call);
    });
  }
  return entry->second;
}

bool Plan::total(const Target &bound) {
  auto method = bound.method;
  if (!method)
    return false;
  Site site{bound.receiver, method};
  if (auto found = totalGuards.find(site); found != totalGuards.end())
    return found->second;
  auto values = yield(method.getGuard().front()).getValues();
  bool result =
      values.empty() || alwaysTrue(values.front(), bound.receiver, method);
  totalGuards.try_emplace(site, result);
  return result;
}

bool Plan::alwaysTrue(Value predicate, InstanceId id,
                      ac::SourceMethodOp method) {
  if (ac::isSourceLiteralTrue(predicate))
    return true;
  if (auto query = predicate.getDefiningOp<ac::SourceMethodGuardQueryOp>()) {
    auto call = prepareMethod(method).calls.find(query.getOccurrence())->second;
    return total(target({id, call}));
  }
  if (auto binary = predicate.getDefiningOp<ac::BitsBinaryOp>();
      binary && binary.getOpcode() == "and")
    return alwaysTrue(binary.getLhs(), id, method) &&
           alwaysTrue(binary.getRhs(), id, method);
  return false;
}

LogicalResult Plan::callDependencies(Site site, unsigned consumer,
                                     llvm::DenseSet<Site> &visited) {
  auto found = targets.find(site);
  if (found == targets.end())
    return site.second->emitOpError() << "missing verified Method binding";
  const auto &bound = found->second;
  auto method = bound.method;
  if (!method)
    return bound.queue ? success()
                       : site.second->emitOpError()
                             << "unsupported t-gfsim Method terminal";
  if (!visited.insert({bound.receiver, method}).second)
    return success();

  if (method->hasAttr("ac.formal_proposals"))
    return method.emitOpError()
           << "writable Method formals are unsupported in t-gfsim";

  if (activatedMethods.insert({bound.receiver, method}).second)
    instances.at(bound.receiver).methods.push_back(method);
  const auto &info = prepareMethod(method);
  auto numFormals = method.getFunctionType().getNumInputs();
  SmallVector<Value> usedCaptures;
  for (auto [index, capture] : llvm::enumerate(method.getCaptures()))
    if (info.guardArguments.contains(
            method.getGuard().front().getArgument(numFormals + index)) ||
        info.bodyArguments.contains(
            method.getBody().front().getArgument(numFormals + index)))
      usedCaptures.push_back(capture);
  for (Value capture : usedCaptures)
    if (failed(require({bound.receiver, capture}, consumer, site.second)))
      return failure();
  // Use lexical order for deterministic collection and diagnostics.
  auto walk = method.getBody().walk([&](ac::SourceMethodCallOp nested) {
    return failed(callDependencies({bound.receiver, nested}, consumer, visited))
               ? WalkResult::interrupt()
               : WalkResult::advance();
  });
  return failure(walk.wasInterrupted());
}

LogicalResult Plan::activate(Site site) {
  auto [position, fresh] = nodeIds.try_emplace(site, nodes.size());
  auto [id, op] = site;
  auto index = position->second;
  if (!fresh) {
    if (!nodes[index].available)
      return op->emitOpError() << "cyclic same-sample value dependence";
    return success();
  }
  nodes.push_back({site, {}, isWire(op), false});
  for (Value operand : op->getOperands())
    if (failed(require({id, operand}, index, op)))
      return failure();
  if (isa<ac::SourceMethodCallOp>(op)) {
    llvm::DenseSet<Site> visited;
    if (failed(callDependencies(site, index, visited)))
      return failure();
  }
  auto &node = nodes[index];
  if (node.fixed) {
    for (auto value : op->getOperands()) {
      auto net = resolve({id, value});
      auto found = nodeIds.find({net.first, net.second.getDefiningOp()});
      if (found == nodeIds.end() || !nodes[found->second].fixed) {
        node.fixed = false;
        break;
      }
    }
  }
  node.available = true;
  if (node.fixed) {
    fixed.push_back(index);
  } else {
    auto &work = instances.at(id).work;
    // All producers are available, so completion order is topological.
    workOrder.emplace_back(id, work.size());
    work.push_back(index);
  }
  for (auto result : op->getResults()) {
    Net net{id, result};
    if (!storedNets.contains(net))
      addSlot(net);
  }
  return success();
}

void Plan::scheduleWork() {
  llvm::DenseMap<InstanceId, llvm::SetVector<InstanceId>> incoming;
  llvm::DenseMap<InstanceId, SmallVector<InstanceId>> outgoing;
  for (const auto &node : nodes) {
    auto consumer = node.site.first;
    for (auto index : node.inputs) {
      const auto &input = nodes[index];
      auto producer = input.site.first;
      if (!input.fixed && producer != consumer &&
          incoming[consumer].insert(producer))
        outgoing[producer].push_back(consumer);
    }
  }

  std::set<InstanceId> ready;
  for (auto &[id, _] : instances)
    if (incoming[id].empty())
      ready.insert(id);
  SmallVector<InstanceId> moduleOrder;
  while (!ready.empty()) {
    auto id = *ready.begin();
    ready.erase(ready.begin());
    moduleOrder.push_back(id);
    for (auto next : outgoing[id]) {
      incoming[next].remove(id);
      if (incoming[next].empty())
        ready.insert(next);
    }
  }
  if (moduleOrder.size() == instances.size()) {
    for (auto id : moduleOrder) {
      auto &instance = instances.at(id);
      schedule.push_back({id, instance.workSegments++, 0, instance.work.size()});
    }
    return;
  }

  // Group consecutive operations of each instance in the global order.
  // A registered Rule is one node, including its guard and Method calls.
  for (auto [id, offset] : workOrder) {
    if (schedule.empty() || schedule.back().instance != id)
      schedule.push_back({id, instances.at(id).workSegments++, offset, offset});
    schedule.back().end = offset + 1;
  }
}

} // namespace acir::compiler::linked
