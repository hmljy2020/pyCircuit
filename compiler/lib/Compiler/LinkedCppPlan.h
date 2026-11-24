#ifndef ACIR_LIB_COMPILER_LINKEDCPPPLAN_H
#define ACIR_LIB_COMPILER_LINKEDCPPPLAN_H

#include "pycircuit/Dialect/ACIR/SourceMethodAnalysis.h"
#include "pycircuit/Dialect/ACIR/SourceTransactionStateAnalysis.h"
#include "llvm/ADT/DenseSet.h"

namespace acir::compiler::linked {

using InstanceId = ac::LexicalInstanceId;
using Net = std::pair<InstanceId, mlir::Value>;
using Site = std::pair<InstanceId, mlir::Operation *>;

struct Target {
  InstanceId receiver;
  ac::SourceMethodOp method;
  ac::InstanceOp queue;
  ac::NativeQueueAction action;
};

struct Method {
  llvm::DenseSet<mlir::Operation *> liveGuard, liveBody;
  llvm::DenseSet<mlir::Value> guardArguments, bodyArguments, guardValues,
      bodyValues;
  llvm::DenseMap<mlir::Attribute, ac::SourceMethodCallOp> calls;
};

struct Resource {
  enum Kind { State, Queue, Held } kind;
  InstanceId owner;
  mlir::Type type;
  std::string name;
  Net initial;
  uint64_t depth = 0;
  uint64_t latency = 0;
};

struct Node {
  Site site;
  llvm::SmallVector<unsigned> inputs;
  bool fixed = false;
  bool available = false;
};

struct Instance {
  ac::TransactionLexicalInstance info;
  std::string path;
  llvm::SmallVector<unsigned> work;
  unsigned workSegments = 0;
  llvm::SmallVector<Net> inputs, outputs;
  llvm::SmallVector<ac::SourceMethodOp> methods;
};

struct WorkSegment {
  InstanceId instance;
  unsigned ordinal;
  size_t begin, end; // Half-open range in Instance::work.
};

class Plan {
public:
  explicit Plan(mlir::ModuleOp package) : package(package), hardware(package) {}
  mlir::LogicalResult build();

  Net resolve(Net net) const;
  const Target &target(Site site) const { return targets.find(site)->second; }
  const ac::HardwareBindings &bindings(InstanceId id) const {
    return instances.find(id)->second.info.bindings;
  }
  mlir::Operation *module(InstanceId id) const {
    return instances.find(id)->second.info.definition;
  }

  mlir::ModuleOp package;
  ac::HardwareAnalysis hardware;
  std::map<InstanceId, Instance> instances;
  llvm::DenseMap<mlir::Operation *, Method> methods;
  llvm::DenseMap<Net, unsigned> slotIds, storedNets;
  llvm::SmallVector<Net> slots;
  llvm::SmallVector<Resource> resources;
  llvm::DenseMap<Site, unsigned> queues;
  llvm::DenseMap<std::pair<InstanceId, mlir::StringAttr>, unsigned> states;
  llvm::SmallVector<Node> nodes;
  llvm::SmallVector<unsigned> fixed;
  llvm::SmallVector<WorkSegment> schedule;
  Net clock{}, reset{};

private:
  Method &prepareMethod(ac::SourceMethodOp method);
  bool total(const Target &target);
  bool alwaysTrue(mlir::Value predicate, InstanceId id,
                  ac::SourceMethodOp method);
  mlir::LogicalResult connect(InstanceId id, Instance &instance);
  mlir::LogicalResult checkDomains();
  void scheduleWork();
  mlir::LogicalResult activate(Site site);
  mlir::LogicalResult require(Net net, std::optional<unsigned> consumer,
                              mlir::Operation *use);
  mlir::LogicalResult callDependencies(Site call, unsigned consumer,
                                       llvm::DenseSet<Site> &visited);
  mlir::LogicalResult addResource(InstanceId id, ac::SourceStateOp state);
  mlir::LogicalResult addQueue(InstanceId id, ac::InstanceOp queue);
  void addSlot(Net net);

  llvm::DenseMap<Net, Net> aliases;
  llvm::DenseMap<Site, Target> targets;
  llvm::DenseMap<Site, unsigned> nodeIds;
  llvm::DenseSet<Site> activatedMethods;
  llvm::DenseMap<Site, bool> totalGuards;
  llvm::SmallVector<Net> aliasesToValidate;
  llvm::SmallVector<std::pair<InstanceId, size_t>> workOrder;
};

} // namespace acir::compiler::linked

#endif
