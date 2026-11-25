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
  bool wholeProposal = false;
};

struct Projection {
  enum Kind { Field, Element } kind;
  uint64_t index;
  bool operator==(const Projection &) const = default;
};

struct ProjectedNet {
  Net net;
  llvm::SmallVector<Projection, 2> path;
  bool operator==(const ProjectedNet &) const = default;
};

struct Copy {
  ProjectedNet destination, source;
};

struct Node {
  Site site;
  llvm::SmallVector<unsigned> inputs;
  bool fixed = false;
  bool available = false;
  // A TableGet result has its own dependencies. Copy nodes replace ordinary
  // operation emission for port assembly and for projected wire reads.
  mlir::Value result;
  llvm::SmallVector<Copy, 0> copies;
  // If set, the copies are alternatives in logical lane order.
  mlir::Value copyIndex = {};
};

struct Instance {
  ac::TransactionLexicalInstance info;
  ac::CollectionOp collection = {};
  uint64_t element = 0;
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

  bool canReuseProposal() const;
  ProjectedNet projection(Net net) const;
  bool isLive(Site site) const { return liveSites.contains(site); }
  bool isTableMapResultLive(Net result) const {
    return liveTableMapResults.contains(result);
  }
  const std::optional<ProjectedNet> *captureProjection(Site site) const {
    auto found = captureProjections.find(site);
    return found == captureProjections.end() ? nullptr : &found->second;
  }
  bool captureByProjection(Net net) const {
    return projectedCaptures.contains(net);
  }
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
  llvm::DenseSet<Net> persistentSlots;
  llvm::DenseSet<Net> methodSlots;
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
  void planWholeStateWrites();
  void scheduleWork();
  mlir::LogicalResult activate(Site site);
  mlir::LogicalResult activateResult(Net net);
  mlir::FailureOr<std::optional<unsigned>>
  activateIndexedProjection(const ProjectedNet &access);
  void complete(unsigned index);
  mlir::LogicalResult copy(unsigned node, ProjectedNet destination,
                           ProjectedNet source);
  mlir::FailureOr<ProjectedNet>
  resolve(ProjectedNet access, mlir::Operation *use, bool live = false);
  mlir::FailureOr<unsigned> fieldOrdinal(mlir::Type type,
                                         mlir::StringAttr field,
                                         InstanceId owner,
                                         mlir::Operation *site) const;
  mlir::FailureOr<std::optional<uint64_t>>
  staticOrdinal(mlir::Value value, ac::TableType table, InstanceId owner,
                mlir::Operation *site) const;
  mlir::LogicalResult require(ProjectedNet access,
                              std::optional<unsigned> consumer,
                              mlir::Operation *use);
  mlir::LogicalResult requireResolved(ProjectedNet access,
                                      std::optional<unsigned> consumer,
                                      mlir::Operation *use);
  mlir::LogicalResult require(Net net, std::optional<unsigned> consumer,
                              mlir::Operation *use);
  mlir::LogicalResult
  captureDependencies(InstanceId receiver, mlir::Value argument,
                      ProjectedNet source,
                      const llvm::DenseSet<mlir::Operation *> &live,
                      unsigned consumer, mlir::Operation *use);
  mlir::LogicalResult callDependencies(Site call, unsigned consumer,
                                       llvm::DenseSet<Site> &visited);
  mlir::LogicalResult addResource(InstanceId id, ac::SourceStateOp state);
  mlir::LogicalResult addQueue(InstanceId id, ac::InstanceOp queue);
  void addSlot(Net net);

  struct TableInput {
    Net source;
    uint64_t offset, size;
  };
  llvm::DenseMap<Net, ProjectedNet> aliases, resolvedAliases;
  llvm::DenseMap<Net, TableInput> tableInputs;
  llvm::DenseMap<Site, llvm::SmallVector<InstanceId, 0>> collectionElements;
  llvm::DenseMap<Site, Target> targets;
  llvm::DenseMap<Site, unsigned> nodeIds;
  llvm::DenseMap<Net, unsigned> resultNodes;
  llvm::DenseMap<Net,
                 llvm::SmallVector<
                     std::pair<llvm::SmallVector<Projection, 2>, unsigned>, 0>>
      indexedProjections;
  llvm::DenseSet<Site> activatedMethods, liveSites;
  llvm::DenseSet<Net> liveTableMapResults;
  llvm::DenseSet<Net> projectedCaptures;
  llvm::DenseMap<Site, std::optional<ProjectedNet>> captureProjections;
  llvm::DenseMap<Site, bool> totalGuards;
  llvm::SmallVector<Net> aliasesToValidate;
  llvm::SmallVector<std::pair<InstanceId, size_t>> workOrder;
};

} // namespace acir::compiler::linked

#endif
