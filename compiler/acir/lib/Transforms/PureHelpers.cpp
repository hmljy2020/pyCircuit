#include "acir/Transforms/Passes.h"

#include "acir/Dialect/ACIR/ACIRTypes.h"
#include "mlir/Dialect/Func/IR/FuncOps.h"
#include "mlir/IR/IRMapping.h"
#include "mlir/IR/SymbolTable.h"
#include "mlir/Interfaces/SideEffectInterfaces.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/StringMap.h"

using namespace mlir;

namespace acir {
namespace {

constexpr size_t kMaxHelpers = 256;
constexpr size_t kMaxHelperDepth = 64;
constexpr size_t kMaxHelperEdges = 4096;

bool isHelper(func::FuncOp function) {
  auto marker = function->getAttrOfType<BoolAttr>("ac.helper");
  return marker && marker.getValue();
}

bool requestsInline(func::FuncOp function) {
  auto marker = function->getAttrOfType<BoolAttr>("ac.inline");
  return marker && marker.getValue();
}

LogicalResult verifyHelperBody(func::FuncOp function,
                               SmallVectorImpl<func::CallOp> &calls) {
  if (!function.isPrivate() || function.isExternal() ||
      !llvm::hasSingleElement(function.getBody()))
    return function.emitOpError(
        "pure helper must be private with one defined block");
  FunctionType type = function.getFunctionType();
  if (type.getNumInputs() == 0 || type.getNumResults() == 0 ||
      llvm::any_of(type.getInputs(),
                   [](Type value) { return !isa<ac::VarType>(value); }) ||
      llvm::any_of(type.getResults(),
                   [](Type value) { return !isa<ac::VarType>(value); }))
    return function.emitOpError(
        "pure helper requires ac.var parameters and one or more ac.var results");
  auto returned = dyn_cast<func::ReturnOp>(function.getBody().front().back());
  if (!returned || returned.getOperandTypes() != type.getResults())
    return function.emitOpError(
        "pure helper return values must exactly match its result types");

  for (Operation &operation : function.getBody().front().without_terminator()) {
    if (auto call = dyn_cast<func::CallOp>(operation)) {
      calls.push_back(call);
      continue;
    }
    if (!operation.getName().getStringRef().starts_with("ac.var.") ||
        !isMemoryEffectFree(&operation))
      return operation.emitOpError(
          "pure helper body permits only effect-free ac.var operations and "
          "calls to other pure helpers");
  }
  return success();
}

struct HelperNode {
  func::FuncOp function;
  SmallVector<func::CallOp> calls;
};

FailureOr<llvm::StringMap<HelperNode>> collectAndVerify(ModuleOp model) {
  llvm::StringMap<HelperNode> helpers;
  for (func::FuncOp function : model.getOps<func::FuncOp>()) {
    if (!isHelper(function)) {
      if (function->hasAttr("ac.inline"))
        return function.emitOpError("ac.inline requires ac.helper = true");
      continue;
    }
    if (helpers.size() == kMaxHelpers)
      return model.emitError("pure helper count exceeds capability limit 256");
    SmallVector<func::CallOp> calls;
    if (failed(verifyHelperBody(function, calls)))
      return failure();
    helpers.try_emplace(function.getSymName(), HelperNode{function, calls});
  }

  size_t edges = 0;
  for (auto &entry : helpers)
    for (func::CallOp call : entry.getValue().calls) {
      if (++edges > kMaxHelperEdges)
        return call.emitOpError(
            "pure helper call graph exceeds capability limit 4096");
      auto target = helpers.find(call.getCallee());
      if (target == helpers.end())
        return call.emitOpError() << "pure helper callee '@" << call.getCallee()
                                  << "' is unresolved or not a pure helper";
    }

  enum class State : uint8_t { New, Active, Done };
  llvm::StringMap<State> states;
  SmallVector<StringRef> stack;
  std::function<LogicalResult(StringRef)> visit =
      [&](StringRef name) -> LogicalResult {
    if (states[name] == State::Done)
      return success();
    if (states[name] == State::Active) {
      auto found = llvm::find(stack, name);
      InFlightDiagnostic diagnostic =
          helpers[name].function.emitOpError("recursive pure helper call graph: ");
      for (auto item = found; item != stack.end(); ++item)
        diagnostic << '@' << *item << " -> ";
      diagnostic << '@' << name;
      return failure();
    }
    if (stack.size() == kMaxHelperDepth) {
      helpers[name].function.emitOpError(
          "pure helper call graph exceeds depth limit 64");
      return failure();
    }
    states[name] = State::Active;
    stack.push_back(name);
    for (func::CallOp call : helpers[name].calls)
      if (failed(visit(call.getCallee())))
        return failure();
    stack.pop_back();
    states[name] = State::Done;
    return success();
  };
  for (auto &entry : helpers)
    if (failed(visit(entry.getKey())))
      return failure();

  LogicalResult roots = success();
  model.walk([&](func::CallOp call) {
    if (failed(roots) || call->getParentOfType<func::FuncOp>())
      return;
    if (!helpers.contains(call.getCallee()))
      roots = call.emitOpError(
          "Queue expression call must resolve to a verified pure helper");
  });
  if (failed(roots))
    return failure();
  return helpers;
}

LogicalResult inlineOne(func::CallOp call, func::FuncOp callee) {
  Block &body = callee.getBody().front();
  if (call.getNumOperands() != body.getNumArguments() ||
      call.getNumResults() != callee.getNumResults())
    return call.emitOpError("pure helper call signature is malformed");
  IRMapping mapping;
  for (auto [argument, value] :
       llvm::zip_equal(body.getArguments(), call.getOperands()))
    mapping.map(argument, value);
  OpBuilder builder(call);
  for (Operation &operation : body.without_terminator())
    builder.clone(operation, mapping);
  auto returned = cast<func::ReturnOp>(body.getTerminator());
  for (auto [result, returnedValue] :
       llvm::zip_equal(call.getResults(), returned.getOperands()))
    result.replaceAllUsesWith(mapping.lookupOrDefault(returnedValue));
  call.erase();
  return success();
}

#define GEN_PASS_DEF_VERIFYPUREHELPERSPASS
#define GEN_PASS_DEF_INLINEPUREHELPERSPASS
#include "acir/Transforms/Passes.h.inc"

struct VerifyPureHelpersPass
    : impl::VerifyPureHelpersPassBase<VerifyPureHelpersPass> {
  void runOnOperation() override {
    if (failed(verifyPureHelpers(getOperation())))
      signalPassFailure();
  }
};

struct InlinePureHelpersPass
    : impl::InlinePureHelpersPassBase<InlinePureHelpersPass> {
  void runOnOperation() override {
    ModuleOp model = getOperation();
    auto helpers = collectAndVerify(model);
    if (failed(helpers)) {
      signalPassFailure();
      return;
    }
    while (true) {
      SmallVector<func::CallOp> calls;
      model.walk([&](func::CallOp call) {
        auto found = helpers->find(call.getCallee());
        if (found != helpers->end() && requestsInline(found->getValue().function))
          calls.push_back(call);
      });
      if (calls.empty())
        break;
      for (func::CallOp call : calls) {
        auto found = helpers->find(call.getCallee());
        if (found == helpers->end() ||
            failed(inlineOne(call, found->getValue().function))) {
          signalPassFailure();
          return;
        }
      }
    }
    SmallVector<func::FuncOp> erased;
    for (auto &entry : *helpers)
      if (requestsInline(entry.getValue().function))
        erased.push_back(entry.getValue().function);
    for (func::FuncOp function : erased)
      function.erase();
  }
};

} // namespace

LogicalResult verifyPureHelpers(ModuleOp model) {
  return succeeded(collectAndVerify(model)) ? success() : failure();
}

std::unique_ptr<Pass> createVerifyPureHelpersPass() {
  return std::make_unique<VerifyPureHelpersPass>();
}

std::unique_ptr<Pass> createInlinePureHelpersPass() {
  return std::make_unique<InlinePureHelpersPass>();
}

} // namespace acir
