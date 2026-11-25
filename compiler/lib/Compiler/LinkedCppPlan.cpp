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

void collectWrittenSlots(const Node &node, llvm::DenseSet<Net> &slots) {
  if (!node.copies.empty()) {
    for (const auto &copy : node.copies)
      slots.insert(copy.destination.net);
  } else if (node.result) {
    slots.insert({node.site.first, node.result});
  } else {
    for (Value result : node.site.second->getResults())
      slots.insert({node.site.first, result});
  }
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
    if (auto get = val.getDefiningOp<ac::TableGetOp>()) {
      operations.insert(get);
      value(get.getIndex());
      if (val == get.getValue())
        value(get.getInput());
      return;
    }
    operation(val.getDefiningOp());
  }

  void operation(Operation *op) {
    if (!operations.insert(op).second)
      return;
    if (auto choose = dyn_cast<ac::TableChooseOp>(op)) {
      value(choose.getMask());
      return;
    }
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
    auto &instance = instances[id];
    instance.info = bound;
    if (!bound.parent) {
      instance.path = definition.getSymName().str();
    } else {
      instance.path = instances.at(*bound.parent).path + ".";
      if (allocation) {
        instance.path += allocation.getInstanceName().str();
      } else {
        auto collection = instance.collection;
        instance.path += collection.getInstanceName().str() + "[" +
                         std::to_string(instance.element) + "]";
        collectionElements[{*bound.parent, collection}].push_back(id);
      }
    }
    return success();
  };
  auto call = [&](ac::SourceMethodCallOp operation, InstanceId owner,
                  InstanceId receiver, const ac::SourceMethodView &method) {
    targets.try_emplace({owner, operation}, Target{receiver, method.definition,
                                                   method.nativeQueueInstance,
                                                   method.nativeQueueAction});
    return success();
  };
  auto bindElement = [&](ac::CollectionOp collection, InstanceId id,
                         uint64_t ordinal) {
    auto &instance = instances[id];
    instance.collection = collection;
    instance.element = ordinal;
  };
  ac::LinkedSourceBindingVisitor hooks{visitor, call, bindElement};
  if (failed(ac::verifyLinkedSourceMethodBindings(package, &hooks)))
    return failure();
  for (auto &[id, instance] : instances)
    if (failed(connect(id, instance)))
      return failure();
  for (Net net : aliasesToValidate) {
    auto source = resolve({net, {}}, module(net.first));
    if (failed(source))
      return failure();
    resolvedAliases.try_emplace(net, *source);
  }
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
    if (!id)
      for (Net port : instance.outputs) {
        if (failed(require(port, std::nullopt, instance.info.definition)))
          return failure();
        persistentSlots.insert(projection(port).net);
      }
  }
  for (const auto &resource : resources) {
    if (resource.kind != Resource::State)
      continue;
    if (failed(require(resource.initial, std::nullopt, module(resource.owner))))
      return failure();
    auto initial = projection(resource.initial).net;
    auto result = resultNodes.find(initial);
    auto operation =
        nodeIds.find({initial.first, initial.second.getDefiningOp()});
    if (result != resultNodes.end()
            ? !nodes[result->second].fixed
            : operation == nodeIds.end() || !nodes[operation->second].fixed)
      return module(resource.owner)->emitOpError()
             << "t-gfsim requires a static reset value for " << resource.name;
  }
  planWholeStateWrites();
  scheduleWork();
  return success();
}

ProjectedNet Plan::projection(Net net) const {
  if (auto found = resolvedAliases.find(net); found != resolvedAliases.end())
    return found->second;
  return {net, {}};
}

FailureOr<unsigned> Plan::fieldOrdinal(Type type, StringAttr name,
                                       InstanceId owner,
                                       Operation *site) const {
  auto actual = hardware.resolveType(type, bindings(owner), site);
  if (failed(actual))
    return failure();
  auto fields =
      hardware.lookupStruct(cast<ac::StructType>(*actual)).getFields();
  for (auto [index, raw] : llvm::enumerate(fields))
    if (cast<DictionaryAttr>(raw).getAs<StringAttr>("name") == name)
      return unsigned(index);
  return site->emitOpError() << "missing verified Struct field " << name;
}

FailureOr<std::optional<uint64_t>> Plan::staticOrdinal(Value value,
                                                       ac::TableType table,
                                                       InstanceId owner,
                                                       Operation *site) const {
  std::optional<llvm::APInt> index;
  if (auto constant = value.getDefiningOp<ac::BitsConstantOp>()) {
    auto actual =
        hardware.evaluateStatic(constant.getValue(), bindings(owner), site);
    auto width =
        hardware.getPackedWidth(value.getType(), bindings(owner), site);
    if (failed(actual) || failed(width))
      return failure();
    auto integer = cast<ac::MathIntAttr>(*actual).getCanonicalValue();
    index = llvm::APSInt(integer).extOrTrunc(*width);
  } else if (auto constant = value.getDefiningOp<ac::BitsConstant4Op>()) {
    index.emplace(constant.getPattern().size(), constant.getPattern(), 2);
  }
  if (!index || index->getActiveBits() > 64)
    return std::optional<uint64_t>{};
  auto size = hardware.getTableSize(table, bindings(owner), site);
  if (failed(size))
    return failure();
  if (index->getZExtValue() >= *size)
    return std::optional<uint64_t>{};
  return std::optional{index->getZExtValue()};
}

FailureOr<ProjectedNet> Plan::resolve(ProjectedNet access, Operation *use,
                                      bool live) {
  SmallVector<ProjectedNet> visited;
  for (;;) {
    if (llvm::is_contained(visited, access))
      return use->emitOpError()
             << "cyclic linked port forwarding or field projection";
    visited.push_back(access);
    auto [owner, value] = access.net;
    if (auto alias = aliases.find(access.net); alias != aliases.end()) {
      if (live && isa<BlockArgument>(value)) {
        auto &instance = instances.at(owner);
        Operation *allocation = instance.info.allocation
                                    ? instance.info.allocation.getOperation()
                                    : instance.collection.getOperation();
        liveSites.insert({*instance.info.parent, allocation});
      }
      access.net = alias->second.net;
      access.path.insert(access.path.begin(), alias->second.path.begin(),
                         alias->second.path.end());
      continue;
    }
    if (access.path.empty() || storedNets.contains(access.net))
      return access;
    auto first = access.path.front();
    auto *producer = value.getDefiningOp();
    if (live && producer)
      liveSites.insert({owner, producer});
    if (auto input = tableInputs.find(access.net); input != tableInputs.end()) {
      if (live)
        liveSites.insert(
            {input->second.source.first, instances.at(owner).collection});
      access.net = input->second.source;
      access.path.front().index += input->second.offset;
    } else if (auto get = dyn_cast_or_null<ac::StructGetOp>(producer)) {
      auto field = fieldOrdinal(get.getValue().getType(), get.getFieldAttr(),
                                owner, producer);
      if (failed(field))
        return failure();
      access.net.second = get.getValue();
      access.path.insert(access.path.begin(), {Projection::Field, *field});
    } else if (auto structure =
                   dyn_cast_or_null<ac::StructCreateOp>(producer)) {
      assert(first.kind == Projection::Field);
      access.net.second = structure.getValues()[first.index];
      access.path.erase(access.path.begin());
    } else if (auto create = dyn_cast_or_null<ac::TableCreateOp>(producer)) {
      assert(first.kind == Projection::Element);
      access.net.second = create.getInputs()[first.index];
      access.path.erase(access.path.begin());
    } else if (auto splat = dyn_cast_or_null<ac::TableSplatOp>(producer)) {
      assert(first.kind == Projection::Element);
      access.net.second = splat.getInput();
      auto type = hardware.resolveType(splat.getInput().getType(),
                                       bindings(owner), splat);
      if (failed(type))
        return failure();
      if (auto table = dyn_cast<ac::TableType>(*type)) {
        auto size = hardware.getTableSize(table, bindings(owner), splat);
        if (failed(size))
          return failure();
        access.path.front().index %= *size;
      } else {
        access.path.erase(access.path.begin());
      }
    } else if (auto view = dyn_cast_or_null<ac::TableViewOp>(producer)) {
      assert(first.kind == Projection::Element);
      auto lane =
          hardware.getViewSourceOrdinal(view, first.index, bindings(owner));
      if (failed(lane))
        return failure();
      access.net.second = view.getInput();
      access.path.front().index = *lane;
    } else if (auto collection = dyn_cast_or_null<ac::CollectionOp>(producer)) {
      assert(first.kind == Projection::Element);
      auto &elements = collectionElements.find({owner, collection})->second;
      auto port = cast<OpResult>(value).getResultNumber();
      Net output = instances.at(elements.front()).outputs[port];
      auto type = hardware.resolveType(output.second.getType(),
                                       bindings(output.first), collection);
      if (failed(type))
        return failure();
      if (auto table = dyn_cast<ac::TableType>(*type)) {
        auto size =
            hardware.getTableSize(table, bindings(output.first), collection);
        if (failed(size))
          return failure();
        access.net = instances.at(elements[first.index / *size]).outputs[port];
        access.path.front().index %= *size;
      } else {
        access.net = instances.at(elements[first.index]).outputs[port];
        access.path.erase(access.path.begin());
      }
    } else if (isa_and_nonnull<ac::TableReadOp, ac::TableGetOp>(producer)) {
      auto input = producer->getOperand(0);
      auto lane =
          staticOrdinal(producer->getOperand(1),
                        cast<ac::TableType>(input.getType()), owner, producer);
      if (failed(lane))
        return failure();
      if (!*lane)
        return access;
      access.net.second = input;
      access.path.insert(access.path.begin(), {Projection::Element, **lane});
    } else {
      return access;
    }
  }
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
          net, ProjectedNet{
                   {*instance.info.parent, allocation.getInputs()[index]}, {}});
      aliasesToValidate.push_back(net);
    } else if (auto collection = instance.collection) {
      Net source{*instance.info.parent, collection.getInputs()[index]};
      auto type = hardware.resolveType(arg.getType(), bindings(id), collection);
      if (failed(type))
        return failure();
      if (auto table = dyn_cast<ac::TableType>(*type)) {
        auto size = hardware.getTableSize(table, bindings(id), collection);
        if (failed(size))
          return failure();
        tableInputs.try_emplace(
            net, TableInput{source, instance.element * *size, *size});
      } else {
        aliases.try_emplace(
            net,
            ProjectedNet{source, {{Projection::Element, instance.element}}});
        aliasesToValidate.push_back(net);
      }
    } else {
      addSlot(net);
    }
  }
  for (auto [index, val] : llvm::enumerate(yield(block).getValues())) {
    Net net{id, val};
    instance.outputs.push_back(net);
    if (allocation) {
      Net alias{*instance.info.parent, allocation.getResult(index)};
      aliases.try_emplace(alias, ProjectedNet{net, {}});
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
    } else if (auto collection = dyn_cast<ac::CollectionOp>(op)) {
      if (!isa<ac::ModuleOp>(hardware.resolveCallee(collection)))
        return collection.emitOpError()
               << "unsupported t-gfsim Collection primitive "
               << collection.getCallee();
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
        if (projection(instance.inputs[*port]) != ProjectedNet{expected, {}})
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
      if (clk && (projection({id, clk}) != ProjectedNet{clock, {}} ||
                  projection({id, rst}) != ProjectedNet{reset, {}}))
        return op.emitOpError() << "storage must use the root clock and reset";
    }
  }
  return success();
}

LogicalResult Plan::require(Net net, std::optional<unsigned> consumer,
                            Operation *use) {
  return require(ProjectedNet{net, {}}, consumer, use);
}

LogicalResult Plan::require(ProjectedNet access,
                            std::optional<unsigned> consumer, Operation *use) {
  auto source = resolve(std::move(access), use, true);
  return failed(source) ? failure() : requireResolved(*source, consumer, use);
}

LogicalResult Plan::requireResolved(ProjectedNet access,
                                    std::optional<unsigned> consumer,
                                    Operation *use) {
  auto net = access.net;
  auto [owner, value] = net;
  if (net == clock)
    return use->emitOpError() << "t-gfsim cannot use the root clock as data";
  if (storedNets.contains(net) ||
      (isa<BlockArgument>(value) && !tableInputs.contains(net))) {
    if (consumer)
      nodes[*consumer].fixed = false;
    return success();
  }
  auto *producer = value.getDefiningOp();
  if (isa_and_nonnull<ac::TableMapOp>(producer))
    liveTableMapResults.insert(net);
  if (auto queue = dyn_cast_or_null<ac::InstanceOp>(producer)) {
    auto index = cast<OpResult>(value).getResultNumber();
    auto callee = cast<ac::ModuleImportOp>(hardware.resolveCallee(queue));
    return use->emitOpError()
           << "t-gfsim Queue " << instances.at(owner).path << '.'
           << queue.getInstanceName() << " has a live raw output "
           << cast<StringAttr>(callee.getOutputNames()[index]).getValue();
  }
  unsigned dependency;
  auto projected = activateIndexedProjection(access);
  if (failed(projected))
    return failure();
  if (*projected) {
    dependency = **projected;
  } else if (tableInputs.contains(net) ||
             isa_and_nonnull<ac::CollectionOp, ac::TableGetOp>(producer)) {
    if (failed(activateResult(net)))
      return failure();
    dependency = resultNodes.find(net)->second;
  } else {
    Site site{owner, producer};
    if (failed(activate(site)))
      return failure();
    dependency = nodeIds.find(site)->second;
  }
  if (consumer)
    nodes[*consumer].inputs.push_back(dependency);
  return success();
}

LogicalResult Plan::copy(unsigned node, ProjectedNet destination,
                         ProjectedNet source) {
  auto *site = nodes[node].site.second;
  auto resolved = resolve(std::move(source), site, true);
  if (failed(resolved) || failed(requireResolved(*resolved, node, site)))
    return failure();
  nodes[node].copies.push_back({std::move(destination), std::move(*resolved)});
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

LogicalResult Plan::captureDependencies(InstanceId receiver, Value argument,
                                        ProjectedNet source,
                                        const llvm::DenseSet<Operation *> &live,
                                        unsigned consumer, Operation *use) {
  auto *projection = argument.getDefiningOp();
  SmallVector<ac::StructGetOp> fields;
  for (auto &operand : argument.getUses()) {
    auto *user = operand.getOwner();
    if (!live.contains(user) && !isa<ac::YieldOp>(user))
      continue;
    if (auto get = dyn_cast<ac::StructGetOp>(user)) {
      fields.push_back(get);
      continue;
    }
    auto resolved = resolve(std::move(source), use, true);
    if (failed(resolved) || failed(requireResolved(*resolved, consumer, use)))
      return failure();
    methodSlots.insert(resolved->net);
    if (projection)
      captureProjections.try_emplace({receiver, projection}, *resolved);
    return success();
  }
  if (projection)
    captureProjections.try_emplace({receiver, projection}, std::nullopt);
  else
    projectedCaptures.insert({receiver, argument});
  for (auto get : fields) {
    auto field =
        fieldOrdinal(argument.getType(), get.getFieldAttr(), receiver, get);
    if (failed(field))
      return failure();
    auto selected = source;
    selected.path.push_back({Projection::Field, *field});
    if (failed(captureDependencies(receiver, get.getResult(), selected, live,
                                   consumer, use)))
      return failure();
  }
  return success();
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
  for (auto [index, capture] : llvm::enumerate(method.getCaptures())) {
    ProjectedNet source{{bound.receiver, capture}, {}};
    auto guard = method.getGuard().front().getArgument(numFormals + index);
    auto body = method.getBody().front().getArgument(numFormals + index);
    if ((info.guardArguments.contains(guard) &&
         failed(captureDependencies(bound.receiver, guard, source,
                                    info.liveGuard, consumer, site.second))) ||
        (info.bodyArguments.contains(body) &&
         failed(captureDependencies(bound.receiver, body, source, info.liveBody,
                                    consumer, site.second))))
      return failure();
  }
  auto walk = method.getBody().walk([&](ac::SourceMethodCallOp nested) {
    return failed(callDependencies({bound.receiver, nested}, consumer, visited))
               ? WalkResult::interrupt()
               : WalkResult::advance();
  });
  return failure(walk.wasInterrupted());
}

FailureOr<std::optional<unsigned>>
Plan::activateIndexedProjection(const ProjectedNet &access) {
  auto [owner, value] = access.net;
  auto *op = value.getDefiningOp();
  if (access.path.empty() ||
      !isa_and_nonnull<ac::TableReadOp, ac::TableGetOp>(op))
    return std::optional<unsigned>{};

  auto &entries = indexedProjections[access.net];
  for (const auto &[path, index] : entries) {
    if (path != access.path)
      continue;
    if (!nodes[index].available)
      return op->emitOpError() << "cyclic same-cycle value dependence";
    return std::optional{index};
  }

  auto input = op->getOperand(0);
  auto indexValue = op->getOperand(1);
  ProjectedNet lane{{owner, input}, access.path};
  lane.path.insert(lane.path.begin(), {Projection::Element, 0});
  auto first = resolve(lane, op);
  if (failed(first))
    return failure();
  // An opaque table has one producer for all lanes. Keep its ordinary
  // indexed read and avoid generating an unrolled selection.
  auto whole = projection({owner, input});
  whole.path.append(lane.path.begin(), lane.path.end());
  if (*first == whole)
    return std::optional<unsigned>{};

  auto table = cast<ac::TableType>(input.getType());
  auto count = hardware.getTableSize(table, bindings(owner), op);
  if (failed(count))
    return failure();
  if (isa<ac::TableReadOp>(op) && failed(hardware.verifySourceIndex(
                                      indexValue, table, bindings(owner), op)))
    return failure();
  auto indexWidth =
      hardware.getPackedWidth(indexValue.getType(), bindings(owner), op);
  if (failed(indexWidth))
    return failure();
  uint64_t lanes = *count;
  if (*indexWidth < 64)
    lanes = std::min(lanes, uint64_t{1} << *indexWidth);
  auto index = unsigned(nodes.size());
  entries.emplace_back(access.path, index);
  nodes.push_back({{owner, op}, {}, true, false, value, {}, indexValue});
  liveSites.insert({owner, op});
  if (failed(require({owner, indexValue}, index, op)))
    return failure();
  for (uint64_t ordinal = 0; ordinal < lanes; ++ordinal) {
    lane.path.front().index = ordinal;
    if (failed(copy(index, access, lane)))
      return failure();
  }
  addSlot(access.net);
  complete(index);
  return std::optional{index};
}

LogicalResult Plan::activateResult(Net net) {
  auto [owner, result] = net;
  auto input = tableInputs.find(net);
  auto *site = input == tableInputs.end()
                   ? result.getDefiningOp()
                   : instances.at(owner).collection.getOperation();
  auto [position, fresh] = resultNodes.try_emplace(net, nodes.size());
  auto index = position->second;
  if (!fresh) {
    if (!nodes[index].available)
      return site->emitOpError() << "cyclic same-cycle value dependence";
    return success();
  }
  nodes.push_back({{owner, site}, {}, true, false, result, {}});
  liveSites.insert({owner, site});
  if (input != tableInputs.end()) {
    const auto &row = input->second;
    for (uint64_t lane = 0; lane < row.size; ++lane)
      if (failed(
              copy(index, {net, {{Projection::Element, lane}}},
                   {row.source, {{Projection::Element, row.offset + lane}}})))
        return failure();
  } else if (auto collection = dyn_cast<ac::CollectionOp>(site)) {
    auto size = hardware.getTableSize(cast<ac::TableType>(result.getType()),
                                      bindings(owner), collection);
    if (failed(size))
      return failure();
    // Resolve each logical lane to the element's actual output. The C++ Table
    // layout flattens consecutive axes just as the verified wire type does.
    for (uint64_t lane = 0; lane < *size; ++lane) {
      ProjectedNet laneNet{net, {{Projection::Element, lane}}};
      if (failed(copy(index, laneNet, laneNet)))
        return failure();
    }
  } else {
    auto get = cast<ac::TableGetOp>(site);
    if (failed(require({owner, get.getIndex()}, index, site)))
      return failure();
    if (result == get.getValue()) {
      auto ordinal =
          staticOrdinal(get.getIndex(), get.getInput().getType(), owner, site);
      if (failed(ordinal))
        return failure();
      if (*ordinal) {
        if (failed(copy(
                index, {net, {}},
                {{owner, get.getInput()}, {{Projection::Element, **ordinal}}})))
          return failure();
      } else if (failed(require({owner, get.getInput()}, index, site))) {
        return failure();
      }
    }
  }
  addSlot(net);
  complete(index);
  return success();
}

LogicalResult Plan::activate(Site site) {
  auto [position, fresh] = nodeIds.try_emplace(site, nodes.size());
  auto [id, op] = site;
  auto index = position->second;
  if (!fresh) {
    if (!nodes[index].available)
      return op->emitOpError() << "cyclic same-cycle value dependence";
    return success();
  }
  nodes.push_back({site, {}, isWire(op), false, {}, {}});
  liveSites.insert(site);
  if (auto get = dyn_cast<ac::StructGetOp>(op)) {
    auto field =
        fieldOrdinal(get.getValue().getType(), get.getFieldAttr(), id, get);
    if (failed(field) ||
        failed(copy(index, {{id, get.getResult()}, {}},
                    {{id, get.getValue()}, {{Projection::Field, *field}}})))
      return failure();
  } else if (auto read = dyn_cast<ac::TableReadOp>(op)) {
    auto ordinal =
        staticOrdinal(read.getIndex(), read.getInput().getType(), id, read);
    if (failed(ordinal))
      return failure();
    if (failed(require({id, read.getIndex()}, index, op)))
      return failure();
    if (*ordinal) {
      if (failed(copy(
              index, {{id, read.getValue()}, {}},
              {{id, read.getInput()}, {{Projection::Element, **ordinal}}})))
        return failure();
    } else if (failed(require({id, read.getInput()}, index, op))) {
      return failure();
    }
  } else if (auto choose = dyn_cast<ac::TableChooseOp>(op)) {
    if (failed(require({id, choose.getMask()}, index, op)))
      return failure();
  } else {
    for (Value operand : op->getOperands())
      if (failed(require({id, operand}, index, op)))
        return failure();
  }
  if (isa<ac::SourceMethodCallOp>(op)) {
    llvm::DenseSet<Site> visited;
    if (failed(callDependencies(site, index, visited)))
      return failure();
  }
  for (auto result : op->getResults()) {
    Net net{id, result};
    if (!storedNets.contains(net))
      addSlot(net);
  }
  complete(index);
  return success();
}

void Plan::complete(unsigned index) {
  auto &node = nodes[index];
  node.fixed &= llvm::all_of(
      node.inputs, [&](unsigned input) { return nodes[input].fixed; });
  node.available = true;
  if (node.fixed) {
    collectWrittenSlots(node, persistentSlots);
    fixed.push_back(index);
    return;
  }
  auto id = node.site.first;
  auto &work = instances.at(id).work;
  // All producers are available, so completion order is topological.
  workOrder.emplace_back(id, work.size());
  work.push_back(index);
}

void Plan::planWholeStateWrites() {
  // targets is keyed by lexical owner and operation, so every resolved call
  // site counts, even when dependency analysis visits a Method only once.
  llvm::DenseMap<Site, ac::SourceMethodCallOp> uniqueCalls;
  for (const auto &[site, bound] : targets) {
    if (!bound.method)
      continue;
    auto [entry, first] = uniqueCalls.try_emplace(
        Site{bound.receiver, bound.method}, cast<ac::SourceMethodCallOp>(site.second));
    if (!first)
      entry->second = {};
  }

  struct Uses {
    unsigned writes = 0;
    bool readsCandidate = false;
    bool wholeWriter = false;
  };
  SmallVector<Uses> uses(resources.size());
  for (const auto &[id, instance] : instances) {
    auto state = [&](StringAttr name) -> Uses & {
      return uses[states.find({id, name})->second];
    };
    for (auto method : instance.methods) {
      ac::SourceStateWriteOp soleWrite;
      unsigned writes = 0;
      method.getBody().walk([&](Operation *op) {
        if (auto read = dyn_cast<ac::SourceStateReadOp>(op))
          state(read.getStateAttr()).readsCandidate = true;
        if (auto write = dyn_cast<ac::SourceStateWriteOp>(op)) {
          ++state(write.getStateAttr()).writes;
          ++writes;
          soleWrite = write;
        }
      });
      if (writes != 1 || soleWrite->getBlock() != &method.getBody().front() ||
          !soleWrite.getSelectors().empty() || !soleWrite.getIndices().empty() ||
          !ac::isSourceLiteralTrue(soleWrite.getPath()))
        continue;
      auto call = uniqueCalls.lookup({id, method});
      if (call && isa<ac::ModuleOp>(call->getParentOp()) &&
          call->hasAttr("ac.registration_name") &&
          (!call.getPath() || ac::isSourceLiteralTrue(call.getPath())) &&
          total(Target{id, method, {}, ac::NativeQueueAction::None}))
        state(soleWrite.getStateAttr()).wholeWriter = true;
    }
  }
  for (auto [resource, use] : llvm::zip_equal(resources, uses))
    resource.wholeProposal =
        use.writes == 1 && use.wholeWriter && !use.readsCandidate;
}

bool Plan::canReuseProposal() const {
  if (resources.empty() || llvm::any_of(resources, [](const Resource &resource) {
        return !resource.wholeProposal;
      }))
    return false;

  // An ordinary combinational operation is reusable only when its execution
  // cannot report a fault. Region bodies must satisfy the same check.
  auto pure = [](Operation *op) {
    if (auto binary = dyn_cast<ac::BitsBinaryOp>(op))
      return binary.getOpcode() != "udiv" && binary.getOpcode() != "urem";
    return isa<ac::YieldOp>(op) ||
           (isWire(op) && !isa<ac::ValueRequireOp>(op));
  };
  for (const auto &node : nodes) {
    auto *op = node.site.second;
    // Planned port copies only read scheduled values. Registration bodies
    // and guards are checked in their own lexical instances below.
    if (!node.copies.empty() || isa<ac::SourceMethodCallOp>(op))
      continue;
    if (op->walk([&](Operation *nested) {
          return pure(nested) ? WalkResult::advance() : WalkResult::interrupt();
        }).wasInterrupted())
      return false;
  }
  for (const auto &[id, instance] : instances) {
    for (auto method : instance.methods) {
      if (method->walk([&](Operation *op) {
            if (op == method.getOperation() || pure(op))
              return WalkResult::advance();
            if (auto write = dyn_cast<ac::SourceStateWriteOp>(op))
              if (resources[states.find({id, write.getStateAttr()})->second]
                      .wholeProposal)
                return WalkResult::advance();
            return WalkResult::interrupt();
          }).wasInterrupted())
        return false;
    }
  }
  return true;
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
