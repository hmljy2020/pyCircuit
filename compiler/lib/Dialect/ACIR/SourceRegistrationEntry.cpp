#include "pycircuit/Dialect/ACIR/SourceMethodAnalysis.h"
#include "mlir/IR/SymbolTable.h"
#include "llvm/ADT/STLExtras.h"

using namespace mlir;
namespace acir::ac {
bool isSourceLiteralTrue(Value input) {
  auto constant = input ? input.getDefiningOp<BitsConstantOp>() : BitsConstantOp();
  if (!constant || failed(constant.verifyInvariantsImpl()) ||
      failed(constant.verify()))
    return false;
  auto literal = [](StaticExprAttr expression) -> Attribute {
    auto tree = expression.getTree();
    auto kind = tree.getAs<StringAttr>("kind");
    auto value = tree.getAs<DictionaryAttr>("value");
    return kind && kind.getValue() == "literal" && value
               ? value.get("value") : Attribute();
  };
  auto width = dyn_cast_or_null<MathIntAttr>(
      literal(constant.getResult().getType().getWidth()));
  auto value = literal(constant.getValue());
  if (!width || width.getCanonicalValue() != "1")
    return false;
  if (auto integer = dyn_cast_or_null<MathIntAttr>(value))
    return integer.getCanonicalValue() == "1";
  if (auto boolean = dyn_cast_or_null<BoolAttr>(value))
    return boolean.getValue();
  return false;
}

namespace {
/// Return the clock/reset SSA values named by the lexical Module contract.
std::pair<Value, Value> registrationDomain(ModuleOp module) {
  auto domain = module->getAttrOfType<DictionaryAttr>("ac.domain_inputs");
  if (!domain || domain.size() != 2 || !module.getBody().hasOneBlock())
    return {};
  auto &body = module.getBody().front();
  auto input = [&](StringRef name) -> Value {
    auto index = domain.getAs<IntegerAttr>(name);
    if (!index || !index.getType().isSignlessInteger(64) ||
        index.getInt() < 0 ||
        static_cast<uint64_t>(index.getInt()) >= body.getNumArguments())
      return {};
    return body.getArgument(index.getInt());
  };
  return {input("clock"), input("reset")};
}
} // namespace

FailureOr<std::optional<SourceRegistrationEntryView>>
getSourceRegistrationEntry(SourceTransactionOp transaction) {
  Attribute marker = transaction->getAttr("ac.registration_entry");
  if (!marker)
    return std::optional<SourceRegistrationEntryView>();
  auto error = [&]() {
    return transaction.emitOpError() << "registration entry requires ";
  };
  if (!isa<UnitAttr>(marker))
    return error() << "a unit ac.registration_entry marker";
  auto module = dyn_cast_or_null<ModuleOp>(transaction->getParentOp());
  if (!module || transaction->getNumRegions() != 2 ||
      !transaction.getGuard().hasOneBlock() ||
      !transaction.getBody().hasOneBlock())
    return error() << "direct module placement and single guard/body blocks";
  auto &guard = transaction.getGuard().front();
  auto &body = transaction.getBody().front();
  if (guard.getNumArguments() != transaction.getCaptures().size() ||
      body.getNumArguments() != transaction.getCaptures().size() ||
      std::distance(guard.begin(), guard.end()) != 2 ||
      std::distance(body.begin(), body.end()) != 3)
    return error() << "only a true guard and one transparent Method call";
  auto guardYield = dyn_cast<YieldOp>(guard.back());
  auto call = dyn_cast<SourceMethodCallOp>(*std::next(body.begin()));
  auto resultYield = dyn_cast<YieldOp>(body.back());
  if (!call)
    return error() << "one transparent Method call";
  if (failed(call.verifyInvariantsImpl()))
    return failure();
  if (!guardYield || guardYield.getValues().size() != 1 ||
      guardYield.getValues()[0].getDefiningOp() != &guard.front() ||
      !isSourceLiteralTrue(guardYield.getValues()[0]) || !resultYield ||
      !call.getPath() || call.getPath().getDefiningOp() != &body.front() ||
      !isSourceLiteralTrue(call.getPath()))
    return error() << "direct true guard/path constants and exact call forwarding";
  if (!call.getInstancePath().empty() ||
      call->hasAttr("ac.reference") || call->hasAttr("receiver_kind") ||
      call->hasAttr("ancestor_depth") ||
      call->hasAttr("ac.formal_owner_bindings") ||
      call.getOccurrence() != transaction.getOccurrence())
    return error() << "one local call with registration identity and no owner map";
  auto callee = call.getCalleeAttr();
  if (callee.getRootReference() != module.getSymNameAttr() ||
      callee.getNestedReferences().size() != 1)
    return error() << "a Method in the same lexical module";
  auto resolved = lookupSourceMethod(module,
      callee.getNestedReferences().front().getValue(), transaction);
  if (failed(resolved) || !resolved->definition)
    return failure();
  SourceMethodOp method = resolved->definition;
  if (method.getOccurrence() != transaction.getDeclaration() ||
      method->getAttr("ac.guard_location") !=
          transaction->getAttr("ac.guard_location"))
    return error() << "the declared Method and identical guard location";
  if (transaction.getOccurrence() == transaction.getDeclaration()) {
    auto visibility = resolved->getVisibility();
    if (!visibility || visibility.getValue() != "private")
      return error() << "a private body for an autonomous Rule declaration";
  }
  auto signature = resolved->getFunctionType();
  if (call.getArguments().size() != signature.getNumInputs() ||
      call.getNumResults() != signature.getNumResults() ||
      call.getArguments().size() != body.getNumArguments())
    return error() << "all and only explicit arguments in declared order";
  SourceRegistrationEntryView view{method, call, {}};
  for (auto [index, argument] : llvm::enumerate(call.getArguments())) {
    if (argument != body.getArgument(index) ||
        !areEquivalentHardwareTypes(argument.getType(), signature.getInput(index)))
      return error() << "exact body argument forwarding in declared order";
    view.argumentCaptureOrdinals.push_back(index);
  }
  unsigned publicCount = resolved->getNumPublicResults();
  if (transaction.getNumResults() != publicCount ||
      resultYield.getValues().size() != publicCount)
    return error() << "only the Method public result prefix";
  for (auto [index, result] : llvm::enumerate(call.getResults())) {
    if (!areEquivalentHardwareTypes(result.getType(), signature.getResult(index)))
      return error() << "the exact physical Method result types";
    if (index < publicCount &&
        (resultYield.getValues()[index] != result ||
         !areEquivalentHardwareTypes(transaction.getResult(index).getType(),
                                     signature.getResult(index))))
      return error() << "the ordered public result prefix";
  }
  auto [clock, reset] = registrationDomain(module);
  if (transaction.getClk() != clock || transaction.getRst() != reset)
    return error() << "the lexical Method clock/reset domain";
  return std::optional<SourceRegistrationEntryView>(std::move(view));
}

LogicalResult verifyLinkedSourceRegistration(SourceMethodCallOp call,
                                             const SourceMethodView &method) {
  auto module = cast<ModuleOp>(call->getParentOp());
  if (!method.definition || method.module != module ||
      !call.getInstancePath().empty() || call->hasAttr("ac.reference") ||
      call->hasAttr("receiver_kind") || call->hasAttr("ancestor_depth") ||
      call->hasAttr("ac.formal_owner_bindings"))
    return call.emitOpError()
           << "registration requires a local Method definition without owner bindings";
  if (!call.hasImplicitPath() && !isSourceLiteralTrue(call.getPath()))
    return call.emitOpError() << "registration path must be a true constant";
  auto [clock, reset] = registrationDomain(module);
  if (!clock || !reset)
    return call.emitOpError() << "registration requires the lexical clock/reset domain";
  if (call.getOccurrence() == method.getDeclaration()) {
    auto visibility = method.getVisibility();
    if (!visibility || visibility.getValue() != "private")
      return call.emitOpError() << "autonomous Rule requires a private Method body";
  }
  for (auto result : call.getResults().drop_front(method.getNumPublicResults()))
    if (!result.use_empty())
      return call.emitOpError() << "registration may expose only public Method results";
  return success();
}
} // namespace acir::ac
