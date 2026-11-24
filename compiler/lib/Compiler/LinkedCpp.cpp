#include "LinkedCpp.h"
#include "LinkedCppPlan.h"
#include "LinkedSourceACIR.h"

#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/ADT/StringSwitch.h"
#include "llvm/Support/raw_ostream.h"

#include <limits>

using namespace mlir;

namespace acir::compiler {
namespace linked {
namespace {

// Bound the generated source in one function and the extra automatic storage
// before the host compiler has a chance to eliminate temporaries.
constexpr size_t maxInlineWork = 8192;
constexpr uint64_t maxLocalBytes = 128 * 1024;
constexpr size_t minCachedOperations = 256;
constexpr size_t minConeOperations = 128;

std::string quote(StringRef name) {
  std::string result = "\"";
  for (unsigned char ch : name) {
    if (ch == '\\' || ch == '"') {
      result += '\\';
      result += ch;
    } else if (ch < 32 || ch >= 127) {
      result += '\\';
      result += '0' + ((ch >> 6) & 7);
      result += '0' + ((ch >> 3) & 7);
      result += '0' + (ch & 7);
    } else {
      result += ch;
    }
  }
  return result + '"';
}

std::string portName(StringRef prefix, StringAttr source) {
  StringRef spelling = source.getValue();
  if (!spelling.starts_with("_") && !spelling.contains("__") &&
      !spelling.starts_with("encoded_") &&
      llvm::all_of(spelling, [](unsigned char ch) {
        return llvm::isAlnum(ch) || ch == '_';
      }))
    return (prefix + spelling).str();

  // Reserve the spelling prefix for escaped names to keep the mapping
  // injective.
  std::string name = (prefix + "encoded_").str();
  for (unsigned char ch : spelling) {
    name += "0123456789abcdef"[ch >> 4];
    name += "0123456789abcdef"[ch & 15];
  }
  return name;
}

std::string numberName(StringRef prefix, uint64_t ordinal) {
  return prefix.str() + std::to_string(ordinal);
}

std::string workName(const Instance &instance, unsigned ordinal) {
  return instance.workSegments == 1 ? "Work" : numberName("Work_", ordinal);
}

std::string join(ArrayRef<std::string> parts, StringRef split = ", ") {
  return llvm::join(parts, split);
}

std::string bits(uint64_t width) { return numberName("Bits<", width) + ">"; }

std::string literal(const llvm::APInt &value, unsigned width,
                    bool constant = false) {
  auto resized = value.zextOrTrunc(width);
  if (constant)
    return bits(width) + "(" + std::to_string(resized.getZExtValue()) + "ULL)";
  SmallVector<std::string> words;
  for (unsigned i = 0; i < resized.getNumWords(); ++i)
    words.push_back(std::to_string(resized.getRawData()[i]) + "ULL");
  return bits(width) + "{" + join(words) + "}";
}

struct Expr {
  Expr(InstanceId id, StringRef qualifier) : owner(id), base(qualifier) {}
  InstanceId owner;
  std::string base;
  llvm::DenseMap<Value, std::string> values;
  const Method *method = nullptr;
  const llvm::DenseSet<Value> *regionValues = nullptr;
  ArrayRef<bool> regionYields;
  Value selectedResult;
  bool guard = false;
  bool constant = false;
};

struct Selection {
  std::string access;
  uint64_t low = 0;
  uint64_t width = 0;
  SmallVector<std::string> bounds;
  SmallVector<std::string> offsets;
  std::string sliceStart;
};

struct Nominal {
  std::string name;
  uint64_t bytes;
};

struct WorkCone {
  ProjectedNet input;
  SmallVector<unsigned> nodes;
};

class Cpp {
public:
  Cpp(const Plan &plan, llvm::raw_ostream &out) : plan(plan), out(out) {}
  LogicalResult emit();

private:
  LogicalResult planTypes();
  void planForwardedResults();
  LogicalResult planConstantSlots();
  LogicalResult planWorkCone();
  FailureOr<std::string> type(Type type, InstanceId id, Operation *site,
                              uint64_t *bytes = nullptr);
  FailureOr<unsigned> width(Type type, InstanceId id, Operation *site);
  FailureOr<Selection> select(Type type, ArrayAttr selectors, InstanceId id,
                              Operation *site, const Expr *scope = nullptr,
                              ValueRange indices = {});
  FailureOr<std::string> field(Type &type, StringRef name, InstanceId id,
                               Operation *site, uint64_t *low = nullptr);

  std::string slot(Net net) const;
  std::string projectedSlot(const ProjectedNet &access) const;
  LogicalResult emitNode(const Node &node, Expr &scope, unsigned indent);
  std::string ref(const Expr &expr, Value value) const;
  std::string var(Expr &expr, Value value);
  std::string storage(unsigned ordinal) const {
    return numberName("storage_", ordinal);
  }
  std::string function(ac::SourceMethodOp method, bool guard);
  FailureOr<std::string> signature(ac::SourceMethodOp method, InstanceId id,
                                   bool guard, Expr *scope = nullptr);
  LogicalResult block(Block &block, Expr &scope, unsigned indent);
  LogicalResult statement(Operation &op, Expr &scope, unsigned indent);
  LogicalResult tableRegion(Operation &op, Expr &scope, unsigned indent);
  LogicalResult checkGet(ac::TableGetOp op, const Expr &scope, uint64_t size);
  bool liveUse(OpOperand &use, const Expr &scope) const;
  bool local(const Expr &scope) const {
    return scope.method || scope.regionValues;
  }
  LogicalResult registration(ac::SourceMethodCallOp op, Expr &scope);
  LogicalResult invoke(ac::SourceMethodCallOp call, Expr &scope,
                       unsigned indent);
  LogicalResult branch(ac::SourceBranchOp op, Expr &scope, unsigned indent);
  LogicalResult work(const WorkSegment &segment, StringRef qualifier,
                     bool edge = false);
  LogicalResult sampleWork();
  LogicalResult xfer();
  LogicalResult emitModel(ArrayAttr samples);
  void rootOutputBindings(const Instance &root);
  bool needsMethod(InstanceId id, ac::SourceMethodOp method) const {
    return !inlineWork || plan.usesMethod({id, method});
  }
  bool needsModule(InstanceId id, const Instance &instance) const {
    return (!inlineWork && instance.workSegments) ||
           llvm::any_of(instance.methods,
                        [&](auto method) { return needsMethod(id, method); });
  }

  const Plan &plan;
  llvm::raw_ostream &out;
  llvm::DenseMap<Type, Nominal> nominals;
  llvm::DenseMap<Value, std::string> inputs;
  llvm::DenseMap<Operation *, unsigned> methodIds;
  llvm::DenseMap<Net, Net> forwardedResults;
  llvm::DenseSet<Net> localSlots, borrowedSlots, constantSlots, coneSlots;
  llvm::DenseSet<unsigned> coneNodes;
  WorkCone cone;
  SmallVector<Value> cacheInputs;
  bool cacheWork = false;
  bool inlineWork = false;
  unsigned nextLocal = 0;
};

FailureOr<unsigned> Cpp::width(Type t, InstanceId id, Operation *site) {
  auto w = plan.hardware.getPackedWidth(t, plan.bindings(id), site);
  if (failed(w))
    return failure();
  if (!*w || *w > std::numeric_limits<unsigned>::max())
    return site->emitOpError() << "unsupported t-gfsim value width";
  return unsigned(*w);
}

FailureOr<std::string> Cpp::type(Type t, InstanceId id, Operation *site,
                                 uint64_t *bytes) {
  auto actual = plan.hardware.resolveType(t, plan.bindings(id), site);
  if (failed(actual))
    return failure();
  if (isa<ac::BitsType>(*actual)) {
    auto w = width(*actual, id, site);
    if (failed(w))
      return failure();
    if (bytes)
      *bytes = 8 * llvm::divideCeil(*w, 64);
    return bits(*w);
  }
  if (auto table = dyn_cast<ac::TableType>(*actual)) {
    auto shape = plan.hardware.getTableShape(table, plan.bindings(id), site);
    auto element = type(table.getElementType(), id, site, bytes);
    if (failed(shape) || failed(element))
      return failure();
    std::string result = "gfsim::table<" + *element;
    for (uint64_t extent : *shape) {
      result += ", " + std::to_string(extent) + "ULL";
      if (bytes)
        *bytes = *bytes > maxLocalBytes / extent ? maxLocalBytes + 1
                                                : *bytes * extent;
    }
    return result + ">";
  }
  if (auto found = nominals.find(*actual); found != nominals.end()) {
    if (bytes)
      *bytes = found->second.bytes;
    return found->second.name;
  }

  std::string name = numberName("Record_", nominals.size());
  if (auto record = dyn_cast<ac::StructType>(*actual)) {
    auto definition = plan.hardware.lookupStruct(record);
    SmallVector<std::string> fields;
    uint64_t recordBytes = 0;
    for (auto raw : definition.getFields()) {
      auto fieldType =
          cast<DictionaryAttr>(raw).getAs<TypeAttr>("type").getValue();
      uint64_t fieldBytes;
      auto fieldCpp = type(fieldType, id, site, &fieldBytes);
      if (failed(fieldCpp))
        return failure();
      fields.push_back(*fieldCpp);
      recordBytes += fieldBytes;
    }
    name = numberName("Record_", nominals.size());
    // Members occupy word-aligned storage. Round empty records up as well.
    recordBytes = std::max(recordBytes, uint64_t{8});
    nominals.try_emplace(*actual, Nominal{name, recordBytes});
    if (bytes)
      *bytes = recordBytes;
    out << "struct " << name << " { // " << record.getName().getValue() << '\n';
    for (auto [i, member] : llvm::enumerate(fields))
      out << "  " << member << " field_" << i << "{};\n";
    if (cacheWork) {
      out << "  friend bool SameValue([[maybe_unused]] const " << name
          << " &lhs, [[maybe_unused]] const " << name
          << " &rhs) {\n    return true";
      for (unsigned i = 0; i < fields.size(); ++i)
        out << "\n        && SameValue(lhs.field_" << i << ", rhs.field_" << i
            << ')';
      out << ";\n  }\n";
    }
    out << "};\ninline std::string Format(const " << name << " &value) {\n"
        << "  return std::string(\"{\")";
    for (unsigned i = 0; i < fields.size(); ++i)
      out << (i ? " + \", \"" : "") << " + Format(value.field_" << i << ')';
    out << " + \"}\";\n}\n";
    return name;
  }
  if (auto enumeration = dyn_cast<ac::EnumType>(*actual)) {
    auto definition = plan.hardware.resolveEnum(enumeration, site);
    if (failed(definition))
      return failure();
    uint64_t size = 8 * llvm::divideCeil(definition->width, 64);
    nominals.try_emplace(*actual, Nominal{name, size});
    if (bytes)
      *bytes = size;
    out << "struct " << name << " { " << bits(definition->width) << " code{};";
    if (cacheWork)
      out << " friend bool SameValue(const " << name << " &lhs, const " << name
          << " &rhs) { return SameValue(lhs.code, rhs.code); }";
    out << " }; // " << enumeration.getName().getValue() << '\n'
        << "inline std::string Format(const " << name
        << " &value) { return Format(value.code); }\n";
    return name;
  }
  return site->emitOpError() << "unsupported t-gfsim payload " << *actual;
}

FailureOr<std::string> Cpp::field(Type &t, StringRef name, InstanceId id,
                                  Operation *site, uint64_t *low) {
  auto resolved = plan.hardware.resolveType(t, plan.bindings(id), site);
  if (failed(resolved))
    return failure();
  auto record = dyn_cast<ac::StructType>(*resolved);
  if (!record)
    return site->emitOpError() << "expected a Struct for field " << name;
  auto fields = plan.hardware.lookupStruct(record).getFields();
  for (unsigned index = fields.size(); index > 0; --index) {
    auto row = cast<DictionaryAttr>(fields[index - 1]);
    Type member = row.getAs<TypeAttr>("type").getValue();
    if (row.getAs<StringAttr>("name").getValue() == name) {
      t = member;
      return numberName(".field_", index - 1);
    }
    if (low) {
      auto memberWidth = width(member, id, site);
      if (failed(memberWidth))
        return failure();
      *low += *memberWidth;
    }
  }
  return site->emitOpError() << "unknown Struct field " << name;
}

FailureOr<Selection> Cpp::select(Type t, ArrayAttr selectors, InstanceId id,
                                 Operation *site, const Expr *scope,
                                 ValueRange indices) {
  Selection selected;
  SmallVector<std::string> row;
  for (auto raw : selectors) {
    auto selector = dyn_cast<DictionaryAttr>(raw);
    auto name = selector ? selector.getAs<StringAttr>("field")
                         : dyn_cast<StringAttr>(raw);
    if (name) {
      auto member = field(t, name.getValue(), id, site, &selected.low);
      if (failed(member))
        return failure();
      selected.access += *member;
      continue;
    }

    auto ordinal =
        selector ? selector.getAs<IntegerAttr>("index") : IntegerAttr();
    auto resolved = plan.hardware.resolveType(t, plan.bindings(id), site);
    if (failed(resolved))
      return failure();
    auto table = dyn_cast<ac::TableType>(*resolved);
    if (!ordinal || !table || !scope)
      return site->emitOpError() << "invalid t-gfsim state index selector";
    auto shape = plan.hardware.getTableShape(table, plan.bindings(id), site);
    if (failed(shape))
      return failure();

    t = table.getElementType();
    if (shape->size() > 1)
      t = ac::TableType::get(
          site->getContext(),
          ArrayAttr::get(site->getContext(),
                         table.getShape().getValue().drop_front()),
          t);
    auto stride = width(t, id, site);
    if (failed(stride))
      return failure();
    auto extent = std::to_string(shape->front()) + "ULL";
    auto index = ref(*scope, indices[ordinal.getInt()]);
    selected.bounds.push_back("IndexInRange(" + index + ", " + extent + ")");
    auto lowWord = index + ".value()";
    // Logical row zero occupies the highest bits of the containing Table.
    selected.offsets.push_back("(" + extent + " - 1 - " + lowWord + ") * " +
                               std::to_string(*stride) + "ULL");

    uint64_t rowSize = 1;
    for (uint64_t dimension : ArrayRef(*shape).drop_front())
      rowSize *= dimension;
    row.push_back(lowWord + " * " + std::to_string(rowSize) + "ULL");
    if (shape->size() == 1) {
      selected.access += "[" + join(row, " + ") + "]";
      row.clear();
    }
  }
  if (!row.empty())
    selected.sliceStart = join(row, " + ");
  auto size = width(t, id, site);
  if (failed(size))
    return failure();
  selected.width = *size;
  return selected;
}

LogicalResult Cpp::planTypes() {
  size_t workSize = 0;
  size_t sampleSize = 0;
  bool hasWireWork = false;
  for (auto [index, node] : llvm::enumerate(plan.nodes)) {
    if (!node.fixed) {
      workSize += 1 + node.copies.size();
      sampleSize += !plan.edgeWork.contains(index);
      hasWireWork |= !isa<ac::SourceMethodCallOp, ac::SourceExpectOp,
                          ac::SourceObserveOp>(node.site.second);
    }
  }
  // Effect-only schedules have no intermediate wires to localize. Keep
  // their existing function boundaries unless the effects can be deferred.
  inlineWork = workSize <= maxInlineWork &&
               (hasWireWork || plan.pureWork || !plan.deferredWrites.empty());
  cacheWork = inlineWork && plan.pureWork && !plan.deferredWrites.empty() &&
              sampleSize >= minCachedOperations;
  planForwardedResults();
  uint64_t localBytes = 0;
  auto need = [&](Value value, InstanceId id, Operation *site) {
    return success(succeeded(type(value.getType(), id, site)));
  };
  for (const auto &resource : plan.resources)
    if (failed(
            type(resource.type, resource.owner, plan.module(resource.owner))))
      return failure();
  if (cacheWork) {
    uint64_t inputBytes = 0;
    for (auto [id, value] : plan.instances.at(0).inputs) {
      if (!plan.workInputs.contains(value) || value == plan.reset.second)
        continue;
      uint64_t bytes;
      if (failed(type(value.getType(), id, plan.module(id), &bytes)))
        return failure();
      inputBytes += bytes;
      cacheInputs.push_back(value);
    }
    // A cache miss already pays for Work. Keep the comparison and snapshot
    // small relative to the number of calculations it can avoid.
    cacheWork = inputBytes <= 4 * sampleSize;
    if (!cacheWork)
      cacheInputs.clear();
  }
  if (cacheWork && (failed(planConstantSlots()) || failed(planWorkCone())))
    return failure();
  llvm::DenseSet<Net> committedProjections;
  if (inlineWork)
    for (const auto &node : plan.nodes)
      // A StructGet has one producer and copies a complete value. A
      // projection of committed storage stays unchanged throughout Work.
      if (isa<ac::StructGetOp>(node.site.second) && node.copies.size() == 1 &&
          plan.storedNets.contains(node.copies.front().source.net))
        committedProjections.insert(node.copies.front().destination.net);
  for (Net net : plan.slots) {
    auto [id, value] = net;
    uint64_t bytes;
    if (failed(type(value.getType(), id, plan.module(id), &bytes)))
      return failure();
    if (!inlineWork || (id == 0 && isa<BlockArgument>(value)) ||
        plan.persistentSlots.contains(net) || plan.methodSlots.contains(net) ||
        forwardedResults.contains(net) || coneSlots.contains(net) ||
        (cacheWork && plan.cacheFrontier.contains(net)))
      continue;
    if (bytes > sizeof(uint64_t) && committedProjections.contains(net)) {
      borrowedSlots.insert(net);
    } else if (bytes <= maxLocalBytes - localBytes) {
      localSlots.insert(net);
      localBytes += bytes;
    }
  }
  for (const auto &node : plan.nodes) {
    auto [id, op] = node.site;
    if (!isa<ac::TableMapOp, ac::TableMatchOp>(op))
      continue;
    auto checked = op->walk([&](Operation *nested) {
      for (Value value : nested->getResults())
        if (failed(need(value, id, nested)))
          return WalkResult::interrupt();
      return WalkResult::advance();
    });
    if (checked.wasInterrupted())
      return failure();
  }
  for (const auto &[id, instance] : plan.instances) {
    for (Net port : llvm::concat<const Net>(instance.inputs, instance.outputs))
      if (failed(need(port.second, id, plan.module(id))))
        return failure();
    for (auto method : instance.methods) {
      auto &live = plan.methods.find(method)->second;
      for (Type signatureType :
           llvm::concat<const Type>(method.getFunctionType().getInputs(),
                                    method.getFunctionType().getResults()))
        if (failed(type(signatureType, id, method)))
          return failure();
      auto checked = method.walk([&](Operation *op) {
        auto isLive = [&](Operation *candidate) {
          return live.liveGuard.contains(candidate) ||
                 live.liveBody.contains(candidate);
        };
        auto *owner = op;
        while (owner != method && !isLive(owner))
          owner = owner->getParentOp();
        if (owner == method)
          return WalkResult::advance();
        for (auto value : op->getResults())
          if (failed(need(value, id, op)))
            return WalkResult::interrupt();
        return WalkResult::advance();
      });
      if (checked.wasInterrupted())
        return failure();
    }
  }
  return success();
}

LogicalResult Cpp::planConstantSlots() {
  llvm::DenseSet<Net> outputs;
  for (Net output : plan.instances.at(0).outputs)
    outputs.insert(plan.projection(output).net);
  for (unsigned index : plan.fixed) {
    const Node &node = plan.nodes[index];
    auto [id, op] = node.site;
    if (!node.copies.empty() ||
        !isa<ac::BitsConstantOp, ac::BitsConstant4Op, ac::BitsExtractOp,
             ac::BitsResizeOp>(op))
      continue;
    Net net{id, op->getResult(0)};
    // An exposed constant still has its own slot in every Model.
    if (outputs.contains(net))
      continue;
    if (op->getNumOperands()) {
      auto source = plan.projection({id, op->getOperand(0)});
      if (!source.path.empty() || !constantSlots.contains(source.net))
        continue;
    }
    auto size = width(net.second.getType(), id, op);
    if (failed(size))
      return failure();
    if (*size <= 64)
      constantSlots.insert(net);
  }
  return success();
}

LogicalResult Cpp::planWorkCone() {
  // A closed scalar cone can be evaluated before the rest of Work. Accept
  // only single-result wires and a single exactly identified sampled source.
  SmallVector<WorkCone> candidates;
  llvm::DenseMap<unsigned, unsigned> coneOfNode;
  llvm::DenseMap<Net, SmallVector<unsigned, 1>> conesByInput;
  auto findCone = [&](const ProjectedNet &input) {
    auto &matching = conesByInput[input.net];
    for (unsigned index : matching)
      if (candidates[index].input == input)
        return index;
    unsigned index = candidates.size();
    matching.push_back(index);
    candidates.push_back({input, {}});
    return index;
  };
  for (const auto &segment : plan.schedule) {
    auto operations = ArrayRef(plan.instances.at(segment.instance).work);
    for (unsigned index :
         operations.slice(segment.begin, segment.end - segment.begin)) {
      if (plan.edgeWork.contains(index))
        continue;
      const Node &node = plan.nodes[index];
      auto [id, op] = node.site;
      if (!isa<ac::BitsUnaryOp, ac::BitsBinaryOp, ac::BitsCompareOp,
               ac::BitsSelectOp, ac::BitsConcatOp, ac::BitsExtractOp,
               ac::BitsResizeOp, ac::StructGetOp>(op))
        continue;
      auto result = op->getResult(0);
      if (!isa<ac::BitsType>(result.getType()))
        continue;
      std::optional<unsigned> selected;
      if (node.readsSampleInput) {
        if (node.inputs.empty() && !node.copyIndex && node.copies.size() == 1 &&
            node.copies.front().destination.path.empty())
          selected = findCone(node.copies.front().source);
      } else {
        for (unsigned input : node.inputs) {
          if (plan.nodes[input].fixed)
            continue;
          auto parent = coneOfNode.find(input);
          if (parent == coneOfNode.end() ||
              (selected && *selected != parent->second)) {
            selected.reset();
            break;
          }
          selected = parent->second;
        }
      }
      if (!selected)
        continue;
      auto size = width(result.getType(), id, op);
      if (failed(size))
        return failure();
      if (*size > 64)
        continue;
      coneOfNode.try_emplace(index, *selected);
      candidates[*selected].nodes.push_back(index);
    }
  }
  if (candidates.empty())
    return success();
  auto best = llvm::max_element(candidates, [](const WorkCone &a,
                                               const WorkCone &b) {
    return a.nodes.size() < b.nodes.size();
  });
  if (best->nodes.size() < minConeOperations)
    return success();

  llvm::DenseSet<Net> escaping;
  unsigned chosen = best - candidates.begin();
  for (auto [index, node] : llvm::enumerate(plan.nodes)) {
    auto owner = coneOfNode.find(index);
    if (owner != coneOfNode.end() && owner->second == chosen)
      continue;
    for (unsigned input : node.inputs) {
      auto source = coneOfNode.find(input);
      if (source == coneOfNode.end() || source->second != chosen)
        continue;
      auto [id, op] = plan.nodes[input].site;
      escaping.insert({id, op->getResult(0)});
    }
  }
  // Persist only a small frontier relative to the calculation it replaces.
  if (escaping.size() * 2 > best->nodes.size())
    return success();
  cone = std::move(*best);
  coneNodes.insert(cone.nodes.begin(), cone.nodes.end());
  coneSlots = std::move(escaping);
  return success();
}

void Cpp::planForwardedResults() {
  if (!inlineWork || plan.deferredWrites.empty())
    return;
  llvm::DenseMap<Net, unsigned> constructions;
  SmallVector<unsigned> uses(plan.nodes.size());
  for (auto [index, node] : llvm::enumerate(plan.nodes)) {
    auto [owner, op] = node.site;
    if (!node.fixed && isa<ac::StructCreateOp, ac::TableCreateOp>(op))
      constructions.try_emplace(Net{owner, op->getResult(0)}, index);
    for (unsigned input : node.inputs)
      ++uses[input];
  }
  for (const auto &[index, write] : plan.deferredWrites) {
    auto [owner, op] = plan.nodes[index].site;
    auto proposal = plan.projection(write.value).net;
    for (auto [result, operand] :
         llvm::zip_equal(op->getResults(), write.outputs)) {
      auto source = plan.projection(operand);
      auto construction = constructions.find(source.net);
      // Use the result's storage for its otherwise unobservable constructor.
      // Root references and deferred proposals still need their own values.
      if (source.path.empty() && construction != constructions.end() &&
          uses[construction->second] == 1 && source.net != proposal &&
          !plan.persistentSlots.contains(source.net) &&
          !plan.methodSlots.contains(source.net))
        forwardedResults.try_emplace(source.net, Net{owner, result});
    }
  }
}

std::string Cpp::slot(Net net) const {
  return projectedSlot(plan.projection(net));
}

std::string Cpp::projectedSlot(const ProjectedNet &access) const {
  auto net = access.net;
  if (auto result = forwardedResults.find(net); result != forwardedResults.end())
    net = result->second;
  std::string result;
  if (auto state = plan.storedNets.find(net); state != plan.storedNets.end())
    result = storage(state->second) + ".Q()";
  else if (auto input = inputs.find(net.second);
           net.first == 0 && input != inputs.end())
    result = input->second;
  else
    result = numberName("slot_", plan.slotIds.find(net)->second);
  for (auto selector : access.path)
    if (selector.kind == Projection::Field)
      result += numberName(".field_", selector.index);
    else
      result += numberName("[", selector.index) + "]";
  return result;
}

LogicalResult Cpp::emitNode(const Node &node, Expr &scope, unsigned indent) {
  if (node.copyIndex) {
    auto value = ref(scope, node.copyIndex);
    auto target = scope.base + projectedSlot(node.copies.front().destination);
    if (auto get = dyn_cast<ac::TableGetOp>(node.site.second))
      if (failed(checkGet(get, scope, node.copies.size())))
        return failure();
    std::string ws(indent, ' ');
    out << ws << target << " = {};\n"
        << ws << "if (IndexInRange(" << value << ", " << node.copies.size()
        << "ULL)) {\n"
        << ws << "  switch (" << value << ".value()) {\n";
    for (auto [ordinal, copy] : llvm::enumerate(node.copies))
      out << ws << "  case " << ordinal << ": " << target << " = " << scope.base
          << projectedSlot(copy.source) << "; break;\n";
    out << ws << "  }\n" << ws << "}\n";
    return success();
  }
  if (!node.copies.empty()) {
    for (const auto &copy : node.copies) {
      out << std::string(indent, ' ');
      if (borrowedSlots.contains(copy.destination.net))
        out << "[[maybe_unused]] const auto &";
      out << scope.base << projectedSlot(copy.destination) << " = " << scope.base
          << projectedSlot(copy.source) << ";\n";
    }
    return success();
  }
  scope.selectedResult = node.result;
  return statement(*node.site.second, scope, indent);
}

std::string Cpp::ref(const Expr &scope, Value value) const {
  if (auto found = scope.values.find(value); found != scope.values.end())
    return found->second;
  return scope.base + slot({scope.owner, value});
}

std::string Cpp::var(Expr &scope, Value value) {
  std::string local = numberName("local_", nextLocal++);
  scope.values.try_emplace(value, local);
  return local;
}

std::string Cpp::function(ac::SourceMethodOp method, bool guard) {
  auto id = methodIds.try_emplace(method, methodIds.size()).first->second;
  return numberName(guard ? "Guard_" : "Call_", id);
}

FailureOr<std::string> Cpp::signature(ac::SourceMethodOp method, InstanceId id,
                                      bool guard, Expr *scope) {
  auto proto = method.getFunctionType();
  SmallVector<std::string> outputs;
  if (!guard)
    for (Type result : proto.getResults()) {
      auto t = type(result, id, method);
      if (failed(t))
        return failure();
      outputs.push_back(*t);
    }
  std::string result = guard             ? "bool"
                       : outputs.empty() ? "void"
                                         : "std::tuple<" + join(outputs) + ">";
  if (scope)
    result += numberName(" Model::Module_", id) + "::";
  else
    result += ' ';
  result += function(method, guard);
  result += guard ? "([[maybe_unused]] Sample &sample"
                  : "([[maybe_unused]] Transaction &txn";
  auto &info = plan.methods.find(method)->second;
  auto args =
      (guard ? method.getGuard() : method.getBody()).front().getArguments();
  for (unsigned index = 0; index < proto.getNumInputs(); ++index) {
    auto argument = args[index];
    if (guard && !info.guardArguments.contains(argument))
      continue;
    auto t = type(argument.getType(), id, method);
    if (failed(t))
      return failure();
    std::string name = numberName("arg_", index);
    result += ", [[maybe_unused]] const " + *t + " &" + name;
    if (scope)
      scope->values.try_emplace(argument, name);
  }
  result += ')';
  if (scope) {
    scope->method = &info;
    scope->guard = guard;
    for (auto [index, capture] : llvm::enumerate(method.getCaptures())) {
      auto argument = args[proto.getNumInputs() + index];
      const auto &live = guard ? info.guardArguments : info.bodyArguments;
      if (live.contains(argument) && !plan.captureByProjection({id, argument}))
        scope->values.try_emplace(argument, "model." + slot({id, capture}));
    }
  }
  return result;
}

LogicalResult Cpp::block(Block &block, Expr &scope, unsigned indent) {
  const auto &live =
      scope.guard ? scope.method->liveGuard : scope.method->liveBody;
  for (Operation &op : block.without_terminator())
    if (live.contains(&op) && failed(statement(op, scope, indent)))
      return failure();
  return success();
}

LogicalResult Cpp::branch(ac::SourceBranchOp op, Expr &scope, unsigned indent) {
  const auto &live =
      scope.guard ? scope.method->guardValues : scope.method->bodyValues;
  std::string ws(indent, ' ');
  for (auto result : op.getResults()) {
    if (!live.contains(result))
      continue;
    auto t = type(result.getType(), scope.owner, op);
    if (failed(t))
      return failure();
    out << ws << *t << ' ' << var(scope, result) << "{};\n";
  }
  out << ws << "if (" << ref(scope, op.getCondition()) << ".toBool()) {\n";
  auto arm = [&](Region &region) {
    Expr local = scope;
    if (failed(block(region.front(), local, indent + 2)))
      return failure();
    auto yield = cast<ac::YieldOp>(region.front().back());
    for (auto [result, value] :
         llvm::zip_equal(op.getResults(), yield.getValues()))
      if (live.contains(result))
        out << ws << "  " << ref(scope, result) << " = " << ref(local, value)
            << ";\n";
    return success();
  };
  if (failed(arm(op.getThenRegion())))
    return failure();
  out << ws << "} else {\n";
  if (failed(arm(op.getElseRegion())))
    return failure();
  out << ws << "}\n";
  return success();
}

LogicalResult Cpp::invoke(ac::SourceMethodCallOp call, Expr &scope,
                          unsigned indent) {
  const auto &target = plan.target({scope.owner, call});
  std::string ws(indent, ' ');
  SmallVector<std::string> args;
  for (auto value : call.getArguments())
    args.push_back(ref(scope, value));
  SmallVector<std::string> outputs;
  for (auto result : call.getResults()) {
    auto t = type(result.getType(), scope.owner, call);
    if (failed(t))
      return failure();
    outputs.push_back(var(scope, result));
    out << ws << "[[maybe_unused]] " << *t << ' ' << outputs.back() << "{};\n";
  }
  if (call.getPath()) {
    out << ws << "if (" << ref(scope, call.getPath()) << ".toBool()) {\n";
    ws += "  ";
  }
  if (target.queue) {
    auto ordinal = plan.queues.find({target.receiver, target.queue})->second;
    out << ws;
    if (!outputs.empty())
      out << outputs.front() << " = ";
    out << "model." << storage(ordinal);
    switch (target.action) {
    case ac::NativeQueueAction::Push:
      out << ".Push(txn, " << args.front();
      break;
    case ac::NativeQueueAction::Pop:
      out << ".Pop(txn";
      break;
    case ac::NativeQueueAction::Peek:
      out << ".Peek(txn";
      break;
    case ac::NativeQueueAction::None:
      llvm_unreachable("verified Queue Method");
    }
  } else {
    out << ws;
    if (!outputs.empty())
      out << "std::tie(" << join(outputs) << ") = ";
    out << "model." << numberName("module_", target.receiver) << '.'
        << function(target.method, false) << "(txn";
    for (const auto &argument : args)
      out << ", " << argument;
  }
  out << ");\n";
  if (call.getPath())
    out << std::string(indent, ' ') << "}\n";
  return success();
}

bool Cpp::liveUse(OpOperand &use, const Expr &scope) const {
  auto *user = use.getOwner();
  const auto *values = scope.regionValues;
  if (!values && scope.method)
    values =
        scope.guard ? &scope.method->guardValues : &scope.method->bodyValues;
  if (values) {
    if (auto yield = dyn_cast<ac::YieldOp>(user)) {
      if (auto branch = dyn_cast<ac::SourceBranchOp>(yield->getParentOp()))
        return values->contains(branch.getResult(use.getOperandNumber()));
      return !scope.regionValues || scope.regionYields[use.getOperandNumber()];
    }
    if (scope.regionValues)
      return llvm::any_of(user->getResults(),
                          [&](Value value) { return values->contains(value); });
    auto &ops = scope.guard ? scope.method->liveGuard : scope.method->liveBody;
    return ops.contains(user);
  }
  if (isa<ac::YieldOp>(user))
    return true;
  if (auto method = dyn_cast<ac::SourceMethodOp>(user)) {
    auto found = plan.methods.find(method);
    if (found == plan.methods.end())
      return false;
    unsigned number =
        method.getFunctionType().getNumInputs() + use.getOperandNumber();
    return found->second.guardArguments.contains(
               method.getGuard().front().getArgument(number)) ||
           found->second.bodyArguments.contains(
               method.getBody().front().getArgument(number));
  }
  return plan.isLive({scope.owner, user});
}

LogicalResult Cpp::checkGet(ac::TableGetOp get, const Expr &scope,
                            uint64_t size) {
  auto indexWidth = width(get.getIndex().getType(), scope.owner, get);
  if (failed(indexWidth))
    return failure();
  if (*indexWidth < 64 && (uint64_t{1} << *indexWidth) <= size)
    return success();

  auto condition = get.getInRange();
  auto provesRange = [&](Value tested, bool taken) {
    if (auto negate = tested.getDefiningOp<ac::BitsUnaryOp>()) {
      tested = negate.getInput();
      taken = !taken;
    }
    return taken && tested == condition;
  };
  auto protectedUse = [&](OpOperand &use) {
    Operation *user = use.getOwner();
    if (auto select = dyn_cast<ac::BitsSelectOp>(user)) {
      unsigned operand = use.getOperandNumber();
      if (operand && provesRange(select.getCondition(), operand == 1))
        return true;
    }
    if (auto merge = dyn_cast<ac::ValueMergeOp>(user)) {
      unsigned first = 1 + merge.getGuards().size();
      if (use.getOperandNumber() >= first &&
          provesRange(merge.getGuards()[use.getOperandNumber() - first], true))
        return true;
    }
    for (auto *region = user->getParentRegion(); region;
         region = region->getParentOp()->getParentRegion()) {
      auto branch = dyn_cast<ac::SourceBranchOp>(region->getParentOp());
      if (branch &&
          provesRange(branch.getCondition(), region == &branch.getThenRegion()))
        return true;
    }
    return false;
  };
  // A projection may run eagerly while its result is selected only in
  // range. Trace these value-only operations through to their actual uses.
  llvm::DenseSet<Value> visited;
  SmallVector<Value> pending{get.getValue()};
  while (!pending.empty()) {
    Value value = pending.pop_back_val();
    if (!visited.insert(value).second)
      continue;
    for (OpOperand &use : value.getUses()) {
      auto *user = use.getOwner();
      if (!liveUse(use, scope) || protectedUse(use))
        continue;
      if (isa<ac::StructGetOp, ac::EnumToBitsOp, ac::BitsExtractOp,
              ac::BitsResizeOp>(user) ||
          (isa<ac::TableReadOp>(user) && use.getOperandNumber() == 0)) {
        pending.push_back(user->getResult(0));
        continue;
      }
      auto maximum = plan.hardware.getSourceIndexMaximum(
          get.getIndex(), plan.bindings(scope.owner), get);
      if (succeeded(maximum) && maximum->getActiveBits() <= 64 &&
          maximum->getZExtValue() < size)
        return success();
      return get.emitOpError()
             << "t-gfsim requires a proven in-range index or a payload "
                "use guarded by its in_range result. Unprotected consumer "
             << user->getName() << " at " << user->getLoc();
    }
  }
  return success();
}

LogicalResult Cpp::tableRegion(Operation &op, Expr &scope, unsigned indent) {
  std::string ws(indent, ' ');
  auto map = dyn_cast<ac::TableMapOp>(op);
  auto match = dyn_cast<ac::TableMatchOp>(op);
  auto ordinal = numberName("lane_", nextLocal++);
  auto &body = op.getRegion(0).front();
  auto yielded = cast<ac::YieldOp>(body.back()).getValues();
  auto table = cast<ac::TableType>(map ? map.getResult(0).getType()
                                       : match.getInput().getType());
  auto count =
      plan.hardware.getTableSize(table, plan.bindings(scope.owner), &op);
  if (failed(count))
    return failure();

  // Bind isolated arguments before emitting the loop. Lines inside the region
  // use the same typed expression emitter as ordinary Work.
  Expr region = scope;
  region.selectedResult = {};
  SmallVector<std::string> arguments;
  if (map) {
    auto w = width(body.getArgument(0).getType(), scope.owner, &op);
    if (failed(w))
      return failure();
    arguments.push_back(bits(*w) + "{" + ordinal + "}");
    for (Value input : map.getTables())
      arguments.push_back(ref(scope, input) + "[" + ordinal + "]");
  } else {
    arguments.push_back(ref(scope, match.getInput()) + "[" + ordinal + "]");
  }
  for (Value captured :
       map ? ValueRange(map.getCaptures()) : ValueRange(match.getCaptures()))
    arguments.push_back(ref(scope, captured));
  for (auto [argument, name] : llvm::zip_equal(body.getArguments(), arguments))
    region.values.try_emplace(argument, name);

  const auto *live = scope.regionValues;
  if (!live && scope.method)
    live = scope.guard ? &scope.method->guardValues : &scope.method->bodyValues;
  SmallVector<bool> used;
  SmallVector<Value> pending;
  for (auto [result, value] : llvm::zip_equal(op.getResults(), yielded)) {
    bool needed =
        !map || (live ? live->contains(result)
                      : plan.isTableMapResultLive({scope.owner, result}));
    used.push_back(needed);
    if (!needed)
      continue;
    pending.push_back(value);
    if (local(scope)) {
      auto t = type(result.getType(), scope.owner, &op);
      if (failed(t))
        return failure();
      out << ws << "[[maybe_unused]] " << *t << ' ' << var(scope, result)
          << "{};\n";
    } else if (match) {
      out << ws << ref(scope, result) << " = {};\n";
    }
  }

  llvm::DenseSet<Value> values;
  while (!pending.empty()) {
    Value value = pending.pop_back_val();
    if (!values.insert(value).second)
      continue;
    if (auto get = value.getDefiningOp<ac::TableGetOp>();
        get && value == get.getInRange()) {
      pending.push_back(get.getIndex());
    } else if (auto *producer = value.getDefiningOp()) {
      llvm::append_range(pending, producer->getOperands());
    }
  }
  region.regionValues = &values;
  region.regionYields = used;
  out << ws << "for (std::size_t " << ordinal << " = 0; " << ordinal << " < "
      << *count << "ULL; ++" << ordinal << ") {\n";
  for (Operation &nested : body.without_terminator())
    if (llvm::any_of(nested.getResults(),
                     [&](Value value) { return values.contains(value); }) &&
        failed(statement(nested, region, indent + 2)))
      return failure();
  if (map) {
    for (auto [result, value, needed] :
         llvm::zip_equal(op.getResults(), yielded, used))
      if (needed)
        out << ws << "  " << ref(scope, result) << '[' << ordinal
            << "] = " << ref(region, value) << ";\n";
  } else {
    auto result = ref(scope, match.getMask());
    auto word = ordinal + " / 64";
    out << ws << "  if (" << ref(region, yielded.front()) << ".toBool())\n"
        << ws << "    " << result << ".setWord(" << word << ", " << result
        << ".word(" << word << ") | (std::uint64_t{1} << (" << ordinal
        << " % 64)));\n";
  }
  out << ws << "}\n";
  return success();
}

LogicalResult Cpp::statement(Operation &op, Expr &scope, unsigned indent) {
  auto id = scope.owner;
  std::string ws(indent, ' ');
  auto operand = [&](Value value) { return ref(scope, value); };
  auto arg = [&](unsigned index) { return operand(op.getOperand(index)); };
  auto assign = [&](Value value, const std::string &expression) {
    out << ws;
    if (scope.constant)
      out << "[[maybe_unused]] inline static constexpr auto " << operand(value);
    else if (local(scope))
      out << "[[maybe_unused]] auto " << var(scope, value);
    else
      out << operand(value);
    out << " = " << expression << ";\n";
  };
  auto scalar = [&](const std::string &expression) {
    assign(op.getResult(0), expression);
    return success();
  };
  auto operands = [&] {
    SmallVector<std::string> args;
    for (auto value : op.getOperands())
      args.push_back(operand(value));
    return join(args);
  };
  auto evalInt = [&](ac::StaticExprAttr attr) -> FailureOr<ac::MathIntAttr> {
    auto value = plan.hardware.evaluateStatic(attr, plan.bindings(id), &op);
    if (failed(value))
      return failure();
    auto number = dyn_cast<ac::MathIntAttr>(*value);
    if (!number)
      return op.emitOpError() << "expected resolved integer";
    return number;
  };

  if (auto constant = dyn_cast<ac::BitsConstantOp>(op)) {
    auto w = width(constant.getResult().getType(), id, &op);
    auto value = evalInt(constant.getValue());
    if (failed(w) || failed(value))
      return failure();
    return scalar(
        literal(llvm::APSInt(value->getCanonicalValue()), *w, scope.constant));
  }
  if (auto constant = dyn_cast<ac::BitsConstant4Op>(op))
    return scalar(literal(
        llvm::APInt(constant.getPattern().size(), constant.getPattern(), 2),
        constant.getPattern().size(), scope.constant));
  if (isa<ac::BitsUnaryOp>(op))
    return scalar("~" + arg(0));
  if (auto binary = dyn_cast<ac::BitsBinaryOp>(op)) {
    auto opcode = binary.getOpcode();
    auto token = llvm::StringSwitch<StringRef>(opcode)
                     .Case("add", " + ")
                     .Case("sub", " - ")
                     .Case("mul", " * ")
                     .Case("and", " & ")
                     .Case("or", " | ")
                     .Case("xor", " ^ ")
                     .Default("");
    if (!token.empty())
      return scalar(arg(0) + token.str() + arg(1));
    if (checksDivisor(binary)) {
      auto w = width(binary.getRhs().getType(), id, &op);
      if (failed(w))
        return failure();
      auto sample = scope.method && !scope.guard ? "txn.sample" : "sample";
      out << ws << "if (" << arg(1) << " == " << bits(*w) << "{}) " << sample
          << ".Fail(\"t-gfsim division by zero\");\n";
      return scalar(opcode.str() + "(" + operands() + ")");
    }
    return scalar(opcode.str() + "(" + arg(0) + ", ShiftAmount(" + arg(1) +
                  "))");
  }
  if (auto compare = dyn_cast<ac::BitsCompareOp>(op)) {
    auto pred = compare.getPredicate();
    auto token = llvm::StringSwitch<StringRef>(pred)
                     .Cases({"eq", "case_eq"}, " == ")
                     .Case("ne", " != ")
                     .Case("ult", " < ")
                     .Case("ule", " <= ")
                     .Case("ugt", " > ")
                     .Case("uge", " >= ")
                     .Default("");
    if (!token.empty())
      return scalar("Bits<1>{" + arg(0) + token.str() + arg(1) + "}");
    bool invert = pred == "sle" || pred == "sge";
    bool reverse = pred == "sgt" || pred == "sle";
    return scalar(std::string("Bits<1>{") + (invert ? "!" : "") +
                  "SignedLess(" + arg(reverse ? 1 : 0) + ", " +
                  arg(reverse ? 0 : 1) + ")}");
  }
  if (isa<ac::BitsSelectOp>(op))
    return scalar("mux(" + operands() + ")");
  if (isa<ac::BitsConcatOp>(op))
    return scalar("concat(" + operands() + ")");
  if (isa<ac::BitsResizeOp, ac::BitsExtractOp>(op)) {
    auto w = width(op.getResult(0).getType(), id, &op);
    if (failed(w))
      return failure();
    if (auto extract = dyn_cast<ac::BitsExtractOp>(op)) {
      auto low = evalInt(extract.getLow());
      if (failed(low))
        return failure();
      return scalar(numberName("extract<", *w) + ">(" + arg(0) + ", " +
                    std::to_string(
                        llvm::APSInt(low->getCanonicalValue()).getZExtValue()) +
                    ")");
    }
    auto resize = cast<ac::BitsResizeOp>(op);
    return scalar(numberName(resize.getMode().str() + "<", *w) + ">(" + arg(0) +
                  ")");
  }
  if (isa<ac::StructCreateOp>(op)) {
    auto t = type(op.getResult(0).getType(), id, &op);
    if (failed(t))
      return failure();
    return scalar(*t + "{" + operands() + "}");
  }
  if (auto get = dyn_cast<ac::StructGetOp>(op)) {
    if (auto projected = plan.captureProjection({id, &op}))
      return *projected ? scalar(scope.base + projectedSlot(**projected))
                        : success();
    Type record = get.getValue().getType();
    auto member = field(record, get.getField(), id, &op);
    return failed(member) ? failure() : scalar(arg(0) + *member);
  }
  if (auto create = dyn_cast<ac::EnumCreateOp>(op)) {
    auto t = type(create.getResult().getType(), id, &op);
    if (failed(t))
      return failure();
    auto enumeration =
        plan.hardware.resolveEnum(create.getResult().getType(), &op);
    if (failed(enumeration))
      return failure();
    for (const auto &member : enumeration->members)
      if (member.name.getValue() == create.getMember())
        return scalar(*t + "{" +
                      literal(llvm::APSInt(member.code.getCanonicalValue()),
                              enumeration->width) +
                      "}");
    return op.emitOpError() << "missing verified enum member";
  }
  if (isa<ac::EnumToBitsOp>(op))
    return scalar(arg(0) + ".code");
  if (auto from = dyn_cast<ac::EnumFromBitsOp>(op)) {
    auto t = type(from.getValue().getType(), id, &op);
    auto enumeration =
        plan.hardware.resolveEnum(from.getValue().getType(), &op);
    if (failed(t) || failed(enumeration))
      return failure();
    SmallVector<std::string> tests;
    for (const auto &member : enumeration->members)
      tests.push_back(arg(0) + " == " +
                      literal(llvm::APSInt(member.code.getCanonicalValue()),
                              enumeration->width));
    assign(from.getValue(), *t + "{" + arg(0) + "}");
    assign(from.getMember(), "Bits<1>{" + join(tests, " || ") + "}");
    return success();
  }
  auto tableSize = [&](Type t) {
    return plan.hardware.getTableSize(cast<ac::TableType>(t), plan.bindings(id),
                                      &op);
  };
  if (isa<ac::TableCreateOp, ac::TableSplatOp, ac::TableViewOp>(op)) {
    auto result = op.getResult(0);
    auto t = type(result.getType(), id, &op);
    if (failed(t))
      return failure();
    if (isa<ac::TableCreateOp>(op))
      return scalar(*t + "{{" + operands() + "}}");
    if (auto splat = dyn_cast<ac::TableSplatOp>(op)) {
      std::string populate = "result.elements.fill(" + arg(0) + ");";
      if (isa<ac::TableType>(splat.getInput().getType()))
        populate = "for (std::size_t i = 0; i < result.size; ++i) "
                   "result[i] = " +
                   arg(0) + "[i % " + arg(0) + ".size];";
      return scalar("[&] { " + *t + " result; " + populate +
                    " return result; }()");
    }
    auto view = cast<ac::TableViewOp>(op);
    auto count = tableSize(result.getType());
    if (failed(count))
      return failure();
    // Materialize the verified logical ordinal mapping for this binding.
    SmallVector<std::string> ordinals;
    for (uint64_t i = 0; i < *count; ++i) {
      auto source =
          plan.hardware.getViewSourceOrdinal(view, i, plan.bindings(id));
      if (failed(source))
        return failure();
      ordinals.push_back(std::to_string(*source));
    }
    return scalar("[&] { " + *t + " result; constexpr std::size_t order[]{" +
                  join(ordinals) +
                  "}; for (std::size_t i = 0; i < result.size; ++i) "
                  "result[i] = " +
                  arg(0) + "[order[i]]; return result; }()");
  }
  if (auto index = dyn_cast<ac::TableIndexOp>(op)) {
    auto extents =
        plan.hardware.resolveShape(index.getShape(), plan.bindings(id), &op);
    auto result = type(index.getResult().getType(), id, &op);
    if (failed(extents) || failed(result))
      return failure();
    uint64_t count = 1;
    for (uint64_t extent : *extents)
      count *= extent;
    std::string expression = "[&] { std::uint64_t ordinal = 0; ";
    for (auto [axis, extent] : llvm::enumerate(*extents)) {
      std::string limit = std::to_string(extent) + "ULL";
      expression += "if (!IndexInRange(" + arg(axis) + ", " + limit +
                    ")) return " + *result + "{" + std::to_string(count) +
                    "ULL}; ordinal = ordinal * " + limit + " + " + arg(axis) +
                    ".value(); ";
    }
    return scalar(expression + "return " + *result + "{ordinal}; }()");
  }
  if (isa<ac::TableReadOp, ac::TableGetOp>(op)) {
    auto count = tableSize(op.getOperand(0).getType());
    auto element = type(op.getResult(0).getType(), id, &op);
    if (failed(count) || failed(element))
      return failure();
    std::string predicate =
        "IndexInRange(" + arg(1) + ", " + std::to_string(*count) + "ULL)";
    auto access = [&] { return arg(0) + "[" + arg(1) + ".value()]"; };
    if (auto get = dyn_cast<ac::TableGetOp>(op)) {
      const auto *live = scope.regionValues;
      if (!live && scope.method)
        live = scope.guard ? &scope.method->guardValues
                           : &scope.method->bodyValues;
      auto needed = [&](Value value) {
        if (scope.selectedResult)
          return scope.selectedResult == value;
        return live ? live->contains(value) : !value.use_empty();
      };
      if (needed(get.getInRange()))
        assign(get.getInRange(), "Bits<1>{" + predicate + "}");
      if (!needed(get.getValue()))
        return success();
      if (failed(checkGet(get, scope, *count)))
        return failure();
      return scalar(predicate + " ? " + access() + " : " + *element + "{}");
    }
    auto read = cast<ac::TableReadOp>(op);
    if (failed(plan.hardware.verifySourceIndex(read.getIndex(),
                                               read.getInput().getType(),
                                               plan.bindings(id), &op)))
      return failure();
    return scalar(access());
  }
  if (isa<ac::TableMapOp, ac::TableMatchOp>(op))
    return tableRegion(op, scope, indent);
  if (auto choose = dyn_cast<ac::TableChooseOp>(op)) {
    auto count = tableSize(choose.getInput().getType());
    auto it = type(choose.getResult(0).getType(), id, &op);
    if (failed(count) || failed(it))
      return failure();
    auto ranks = std::to_string(choose.getCount());
    auto size = std::to_string(*count);
    auto selected = numberName("chosen_", nextLocal++);
    out << ws << "auto " << selected << " = [&] { std::array<std::size_t, "
        << ranks << "> result; result.fill(" << size << ");\n"
        << ws << "  for (std::size_t i = 0, rank = 0; i < " << size
        << " && rank < " << ranks << "; ++i) {\n"
        << ws << "    const auto lane = "
        << (choose.getOrder() == "low" ? "i" : size + " - 1 - i") << ";\n"
        << ws << "    if (" << operand(choose.getMask())
        << ".bit(lane)) result[rank++] = lane;\n"
        << ws << "  }\n"
        << ws << "  return result;\n"
        << ws << "}();\n";
    for (auto [i, value] : llvm::enumerate(choose.getResults())) {
      std::string rank =
          selected + "[" + std::to_string(i % choose.getCount()) + "]";
      std::string valid = rank + " != " + size;
      if (i < uint64_t(choose.getCount()))
        assign(value, *it + "{" + valid + " ? " + rank + " : 0}");
      else
        assign(value, "Bits<1>{" + valid + "}");
    }
    return success();
  }
  if (auto fold = dyn_cast<ac::TableFoldOp>(op)) {
    StringRef kind = fold.getKind();
    StringRef token = llvm::StringSwitch<StringRef>(kind)
                          .Case("add", "+")
                          .Case("mul", "*")
                          .Case("and", "&")
                          .Case("or", "|")
                          .Case("xor", "^")
                          .Default("");
    std::string combine = token.empty()
                              ? (kind == "min" ? "left < right ? left : right"
                                               : "left < right ? right : left")
                              : ("left " + token + " right").str();
    return scalar(
        "[&] { auto tree = " + arg(0) +
        ".elements; for (std::size_t n = tree.size(); n > 1; "
        "n = (n + 1) / 2) { for (std::size_t i = 0; i < n / 2; ++i) { "
        "auto left = tree[2 * i], right = tree[2 * i + 1]; "
        "tree[i] = " +
        combine +
        "; } if (n % 2) tree[n / 2] = tree[n - 1]; "
        "} return tree[0]; }()");
  }
  if (auto merge = dyn_cast<ac::ValueMergeOp>(op)) {
    assign(merge.getNext(), operand(merge.getBase()));
    assign(merge.getEn(), "Bits<1>{}");
    for (auto [path, when, replacement] : llvm::zip_equal(
             merge.getPaths(), merge.getGuards(), merge.getValues())) {
      auto access =
          select(merge.getBase().getType(), cast<ArrayAttr>(path), id, &op);
      if (failed(access))
        return failure();
      out << ws << "if (" << operand(when) << ".toBool()) {\n"
          << ws << "  " << operand(merge.getNext()) << access->access << " = "
          << operand(replacement) << ";\n"
          << ws << "  " << operand(merge.getEn()) << " = Bits<1>{1};\n"
          << ws << "}\n";
    }
    return success();
  }

  if (auto nested = dyn_cast<ac::SourceMethodCallOp>(op))
    return invoke(nested, scope, indent);
  if (auto conditional = dyn_cast<ac::SourceBranchOp>(op))
    return branch(conditional, scope, indent);
  if (auto query = dyn_cast<ac::SourceMethodGuardQueryOp>(op)) {
    auto call = scope.method->calls.find(query.getOccurrence())->second;
    const auto &bound = plan.target({id, call});
    if (bound.queue) {
      auto ordinal = plan.queues.find({bound.receiver, bound.queue})->second;
      auto check = bound.action == ac::NativeQueueAction::Push ? ".CanPush()"
                                                               : ".CanPop()";
      return scalar("Bits<1>{model." + storage(ordinal) + check + "}");
    }
    std::string expression = "Bits<1>{model." +
                             numberName("module_", bound.receiver) + "." +
                             function(bound.method, true) + "(sample";
    for (auto value : query.getArguments())
      expression += ", " + operand(value);
    return scalar(expression + ")}");
  }
  auto sample = scope.method && !scope.guard ? "txn.sample" : "sample";
  if (auto read = dyn_cast<ac::SourceStateReadOp>(op)) {
    auto ordinal = plan.states.find({id, read.getStateAttr()})->second;
    auto access = select(plan.resources[ordinal].type, read.getSelectors(), id,
                         &op, &scope, read.getIndices());
    if (failed(access))
      return failure();
    std::string expression = "model." + storage(ordinal) +
                             (scope.guard ? ".Q()" : ".Read(txn)") +
                             access->access;
    if (access->bounds.empty())
      return scalar(expression);
    auto result = type(read.getValue().getType(), id, &op);
    if (failed(result))
      return failure();
    if (!access->sliceStart.empty())
      expression = "TableSlice<" + *result + ">(" + expression + ", " +
                   access->sliceStart + ")";
    return scalar("[&] { if (!" + operand(read.getPath()) +
                  ".toBool()) return " + *result + "{}; if (!(" +
                  join(access->bounds, " && ") + ")) { " + sample +
                  ".Fail(\"out-of-range state read\"); return " + *result +
                  "{}; } return " + expression + "; }()");
  }
  if (auto write = dyn_cast<ac::SourceStateWriteOp>(op)) {
    if (!write.getIndices().empty() &&
        failed(plan.hardware.verifyResolvedOperation(write, plan.bindings(id))))
      return failure();
    auto ordinal = plan.states.find({id, write.getStateAttr()})->second;
    auto access = select(plan.resources[ordinal].type, write.getSelectors(), id,
                         &op, &scope, write.getIndices());
    if (failed(access))
      return failure();
    out << ws << "if (" << operand(write.getPath()) << ".toBool()) {\n";
    std::string body = ws + "  ";
    if (!access->bounds.empty()) {
      out << body << "if (!(" << join(access->bounds, " && ") << ")) " << sample
          << ".Fail(\"out-of-range state write\");\n"
          << body << "else {\n";
      body += "  ";
    }
    auto low = std::to_string(access->low) + "ULL";
    for (const auto &offset : access->offsets)
      low += " + " + offset;
    auto candidate = "candidate" + access->access;
    auto value = operand(write.getValue());
    out << body << "const auto low = " << low << ";\n"
        << body << "model." << storage(ordinal) << ".Write(txn, low, low + "
        << access->width << "ULL, [&](auto &candidate) { ";
    if (access->sliceStart.empty())
      out << candidate << " = " << value;
    else
      out << "std::copy(" << value << ".elements.begin(), " << value
          << ".elements.end(), " << candidate << ".elements.begin() + ("
          << access->sliceStart << "))";
    out << "; });\n";
    if (!access->bounds.empty())
      out << ws << "  }\n";
    out << ws << "}\n";
    return success();
  }
  if (auto check = dyn_cast<ac::SourceExpectOp>(op)) {
    if (check.getKind() == "guard_known")
      return success();
    if (check.getKind() != "assert" && check.getKind() != "range")
      return check.emitOpError() << "unsupported t-gfsim check";
    auto message = op.getAttrOfType<StringAttr>("ac.message");
    out << ws << "if (" << operand(check.getPath()) << ".toBool() && !"
        << operand(check.getCondition()) << ".toBool()) " << sample << ".Fail("
        << quote(message ? message.getValue() : check.getKind()) << ");\n";
    return success();
  }
  if (auto observe = dyn_cast<ac::SourceObserveOp>(op)) {
    auto spec = observe.getSpec();
    auto event = spec.getAs<StringAttr>("event");
    if (!event)
      event = spec.getAs<StringAttr>("name");
    auto items = spec.getAs<ArrayAttr>("items");
    std::string text = "std::string{}";
    if (items) {
      for (auto raw : items) {
        auto row = cast<DictionaryAttr>(raw);
        auto kind = row.getAs<StringAttr>("kind");
        if (kind && kind.getValue() == "value") {
          auto ordinal = row.getAs<IntegerAttr>("ordinal");
          text += " + Format(" +
                  operand(observe.getValues()[ordinal.getInt()]) + ")";
        } else if (auto string = row.getAs<StringAttr>("text")) {
          text += " + " + quote(string.getValue());
        } else {
          return observe.emitOpError()
                 << "unsupported t-gfsim observation item";
        }
      }
    } else {
      for (auto value : observe.getValues())
        text += " + Format(" + operand(value) + ")";
    }
    out << ws << "if (" << operand(observe.getPath()) << ".toBool()) " << sample
        << ".observations.push_back({"
        << quote(event ? event.getValue() : observe.getKind()) << ", " << text
        << "});\n";
    return success();
  }
  return op.emitOpError() << "unsupported t-gfsim operation";
}

LogicalResult Cpp::registration(ac::SourceMethodCallOp call, Expr &scope) {
  const auto &bound = plan.target({scope.owner, call});
  std::string receiver = scope.base + numberName("module_", bound.receiver) + ".";
  auto method = bound.method;
  auto &live = plan.methods.find(method)->second;
  SmallVector<std::string> guardArg, bodyArg;
  for (auto [index, value] : llvm::enumerate(call.getArguments())) {
    auto reference = ref(scope, value);
    bodyArg.push_back(reference);
    if (live.guardArguments.contains(
            method.getGuard().front().getArgument(index)))
      guardArg.push_back(reference);
  }
  for (Value result : call.getResults())
    if (!plan.storedNets.contains({scope.owner, result}))
      out << "  " << ref(scope, result) << " = {};\n";
  out << "  if (!reset";
  if (call.getPath())
    out << " && " << ref(scope, call.getPath()) << ".toBool()";
  out << " && " << receiver << function(method, true) << "(sample";
  for (const auto &arg : guardArg)
    out << ", " << arg;
  out << ")) { // "
      << call->getAttrOfType<StringAttr>("ac.registration_name").getValue()
      << '\n';
  unsigned transactionId = nextLocal++;
  out << "    Transaction txn{sample, " << transactionId << "};\n"
      << "    ";
  if (call.getNumResults())
    out << "auto results = ";
  out << receiver << function(method, false) << "(txn";
  for (const auto &arg : bodyArg)
    out << ", " << arg;
  out << ");\n";
  for (auto [index, result] : llvm::enumerate(call.getResults())) {
    std::string value = numberName("std::get<", index) + ">(results)";
    auto held = plan.storedNets.find({scope.owner, result});
    if (held == plan.storedNets.end()) {
      out << "    " << ref(scope, result) << " = " << value << ";\n";
    } else {
      auto w = width(result.getType(), scope.owner, call);
      if (failed(w))
        return failure();
      out << "    " << scope.base << storage(held->second) << ".Write(txn, 0, "
          << *w << ", [&](auto &candidate) { candidate = " << value << "; });\n";
    }
  }
  out << "  }\n";
  return success();
}

LogicalResult Cpp::work(const WorkSegment &segment, StringRef qualifier,
                        bool edge) {
  auto id = segment.instance;
  const auto &instance = plan.instances.at(id);
  Expr scope(id, qualifier);
  auto operations =
      ArrayRef(instance.work).slice(segment.begin, segment.end - segment.begin);
  for (auto ordinal : operations) {
    if (coneNodes.contains(ordinal) ||
        (inlineWork && plan.edgeWork.contains(ordinal) != edge))
      continue;
    const auto &node = plan.nodes[ordinal];
    unsigned indent = edge ? 6 : cacheWork ? 4 : 2;
    auto deferred = plan.deferredWrites.find(ordinal);
    if (auto call = dyn_cast<ac::SourceMethodCallOp>(node.site.second)) {
      if (inlineWork && deferred != plan.deferredWrites.end()) {
        for (auto [result, source] :
             llvm::zip_equal(call.getResults(), deferred->second.outputs)) {
          auto t = type(result.getType(), id, call);
          if (failed(t))
            return failure();
          out << std::string(indent, ' ');
          if (auto forwarded = forwardedResults.find(plan.projection(source).net);
              forwarded != forwardedResults.end() &&
              forwarded->second == Net{id, result})
            out << "if (reset) " << ref(scope, result) << " = {};\n";
          else
            out << ref(scope, result) << " = reset ? " << *t << "{} : "
                << slot(source) << ";\n";
        }
      } else if (failed(registration(call, scope))) {
        return failure();
      }
    } else if (failed(emitNode(node, scope, indent))) {
      return failure();
    }
  }
  return success();
}

LogicalResult Cpp::sampleWork() {
  if (cacheWork) {
    out << "  if (!workValid || cachedReset != reset";
    for (auto [index, value] : llvm::enumerate(cacheInputs))
      out << "\n      || !SameValue(cached_" << index << ", "
          << slot({0, value}) << ')';
    out << ") {\n";
    if (!cone.nodes.empty()) {
      auto key = projectedSlot(cone.input);
      out << "    if (!coneValid || !SameValue(coneInput, " << key << ")) {\n";
      for (unsigned index : cone.nodes) {
        const Node &node = plan.nodes[index];
        Expr scope(node.site.first, "");
        if (failed(emitNode(node, scope, 6)))
          return failure();
      }
      out << "      coneInput = " << key << ";\n"
          << "      coneValid = true;\n"
          << "    }\n";
    }
  }
  for (const auto &segment : plan.schedule) {
    if (inlineWork) {
      if (failed(work(segment, "")))
        return failure();
    } else {
      out << "  module_" << segment.instance << '.'
          << workName(plan.instances.at(segment.instance), segment.ordinal)
          << "(sample, reset);\n";
    }
  }
  if (cacheWork) {
    for (auto [index, value] : llvm::enumerate(cacheInputs))
      out << "    cached_" << index << " = " << slot({0, value}) << ";\n";
    out << "    cachedReset = reset;\n"
        << "    workValid = true;\n"
        << "  }\n";
  }
  return success();
}

LogicalResult Cpp::xfer() {
  if (!inlineWork || plan.deferredWrites.empty()) {
    for (unsigned index = 0; index < plan.resources.size(); ++index)
      out << "    " << storage(index) << ".Xfer(reset);\n";
    return success();
  }

  out << "    if (!reset) {\n";
  for (const auto &segment : plan.schedule)
    if (failed(work(segment, "", /*edge=*/true)))
      return failure();
  SmallVector<const DeferredWrite *> writes(plan.resources.size());
  for (const auto &entry : plan.deferredWrites)
    writes[entry.second.resource] = &entry.second;
  for (auto [index, write] : llvm::enumerate(writes)) {
    if (!write)
      continue;
    // Hold a value copy when the source refers to Q, which another Xfer
    // can replace before this State receives its value.
    Net source = plan.projection(write->value).net;
    bool snapshot = plan.resources.size() > 1 &&
                    (plan.storedNets.contains(source) ||
                     borrowedSlots.contains(source));
    out << "      const auto " << (snapshot ? "" : "&") << "proposal_" << index
        << " = " << slot(write->value) << ";\n";
  }
  for (auto [index, write] : llvm::enumerate(writes)) {
    out << "      " << storage(index);
    if (write)
      out << ".XferWhole(proposal_" << index << ");\n";
    else
      out << ".Xfer(false);\n";
  }
  out << "    } else {\n";
  for (unsigned index = 0; index < plan.resources.size(); ++index)
    out << "      " << storage(index) << ".Xfer(true);\n";
  out << "    }\n";
  if (cacheWork)
    out << "    workValid = false;\n";
  return success();
}

void Cpp::rootOutputBindings(const Instance &root) {
  auto definition = root.info.definition;
  auto names = definition.getOutputNames();
  for (auto [index, net] : llvm::enumerate(root.outputs))
    out << (index ? ", " : " : ")
        << portName("out_", cast<StringAttr>(names[index])) << '(' << slot(net)
        << ')';
}

LogicalResult Cpp::emitModel(ArrayAttr samples) {
  const auto &root = plan.instances.at(0);
  // Root input names denote the owning slots. Add their bindings before any
  // submodule is initialized so forwarded outputs can alias them directly.
  auto mod = root.info.definition;
  for (auto [index, net] : llvm::enumerate(root.inputs))
    inputs.try_emplace(
        net.second,
        portName("in_", cast<StringAttr>(mod.getInputNames()[index])));

  out << "class Model {\n";
  for (const auto &[id, instance] : plan.instances) {
    if (!needsModule(id, instance))
      continue;
    out << "  struct Module_" << id << " { // " << instance.path << '\n';
    out << "    Model &model;\n";
    if (!inlineWork)
      for (unsigned ordinal = 0; ordinal < instance.workSegments; ++ordinal)
        out << "    void " << workName(instance, ordinal)
            << "(Sample &sample, bool reset);\n";
    for (auto method : instance.methods) {
      if (!needsMethod(id, method))
        continue;
      for (bool guard : {true, false}) {
        auto sig = signature(method, id, guard);
        if (failed(sig))
          return failure();
        out << "    " << *sig << "; // " << method.getSymName() << '\n';
      }
    }
    out << "  };\n";
  }
  out << "public:\n";
  for (auto [index, net] : llvm::enumerate(root.inputs))
    out << "  " << *type(net.second.getType(), 0, mod) << ' '
        << inputs.find(net.second)->second << "{};\n";

  out << "private:\n";
  for (unsigned index : plan.fixed) {
    const Node &node = plan.nodes[index];
    auto [id, op] = node.site;
    if (op->getNumResults() != 1 ||
        !constantSlots.contains({id, op->getResult(0)}))
      continue;
    Expr scope(id, "");
    scope.constant = true;
    if (failed(emitNode(node, scope, 2)))
      return failure();
  }
  for (auto [index, net] : llvm::enumerate(plan.slots)) {
    if ((net.first == 0 && inputs.contains(net.second)) ||
        localSlots.contains(net) || borrowedSlots.contains(net) ||
        forwardedResults.contains(net) || constantSlots.contains(net))
      continue;
    out << "  "
        << *type(net.second.getType(), net.first, plan.module(net.first))
        << " slot_" << index << "{};\n";
  }
  for (auto [index, resource] : llvm::enumerate(plan.resources)) {
    auto t = *type(resource.type, resource.owner, plan.module(resource.owner));
    out << "  " << (resource.kind == Resource::Queue ? "Queue" : "State") << '<'
        << t << "> " << storage(index) << '{' << quote(resource.name);
    if (resource.kind == Resource::Queue)
      out << ", " << resource.depth << "ULL, " << resource.latency << "ULL";
    out << "};\n";
  }
  for (const auto &[id, instance] : plan.instances)
    if (needsModule(id, instance))
      out << "  Module_" << id << " module_" << id << "{*this};\n";
  out << "  std::string initializationError;\n"
      << "  bool previousClock = false;\n";
  if (cacheWork) {
    out << "  bool workValid = false;\n"
        << "  bool cachedReset = false;\n";
    for (auto [index, value] : llvm::enumerate(cacheInputs))
      out << "  decltype(" << slot({0, value}) << ") cached_" << index
          << "{};\n";
    if (!cone.nodes.empty())
      out << "  bool coneValid = false;\n"
          << "  std::remove_cvref_t<decltype((" << projectedSlot(cone.input)
          << "))> coneInput{};\n";
  }
  out << "public:\n";
  for (auto [index, net] : llvm::enumerate(root.outputs))
    out << "  const " << *type(net.second.getType(), 0, mod) << " &"
        << portName("out_", cast<StringAttr>(mod.getOutputNames()[index]))
        << ";\n";
  out << "  std::string error;\n"
      << "  std::vector<Observation> observations;\n"
      << "  Model()";
  rootOutputBindings(root);
  out << " {\n    Sample sample;\n";
  for (auto index : plan.fixed) {
    const auto &node = plan.nodes[index];
    auto [id, op] = node.site;
    if (op->getNumResults() == 1 &&
        constantSlots.contains({id, op->getResult(0)}))
      continue;
    Expr scope(node.site.first, "");
    if (failed(emitNode(node, scope, 4)))
      return failure();
  }
  for (auto [index, resource] : llvm::enumerate(plan.resources))
    if (resource.kind == Resource::State)
      out << "    " << storage(index) << ".Initialize("
          << slot(resource.initial) << ");\n";
  out << "    initializationError = std::move(sample.error);\n"
      << "  }\n"
      << "  Model(const Model &) = delete;\n"
      << "  Model &operator=(const Model &) = delete;\n"
      << "  bool Step();\n";
  if (!samples.empty()) {
    out << "  bool Run() {\n    for (auto row : {";
    SmallVector<std::string> clocks;
    for (Attribute raw : samples)
      clocks.push_back(quote(cast<StringAttr>(raw).getValue()));
    out << join(clocks) << "}) {\n"
        << "      " << slot(plan.clock) << " = Bits<1>{row[0] == '1'};\n"
        << "      " << slot(plan.reset) << " = Bits<1>{row[1] == '1'};\n"
        << "      if (!Step()) return false;\n    }\n    return true;\n  }\n";
  }
  out << "};\n";
  return success();
}

LogicalResult Cpp::emit() {
  auto samples = ac::getSystemSamples(plan.module(0));
  if (failed(samples))
    return failure();
  for (auto raw : *samples)
    if (cast<StringAttr>(raw).getValue().find_first_of("xz") != StringRef::npos)
      return plan.module(0)->emitOpError()
             << "X/Z System samples are unsupported";
  if (!samples->empty() && (!plan.clock.second || !plan.reset.second))
    return plan.module(0)->emitOpError()
           << "System samples require root clock and reset";

  out << "#pragma once\n#include \"gfsim/t_gfsim.h\"\n#include <tuple>\n\n"
      << "namespace gfsim::t_gfsim {\n";
  if (failed(planTypes()) || failed(emitModel(*samples)))
    return failure();
  for (const auto &[id, instance] : plan.instances) {
    for (auto method : instance.methods) {
      if (!needsMethod(id, method))
        continue;
      for (bool guard : {true, false}) {
        Expr scope(id, "model.");
        auto sig = signature(method, id, guard, &scope);
        if (failed(sig))
          return failure();
        out << "\ninline " << *sig << " {\n";
        Block &bbb = (guard ? method.getGuard() : method.getBody()).front();
        if (failed(block(bbb, scope, 2)))
          return failure();
        auto returned = cast<ac::YieldOp>(bbb.back()).getValues();
        if (guard)
          out << "  return "
              << (returned.empty() ? "true"
                                   : ref(scope, returned.front()) + ".toBool()")
              << ";\n";
        else if (!returned.empty()) {
          SmallVector<std::string> parts;
          for (auto value : returned)
            parts.push_back(ref(scope, value));
          out << "  return {" << join(parts) << "};\n";
        }
        out << "}\n";
      }
    }
  }
  if (!inlineWork)
    for (const auto &segment : plan.schedule) {
      out << "inline void Model::Module_" << segment.instance << "::"
          << workName(plan.instances.at(segment.instance), segment.ordinal)
          << "([[maybe_unused]] Sample &sample, [[maybe_unused]] "
             "bool reset) {\n";
      if (failed(work(segment, "model.")))
        return failure();
      out << "}\n";
    }
  bool pureWork = inlineWork && plan.pureWork;
  out << "\ninline bool Model::Step() {\n";
  if (!pureWork)
    out << "  Sample sample;\n";
  out << "  if (!initializationError.empty()) { error = initializationError; "
         "return false; }\n"
      << "  [[maybe_unused]] const bool reset = "
      << (plan.reset.second ? slot(plan.reset) + ".toBool()" : "false") << ";\n"
      << "  const bool clock = "
      << (plan.clock.second ? slot(plan.clock) + ".toBool()" : "true") << ";\n";
  for (auto [index, net] : llvm::enumerate(plan.slots))
    if (localSlots.contains(net))
      out << "  [[maybe_unused]] "
          << *type(net.second.getType(), net.first, plan.module(net.first))
          << " slot_" << index << "{};\n";
  if (failed(sampleWork()))
    return failure();
  if (!pureWork)
    out << "  error = std::move(sample.error);\n";
  out << "  if (" << (pureWork ? "" : "error.empty() && ")
      << "clock && !previousClock) {\n";
  if (failed(xfer()))
    return failure();
  if (!pureWork) {
    out << "  } else {\n";
    for (unsigned index = 0; index < plan.resources.size(); ++index)
      out << "    " << storage(index) << ".Discard();\n";
  }
  out << "  }\n";
  out << (pureWork ? "  error.clear();\n"
                   : "  if (!error.empty()) return false;\n")
      << "  previousClock = clock;\n"
      << (pureWork ? "  observations.clear();\n"
                   : "  observations = std::move(sample.observations);\n")
      << "  return true;\n}\n"
      << "} // namespace gfsim::t_gfsim\n";
  return success();
}

} // namespace
} // namespace linked

LogicalResult emitLinkedCpp(ModuleOp package, llvm::raw_ostream &out) {
  if (!package->hasAttr("ac.linked_inferred_guards"))
    return package.emitOpError()
           << "t-gfsim requires inferred linked Method guards";
  if (failed(verifyLinkedSourceACIR(package)))
    return failure();
  linked::Plan plan(package);
  if (failed(plan.build()))
    return failure();
  return linked::Cpp(plan, out).emit();
}

} // namespace acir::compiler
