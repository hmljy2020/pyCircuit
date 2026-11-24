#include "Compiler/LinkedSourceACIR.h"
#include "mlir/Dialect/Arith/IR/Arith.h"
#include "mlir/Dialect/Func/IR/FuncOps.h"
#include "mlir/Parser/Parser.h"
#include "pycircuit/Dialect/ACIR/ACIRDialect.h"
#include "pycircuit/Dialect/ACIR/ACIROps.h"
#include "pycircuit/Dialect/ACIR/SourceMethodAnalysis.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/StringSwitch.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/raw_ostream.h"

using namespace mlir;
namespace ac = acir::ac;

namespace {
bool isProvenance(StringRef name) {
  return llvm::StringSwitch<bool>(name)
      .Cases({"origin", "location", "source_location", "callee_site",
              "ac.origin", "ac.location", "ac.guard_location",
              "ac.condition_location", "source_owner", "ac.source_owner",
              "ac.source_units", "ac.declaration_role", "dependency_summary",
              "transaction_summary"},
             true)
      .Default(false);
}

bool isFixedBitsConstraint(DictionaryAttr constraint) {
  auto kind =
      constraint ? constraint.getAs<StringAttr>("source_kind") : StringAttr();
  return kind && kind.getValue() == "fixed_bits";
}

bool showAttribute(NamedAttribute attr, ArrayRef<StringRef> handled) {
  StringRef name = attr.getName().getValue();
  if (isProvenance(name) || llvm::is_contained(handled, name))
    return false;
  auto array = dyn_cast<ArrayAttr>(attr.getValue());
  if (name == "ac.payload_constraint")
    return !isFixedBitsConstraint(dyn_cast<DictionaryAttr>(attr.getValue()));
  if (name == "ac.result_constraints" && array)
    return !llvm::all_of(array, [](Attribute row) {
      return isFixedBitsConstraint(dyn_cast<DictionaryAttr>(row));
    });
  if (name == "ac.parameters" && array)
    return !llvm::all_of(array, [](Attribute raw) {
      auto row = dyn_cast<DictionaryAttr>(raw);
      if (!row)
        return false;
      auto defaultValue = row.getAs<DictionaryAttr>("default");
      auto present =
          defaultValue ? defaultValue.getAs<BoolAttr>("present") : BoolAttr();
      return present && !present.getValue() &&
             isFixedBitsConstraint(row.getAs<DictionaryAttr>("constraint"));
    });
  if (array && array.empty())
    return !llvm::StringSwitch<bool>(name)
                .Cases({"argument_names", "input_names", "output_names",
                        "instance_path", "parameters", "type_parameters",
                        "type_arguments", "ac.result_availability"},
                       true)
                .Default(false);
  return true;
}

class LinkedPrinter {
public:
  explicit LinkedPrinter(llvm::raw_ostream &out) : out(out) {}

  void print(ModuleOp package) {
    package.walk([&](ac::SourceMethodCallOp call) {
      if (auto method = call->getParentOfType<ac::SourceMethodOp>())
        callIds.try_emplace(
            std::pair{method.getOperation(), call.getOccurrence()},
            callIds.size() + 1);
    });
    out << "linked-source {\n";
    for (Operation &op : package.getBody()->getOperations())
      printOperation(op, 2);
    out << "}\n";
  }

private:
  template <typename Range, typename Print>
  void separated(Range &&range, StringRef separator, Print print) {
    llvm::interleave(range, out, print, separator);
  }

  void printValue(Value value) {
    out << '%' << values.try_emplace(value, values.size()).first->second;
  }

  void printType(Type type) {
    if (auto bits = dyn_cast<ac::BitsType>(type)) {
      out << "bits<";
      printAttribute(bits.getWidth());
      out << '>';
    } else if (auto table = dyn_cast<ac::TableType>(type)) {
      out << "table<";
      printAttribute(table.getShape());
      out << ", ";
      printType(table.getElementType());
      out << '>';
    } else if (auto fn = dyn_cast<FunctionType>(type)) {
      out << '(';
      separated(fn.getInputs(), ", ", [&](Type arg) { printType(arg); });
      out << ") -> (";
      separated(fn.getResults(), ", ", [&](Type result) { printType(result); });
      out << ')';
    } else {
      out << type;
    }
  }

  void printExpr(DictionaryAttr tree) {
    auto kind = tree.getAs<StringAttr>("kind").getValue();
    if (kind == "literal") {
      auto value = tree.getAs<DictionaryAttr>("value");
      if (isa_and_nonnull<ac::MathIntAttr, BoolAttr>(value.get("value")))
        return printAttribute(value.get("value"));
      return printAttribute(value);
    }
    if (kind == "reference") {
      auto ref = tree.getAs<DictionaryAttr>("ref");
      if (auto symbol = ref.getAs<FlatSymbolRefAttr>("symbol")) {
        out << symbol;
        return;
      }
      if (auto name = ref.getAs<StringAttr>("name")) {
        out << name.getValue();
        return;
      }
    }
    if (kind == "binary" || kind == "unary") {
      out << '(';
      if (kind == "binary") {
        printAttribute(tree.get("lhs"));
        out << ' ';
      }
      out << tree.getAs<StringAttr>("operator").getValue() << ' ';
      printAttribute(tree.get(kind == "binary" ? "rhs" : "operand"));
      out << ')';
      return;
    }
    printAttribute(tree);
  }

  void printAttribute(Attribute attr) {
    if (auto number = dyn_cast<ac::MathIntAttr>(attr)) {
      out << number.getCanonicalValue();
    } else if (auto expr = dyn_cast<ac::StaticExprAttr>(attr)) {
      printExpr(expr.getTree());
    } else if (auto type = dyn_cast<TypeAttr>(attr)) {
      printType(type.getValue());
    } else if (auto list = dyn_cast<ArrayAttr>(attr)) {
      out << '[';
      separated(list, ", ", [&](Attribute item) { printAttribute(item); });
      out << ']';
    } else if (auto dict = dyn_cast<DictionaryAttr>(attr)) {
      if (dict.get("expansion") && dict.get("site")) {
        out << "site@"
            << sites.try_emplace(dict, sites.size() + 1).first->second;
        return;
      }
      printAttributes(dict.getValue());
    } else {
      out << attr;
    }
  }

  void printAttributes(ArrayRef<NamedAttribute> attrs,
                       ArrayRef<StringRef> handled = {}) {
    out << '{';
    bool first = true;
    for (auto attr : attrs) {
      StringRef name = attr.getName().getValue();
      if (!showAttribute(attr, handled))
        continue;
      if (!first)
        out << ", ";
      first = false;
      out << name;
      if (!isa<UnitAttr>(attr.getValue())) {
        out << " = ";
        printAttribute(attr.getValue());
      }
    }
    out << '}';
  }

  void printValues(ValueRange range) {
    separated(range, ", ", [&](Value value) { printValue(value); });
  }

  unsigned callId(Operation *op, DictionaryAttr occurrence) {
    auto method = op->getParentOfType<ac::SourceMethodOp>();
    if (!method)
      return 0;
    return callIds.lookup({method.getOperation(), occurrence});
  }

  bool isVisibleArg(BlockArgument arg, ac::SourceMethodOp method) {
    unsigned ordinal = arg.getArgNumber();
    return !method || ordinal < method.getFunctionType().getNumInputs() ||
           !method.getGuard().front().getArgument(ordinal).use_empty() ||
           !method.getBody().front().getArgument(ordinal).use_empty();
  }

  void printRegion(Region &region, unsigned indent, StringRef label,
                   ac::SourceMethodOp method) {
    if (!label.empty())
      out << ' ' << label;
    out << " {\n";
    for (Block &block : region) {
      auto args =
          llvm::make_filter_range(block.getArguments(), [&](BlockArgument arg) {
            return isVisibleArg(arg, method);
          });
      if (!region.hasOneBlock() || args.begin() != args.end()) {
        out.indent(indent + 2)
            << '^'
            << blockIds.try_emplace(&block, blockIds.size()).first->second
            << '(';
        separated(args, ", ", [&](BlockArgument arg) {
          printValue(arg);
          out << ": ";
          printType(arg.getType());
        });
        out << "):\n";
      }
      for (Operation &op : block)
        printOperation(op, indent + 2);
    }
    out.indent(indent) << '}';
  }

  void printOperation(Operation &op, unsigned indent) {
    if (isa<ac::BitsConstantOp, ac::BitsConstant4Op>(op) &&
        op.getResult(0).use_empty())
      return;
    out.indent(indent);
    if (op.getNumResults()) {
      printValues(op.getResults());
      out << " = ";
    }
    out << op.getName().getStringRef();
    auto method = dyn_cast<ac::SourceMethodOp>(op);
    SmallVector<StringRef> handled;
    if (auto call = dyn_cast<ac::SourceMethodCallOp>(op)) {
      if (unsigned id = callId(&op, call.getOccurrence()))
        out << " @" << id;
      out << ' ' << call.getCalleeAttr() << '(';
      printValues(call.getArguments());
      out << ')';
      if (Value path = call.getPath(); path && !ac::isSourceLiteralTrue(path)) {
        out << " when ";
        printValue(path);
      }
      handled = {"callee", "occurrence", "ac.implicit_path"};
    } else if (auto query = dyn_cast<ac::SourceMethodGuardQueryOp>(op)) {
      out << " @" << callId(&op, query.getOccurrence()) << '(';
      printValues(query.getArguments());
      out << ')';
      handled = {"occurrence"};
    } else if (method) {
      out << " @" << method.getSymName();
      unsigned offset = method.getFunctionType().getNumInputs();
      SmallVector<Value> captures;
      for (auto [index, capture] : llvm::enumerate(method.getCaptures()))
        if (isVisibleArg(method.getGuard().front().getArgument(offset + index),
                         method))
          captures.push_back(capture);
      if (!captures.empty()) {
        out << " captures(";
        printValues(captures);
        out << ')';
      }
      handled = {"sym_name", "occurrence"};
    } else if (isa<ac::ModuleOp, ac::ModuleImportOp>(op)) {
      out << " @" << op.getAttrOfType<StringAttr>("sym_name").getValue();
      handled = {"sym_name"};
    } else if (op.getNumOperands()) {
      out << '(';
      printValues(op.getOperands());
      out << ')';
    } else if (isa<ac::YieldOp>(op)) {
      auto owner = dyn_cast_or_null<ac::SourceMethodOp>(op.getParentOp());
      if (owner && op.getParentRegion() == &owner.getGuard())
        out << "(true)";
    }
    if (llvm::any_of(op.getAttrs(), [&](NamedAttribute attr) {
          return showAttribute(attr, handled);
        })) {
      out << ' ';
      printAttributes(op.getAttrs(), handled);
    }
    if (op.getNumResults()) {
      out << " : ";
      separated(op.getResultTypes(), ", ", [&](Type type) { printType(type); });
    }
    for (Block *successor : op.getSuccessors())
      out << " ^"
          << blockIds.try_emplace(successor, blockIds.size()).first->second;
    for (auto [index, region] : llvm::enumerate(op.getRegions())) {
      StringRef label = method ? (index == 0 ? "guard" : "body") : "";
      if (isa<ac::SourceBranchOp>(op))
        label = index == 0 ? "then" : "else";
      printRegion(region, indent, label, method);
    }
    out << '\n';
  }

  llvm::raw_ostream &out;
  llvm::DenseMap<Value, unsigned> values;
  llvm::DenseMap<Block *, unsigned> blockIds;
  llvm::DenseMap<Attribute, unsigned> sites;
  llvm::DenseMap<std::pair<Operation *, DictionaryAttr>, unsigned> callIds;
};
} // namespace

int main(int argc, char **argv) {
  namespace cl = llvm::cl;
  cl::opt<std::string> input(cl::Positional, cl::Required,
                             cl::desc("<linked.ac>"));
  cl::ParseCommandLineOptions(argc, argv, "Compact linked-source ACIR view\n");
  DialectRegistry dialects;
  dialects.insert<ac::ACIRDialect, arith::ArithDialect, func::FuncDialect>();
  MLIRContext context(dialects);
  auto package = parseSourceFile<ModuleOp>(input.getValue(), &context);
  if (!package || failed(acir::compiler::verifyLinkedSourceACIR(*package)))
    return 1;
  LinkedPrinter(llvm::outs()).print(*package);
  return 0;
}
