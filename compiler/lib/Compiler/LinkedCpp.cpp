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

std::string literal(const llvm::APInt &value, unsigned width) {
  auto resized = value.zextOrTrunc(width);
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
  bool guard = false;
};

struct Selection {
  std::string access;
  uint64_t low;
  uint64_t width;
};

class Cpp {
public:
  Cpp(const Plan &plan, llvm::raw_ostream &out) : plan(plan), out(out) {}
  LogicalResult emit();

private:
  LogicalResult planTypes();
  FailureOr<std::string> type(Type type, InstanceId id, Operation *site);
  FailureOr<unsigned> width(Type type, InstanceId id, Operation *site);
  FailureOr<Selection> select(Type type, ArrayAttr selectors, InstanceId id,
                              Operation *site);
  FailureOr<std::string> field(Type &type, StringRef name, InstanceId id,
                               Operation *site, uint64_t *low = nullptr);

  std::string slot(Net net) const;
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
  LogicalResult registration(ac::SourceMethodCallOp op, Expr &scope);
  LogicalResult invoke(ac::SourceMethodCallOp call, Expr &scope,
                       unsigned indent);
  LogicalResult branch(ac::SourceBranchOp op, Expr &scope, unsigned indent);
  LogicalResult work(const WorkSegment &segment);
  LogicalResult emitModel(ArrayAttr samples);
  void ports(InstanceId id, const Instance &instance);
  void portBindings(const Instance &instance, bool root);

  const Plan &plan;
  llvm::raw_ostream &out;
  llvm::DenseMap<Type, std::string> nominals;
  llvm::DenseMap<Value, std::string> inputs;
  llvm::DenseMap<Operation *, unsigned> methodIds;
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

FailureOr<std::string> Cpp::type(Type t, InstanceId id, Operation *site) {
  auto actual = plan.hardware.resolveType(t, plan.bindings(id), site);
  if (failed(actual))
    return failure();
  if (isa<ac::BitsType>(*actual)) {
    auto w = width(*actual, id, site);
    if (failed(w))
      return failure();
    return bits(*w);
  }
  if (auto found = nominals.find(*actual); found != nominals.end())
    return found->second;

  std::string name = numberName("Record_", nominals.size());
  if (auto record = dyn_cast<ac::StructType>(*actual)) {
    auto definition = plan.hardware.lookupStruct(record);
    SmallVector<std::string> fields;
    for (auto raw : definition.getFields()) {
      auto fieldType =
          cast<DictionaryAttr>(raw).getAs<TypeAttr>("type").getValue();
      auto fieldCpp = type(fieldType, id, site);
      if (failed(fieldCpp))
        return failure();
      fields.push_back(*fieldCpp);
    }
    name = numberName("Record_", nominals.size());
    nominals.try_emplace(*actual, name);
    out << "struct " << name << " { // " << record.getName().getValue() << '\n';
    for (auto [i, member] : llvm::enumerate(fields))
      out << "  " << member << " field_" << i << "{};\n";
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
    nominals.try_emplace(*actual, name);
    out << "struct " << name << " { " << bits(definition->width)
        << " code{}; }; // " << enumeration.getName().getValue() << '\n'
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
                                 Operation *site) {
  Selection selected{"", 0, 0};
  for (auto raw : selectors) {
    auto row = dyn_cast<DictionaryAttr>(raw);
    auto name =
        row ? row.getAs<StringAttr>("field") : dyn_cast<StringAttr>(raw);
    if (!name)
      return site->emitOpError()
             << "t-gfsim supports only static Struct state selections";
    auto member = field(t, name.getValue(), id, site, &selected.low);
    if (failed(member))
      return failure();
    selected.access += *member;
  }
  auto size = width(t, id, site);
  if (failed(size))
    return failure();
  selected.width = *size;
  return selected;
}

LogicalResult Cpp::planTypes() {
  auto need = [&](Value value, InstanceId id, Operation *site) {
    return success(succeeded(type(value.getType(), id, site)));
  };
  for (const auto &resource : plan.resources)
    if (failed(
            type(resource.type, resource.owner, plan.module(resource.owner))))
      return failure();
  for (auto [id, value] : plan.slots)
    if (failed(need(value, id, plan.module(id))))
      return failure();
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
        if (!live.liveGuard.contains(op) && !live.liveBody.contains(op))
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

std::string Cpp::slot(Net net) const {
  net = plan.resolve(net);
  if (auto state = plan.storedNets.find(net); state != plan.storedNets.end())
    return storage(state->second) + ".Q()";
  if (net.first == 0) {
    auto input = inputs.find(net.second);
    if (input != inputs.end())
      return input->second;
  }
  return numberName("slot_", plan.slotIds.find(net)->second);
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
      if (live.contains(argument))
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

LogicalResult Cpp::statement(Operation &op, Expr &scope, unsigned indent) {
  auto id = scope.owner;
  std::string ws(indent, ' ');
  auto operand = [&](Value value) { return ref(scope, value); };
  auto arg = [&](unsigned index) { return operand(op.getOperand(index)); };
  auto assign = [&](Value value, const std::string &expression) {
    out << ws;
    if (scope.method)
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
    return scalar(literal(llvm::APSInt(value->getCanonicalValue()), *w));
  }
  if (auto constant = dyn_cast<ac::BitsConstant4Op>(op))
    return scalar(literal(
        llvm::APInt(constant.getPattern().size(), constant.getPattern(), 2),
        constant.getPattern().size()));
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
    if (opcode == "udiv" || opcode == "urem") {
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
  if (auto read = dyn_cast<ac::SourceStateReadOp>(op)) {
    auto ordinal = plan.states.find({id, read.getStateAttr()})->second;
    auto access =
        select(plan.resources[ordinal].type, read.getSelectors(), id, &op);
    if (failed(access))
      return failure();
    return scalar("model." + storage(ordinal) +
                  (scope.guard ? ".Q()" : ".Read(txn)") + access->access);
  }
  if (auto write = dyn_cast<ac::SourceStateWriteOp>(op)) {
    auto ordinal = plan.states.find({id, write.getStateAttr()})->second;
    auto access =
        select(plan.resources[ordinal].type, write.getSelectors(), id, &op);
    if (failed(access))
      return failure();
    out << ws << "if (" << operand(write.getPath()) << ".toBool())\n"
        << ws << "  model." << storage(ordinal) << ".Write(txn, " << access->low
        << ", " << (access->low + access->width) << ", [&](auto &candidate) { "
        << "candidate" << access->access << " = " << operand(write.getValue())
        << "; });\n";
    return success();
  }
  auto sample = scope.method && !scope.guard ? "txn.sample" : "sample";
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
  std::string receiver = numberName("model.module_", bound.receiver) + ".";
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
      out << "    model." << storage(held->second) << ".Write(txn, 0, " << *w
          << ", [&](auto &candidate) { candidate = " << value << "; });\n";
    }
  }
  out << "  }\n";
  return success();
}

LogicalResult Cpp::work(const WorkSegment &segment) {
  auto id = segment.instance;
  const auto &instance = plan.instances.at(id);
  out << "inline void Model::Module_" << id
      << "::" << workName(instance, segment.ordinal)
      << "([[maybe_unused]] Sample &sample, [[maybe_unused]] "
         "bool reset) {\n";
  Expr scope(id, "model.");
  auto mod = instance.info.definition;
  for (auto [index, net] : llvm::enumerate(instance.inputs))
    scope.values.try_emplace(
        net.second,
        portName("in_", cast<StringAttr>(mod.getInputNames()[index])));
  auto operations =
      ArrayRef(instance.work).slice(segment.begin, segment.end - segment.begin);
  for (auto ordinal : operations) {
    Operation &op = *plan.nodes[ordinal].site.second;
    if (auto call = dyn_cast<ac::SourceMethodCallOp>(op)) {
      if (failed(registration(call, scope)))
        return failure();
    } else if (failed(statement(op, scope, 2))) {
      return failure();
    }
  }
  out << "}\n";
  return success();
}

void Cpp::ports(InstanceId id, const Instance &instance) {
  auto mod = instance.info.definition;
  auto declare = [&](ArrayRef<Net> ports, ArrayAttr names, StringRef prefix) {
    for (auto [index, net] : llvm::enumerate(ports))
      out << "    const " << *type(net.second.getType(), id, mod) << " &"
          << portName(prefix, cast<StringAttr>(names[index])) << ";\n";
  };
  declare(instance.inputs, mod.getInputNames(), "in_");
  declare(instance.outputs, mod.getOutputNames(), "out_");
}

void Cpp::portBindings(const Instance &instance, bool root) {
  auto mod = instance.info.definition;
  if (!root)
    for (auto net : instance.inputs)
      out << ", " << slot(net);
  for (auto [index, net] : llvm::enumerate(instance.outputs)) {
    if (root)
      out << (index ? ", " : " : ")
          << portName("out_", cast<StringAttr>(mod.getOutputNames()[index]))
          << '(' << slot(net) << ')';
    else
      out << ", " << slot(net);
  }
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
    out << "  struct Module_" << id << " { // " << instance.path << '\n';
    out << "    Model &model;\n";
    ports(id, instance);
    for (unsigned ordinal = 0; ordinal < instance.workSegments; ++ordinal)
      out << "    void " << workName(instance, ordinal)
          << "(Sample &sample, bool reset);\n";
    for (auto method : instance.methods)
      for (bool guard : {true, false}) {
        auto sig = signature(method, id, guard);
        if (failed(sig))
          return failure();
        out << "    " << *sig << "; // " << method.getSymName() << '\n';
      }
    out << "  };\n";
  }
  out << "public:\n";
  for (auto [index, net] : llvm::enumerate(root.inputs))
    out << "  " << *type(net.second.getType(), 0, mod) << ' '
        << inputs.find(net.second)->second << "{};\n";

  out << "private:\n";
  for (auto [index, net] : llvm::enumerate(plan.slots)) {
    if (net.first == 0 && inputs.contains(net.second))
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
  for (const auto &[id, instance] : plan.instances) {
    out << "  Module_" << id << " module_" << id << "{*this";
    portBindings(instance, false);
    out << "};\n";
  }
  out << "  std::string initializationError;\n"
      << "  bool previousClock = false;\n"
      << "public:\n";
  for (auto [index, net] : llvm::enumerate(root.outputs))
    out << "  const " << *type(net.second.getType(), 0, mod) << " &"
        << portName("out_", cast<StringAttr>(mod.getOutputNames()[index]))
        << ";\n";
  out << "  std::string error;\n"
      << "  std::vector<Observation> observations;\n"
      << "  Model()";
  portBindings(root, true);
  out << " {\n    Sample sample;\n";
  for (auto index : plan.fixed) {
    auto [id, operation] = plan.nodes[index].site;
    Expr scope(id, "");
    if (failed(statement(*operation, scope, 4)))
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
  for (const auto &segment : plan.schedule)
    if (failed(work(segment)))
      return failure();
  out << "\ninline bool Model::Step() {\n"
      << "  Sample sample;\n"
      << "  if (!initializationError.empty()) { error = initializationError; "
         "return false; }\n"
      << "  const bool reset = "
      << (plan.reset.second ? slot(plan.reset) + ".toBool()" : "false") << ";\n"
      << "  const bool clock = "
      << (plan.clock.second ? slot(plan.clock) + ".toBool()" : "true") << ";\n";
  for (const auto &segment : plan.schedule)
    out << "  module_" << segment.instance << '.'
        << workName(plan.instances.at(segment.instance), segment.ordinal)
        << "(sample, reset);\n";
  out << "  error = std::move(sample.error);\n"
      << "  if (error.empty() && clock && !previousClock) {\n";
  for (unsigned index = 0; index < plan.resources.size(); ++index)
    out << "    " << storage(index) << ".Xfer(reset);\n";
  out << "  } else {\n";
  for (unsigned index = 0; index < plan.resources.size(); ++index)
    out << "    " << storage(index) << ".Discard();\n";
  out << "  }\n"
      << "  if (!error.empty()) return false;\n"
      << "  previousClock = clock;\n"
      << "  observations = std::move(sample.observations);\n"
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
