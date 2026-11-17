#include "Dialect/ACIR/SourceCheckBudget.h"
#include "mlir/Dialect/Arith/IR/Arith.h"
#include "mlir/IR/Builders.h"
#include "mlir/IR/SymbolTable.h"
#include "mlir/IR/Verifier.h"
#include "mlir/Pass/Pass.h"
#include "pycircuit/Dialect/ACIR/ACIRDialect.h"
#include "pycircuit/Dialect/ACIR/ACIROps.h"
#include "pycircuit/Dialect/ACIR/HardwareAnalysis.h"
#include "pycircuit/Dialect/ACIR/SourceHelperAnalysis.h"
#include "pycircuit/Transforms/Passes.h"
#include "llvm/ADT/DenseMap.h"
#include <optional>

using namespace mlir;
namespace acir {
namespace {
// Inclusive signed 128-bit endpoints leave room to compute products of two
// i64 domains before deciding whether the resulting fact remains trustworthy.
struct Interval {
  llvm::APInt low, high;
};
struct IntegerValue {
  Value bits;
  std::optional<llvm::APInt> constant;
  std::optional<Interval> interval;
};
struct Site {
  DictionaryAttr occurrence, location;
};

class Lowering {
public:
  explicit Lowering(mlir::ModuleOp package) : package(package) {}
  LogicalResult run();

private:
  mlir::ModuleOp package;
  llvm::DenseMap<Value, IntegerValue> values;
  llvm::DenseMap<Operation *, Site> sites;
  ac::StaticExprAttr literal(OpBuilder &b, const llvm::APInt &v, Site site) {
    auto payload = b.getDictionaryAttr(
        {b.getNamedAttr("kind", b.getStringAttr("integer")),
         b.getNamedAttr("value", ac::MathIntAttr::get(b.getContext(),
                                                      llvm::APSInt(v, true)))});
    return ac::StaticExprAttr::get(
        b.getContext(),
        b.getDictionaryAttr({b.getNamedAttr("kind", b.getStringAttr("literal")),
                             b.getNamedAttr("value", payload),
                             b.getNamedAttr("origin", site.occurrence),
                             b.getNamedAttr("location", site.location)}));
  }
  ac::BitsType bitsType(OpBuilder &b, unsigned width, Site site) {
    return ac::BitsType::get(b.getContext(),
                             literal(b, llvm::APInt(64, width), site));
  }
  Value operation(OpBuilder &b, Operation *at, StringRef name,
                  ValueRange operands, Type type, StringRef key,
                  Attribute attr) {
    OperationState state(at->getLoc(), name);
    state.addOperands(operands);
    state.addTypes(type);
    state.addAttribute(key, attr);
    return b.create(state)->getResult(0);
  }
  Value constant(OpBuilder &b, Operation *at, const llvm::APInt &v, Type type,
                 Site site) {
    return operation(b, at, ac::BitsConstantOp::getOperationName(), {}, type,
                     "value", literal(b, v, site));
  }
  FailureOr<unsigned> width(ac::BitsType type, Operation *at) {
    ac::HardwareAnalysis analysis(package);
    ac::HardwareBindings bindings;
    bindings.owner = at->getParentOfType<ac::ModuleOp>();
    auto result = analysis.getPackedWidth(type, bindings, at);
    if (failed(result) || *result == 0 || *result > 1048576)
      return failure();
    return static_cast<unsigned>(*result);
  }
  Value resize(OpBuilder &b, Operation *at, Value v, Type type,
               StringRef mode) {
    if (v.getType() == type)
      return v;
    if (ac::areEquivalentHardwareTypes(v.getType(), type))
      return operation(b, at, ac::BitsExtractOp::getOperationName(), v, type,
                       "low", literal(b, llvm::APInt(64, 0), sites.lookup(at)));
    return operation(b, at, ac::BitsResizeOp::getOperationName(), v, type,
                     "mode", b.getStringAttr(mode));
  }
  LogicalResult lower(Operation *op);
};

LogicalResult Lowering::lower(Operation *op) {
  OpBuilder b(op);
  Site site = sites.lookup(op);
  if (auto from = dyn_cast<ac::SourceIntegerFromBitsOp>(op)) {
    auto w = width(from.getInput().getType(), op);
    if (failed(w) || *w > 64)
      return op->emitOpError()
             << "source i64 requires a carrier of at most 64 bits";
    llvm::APSInt lo(
        from.getDomain().getAs<ac::MathIntAttr>("lower").getCanonicalValue());
    llvm::APSInt upper(
        from.getDomain().getAs<ac::MathIntAttr>("upper").getCanonicalValue());
    if (lo.getSignificantBits() > 127 || upper.getSignificantBits() > 127)
      return op->emitOpError()
             << "declared Integer domain exceeds source i64 carrier";
    auto low = lo.isSigned() ? lo.sextOrTrunc(128) : lo.zextOrTrunc(128);
    auto hi =
        upper.isSigned() ? upper.sextOrTrunc(128) : upper.zextOrTrunc(128);
    auto minimum = from.getIsSigned()
                       ? llvm::APInt::getSignedMinValue(*w).sext(128)
                       : llvm::APInt(128, 0);
    auto maximum = from.getIsSigned()
                       ? llvm::APInt::getSignedMaxValue(*w).sext(128)
                       : llvm::APInt::getMaxValue(*w).zext(128);
    if (low.sge(hi) || low.slt(minimum) || (hi - 1).sgt(maximum))
      return op->emitOpError()
             << "declared Integer domain does not fit its carrier";
    values[from.getResult()] = {resize(b, op, from.getInput(),
                                       bitsType(b, 64, site),
                                       from.getIsSigned() ? "sext" : "zext"),
                                std::nullopt, Interval{low, hi - 1}};
    return success();
  }
  if (auto to = dyn_cast<ac::SourceIntegerToBitsOp>(op)) {
    auto found = values.find(to.getInput());
    if (found == values.end())
      return op->emitOpError() << "input lacks source Integer identity";
    auto w = width(to.getResult().getType(), op);
    if (failed(w))
      return failure();
    auto &v = found->second;
    if (!v.interval || v.interval->low.isNegative() ||
        (*w < 128 && v.interval->high.uge(llvm::APInt(128, 1).shl(*w))))
      return op->emitOpError()
             << "integer interval is not proven within destination bounds";
    Value result = v.constant ? constant(b, op, v.constant->zextOrTrunc(*w),
                                         to.getResult().getType(), site)
                              : resize(b, op, v.bits, to.getResult().getType(),
                                       *w < 64 ? "trunc" : "zext");
    to.getResult().replaceAllUsesWith(result);
    return success();
  }
  if (auto c = dyn_cast<arith::ConstantOp>(op)) {
    auto attr = dyn_cast<IntegerAttr>(c.getValue());
    if (!attr || !c.getType().isInteger(64))
      return op->emitOpError() << "expected source i64 integer constant";
    auto v = attr.getValue();
    auto extended = v.sext(128);
    values[c.getResult()] = {constant(b, op, v, bitsType(b, 64, site), site), v,
                             Interval{extended, extended}};
    return success();
  }
  if (op->getNumOperands() != 2 || op->getNumResults() != 1 ||
      !op->getResult(0).getType().isInteger(64))
    return op->emitOpError() << "unsupported source integer operation";
  auto left = values.find(op->getOperand(0)),
       right = values.find(op->getOperand(1));
  if (left == values.end() || right == values.end())
    return op->emitOpError()
           << "arithmetic operand lacks source Integer identity";
  auto &l = left->second;
  auto &r = right->second;
  StringRef opcode;
  if (isa<arith::AddIOp>(op))
    opcode = "add";
  else if (isa<arith::SubIOp>(op))
    opcode = "sub";
  else if (isa<arith::MulIOp>(op))
    opcode = "mul";
  else if (isa<arith::AndIOp>(op))
    opcode = "and";
  else if (isa<arith::OrIOp>(op))
    opcode = "or";
  else if (isa<arith::XOrIOp>(op))
    opcode = "xor";
  else if (isa<arith::ShRUIOp, arith::ShRSIOp>(op))
    opcode = "lshr";
  else
    return op->emitOpError() << "unsupported source integer arithmetic";
  if (auto flags =
          op->getAttrOfType<arith::IntegerOverflowFlagsAttr>("overflowFlags"))
    if (flags.getValue() != arith::IntegerOverflowFlags::none)
      return op->emitOpError()
             << "source integer arithmetic cannot carry overflow promises";
  bool shift = opcode == "lshr";
  if (shift && (!r.constant || r.constant->isNegative() || !l.interval ||
                l.interval->low.isNegative()))
    return op->emitOpError() << "integer right shift requires a static "
                                "nonnegative count and nonnegative input";
  std::optional<llvm::APInt> folded;
  if (l.constant && r.constant) {
    auto a = *l.constant, d = *r.constant;
    if (opcode == "add")
      folded = a + d;
    else if (opcode == "sub")
      folded = a - d;
    else if (opcode == "mul")
      folded = a * d;
    else if (opcode == "and")
      folded = a & d;
    else if (opcode == "or")
      folded = a | d;
    else if (opcode == "xor")
      folded = a ^ d;
    else
      folded = d.uge(64) ? llvm::APInt(64, 0) : a.lshr(d.getZExtValue());
  }
  std::optional<Interval> interval;
  if (l.interval && r.interval) {
    auto a = *l.interval, d = *r.interval;
    if (opcode == "add")
      interval = Interval{a.low + d.low, a.high + d.high};
    else if (opcode == "sub")
      interval = Interval{a.low - d.high, a.high - d.low};
    else if (opcode == "mul") {
      bool overflow = false, ov = false;
      llvm::APInt products[] = {a.low.smul_ov(d.low, ov), llvm::APInt(128, 0),
                                llvm::APInt(128, 0), llvm::APInt(128, 0)};
      overflow |= ov;
      products[1] = a.low.smul_ov(d.high, ov);
      overflow |= ov;
      products[2] = a.high.smul_ov(d.low, ov);
      overflow |= ov;
      products[3] = a.high.smul_ov(d.high, ov);
      overflow |= ov;
      auto lo = products[0], hi = products[0];
      for (const auto &p : products) {
        if (p.slt(lo))
          lo = p;
        if (p.sgt(hi))
          hi = p;
      }
      if (!overflow)
        interval = Interval{lo, hi};
    } else if (shift) {
      interval = r.constant->uge(64)
                     ? Interval{llvm::APInt(128, 0), llvm::APInt(128, 0)}
                     : Interval{a.low.lshr(r.constant->getZExtValue()),
                                a.high.lshr(r.constant->getZExtValue())};
    } else if (opcode == "and" && ((r.constant && !r.constant->isNegative()) ||
                                   (l.constant && !l.constant->isNegative()))) {
      auto mask =
          r.constant && !r.constant->isNegative() ? *r.constant : *l.constant;
      interval = Interval{llvm::APInt(128, 0), mask.zext(128)};
    } else if (!a.low.isNegative() && !d.low.isNegative()) {
      unsigned bits = std::max(a.high.getActiveBits(), d.high.getActiveBits());
      interval =
          Interval{llvm::APInt(128, 0), llvm::APInt::getLowBitsSet(128, bits)};
    }
    // Once i64 can wrap, these mathematical facts cannot authorize narrowing.
    if (interval &&
        (interval->low.slt(llvm::APInt::getSignedMinValue(64).sext(128)) ||
         interval->high.sgt(llvm::APInt::getSignedMaxValue(64).sext(128))))
      interval.reset();
  }
  Value result;
  auto type = bitsType(b, 64, site);
  if (folded)
    result = constant(b, op, *folded, type, site);
  else if (shift) {
    if (r.constant->uge(64))
      result = constant(b, op, llvm::APInt(64, 0), type, site);
    else {
      unsigned amount = r.constant->getZExtValue();
      auto extracted = operation(b, op, ac::BitsExtractOp::getOperationName(),
                                 l.bits, bitsType(b, 64 - amount, site), "low",
                                 literal(b, *r.constant, site));
      result = resize(b, op, extracted, type, "zext");
    }
  } else
    result =
        operation(b, op, ac::BitsBinaryOp::getOperationName(), {l.bits, r.bits},
                  type, "opcode", b.getStringAttr(opcode));
  values[op->getResult(0)] = {result, folded, interval};
  return success();
}

LogicalResult Lowering::run() {
  SmallVector<Operation *> ops;
  package.walk<WalkOrder::PreOrder>([&](Operation *op) {
    if (isa<ac::SourceIntegerFromBitsOp, ac::SourceIntegerToBitsOp>(op) ||
        op->getName().getDialectNamespace() == "arith")
      ops.push_back(op);
  });
  if (ops.empty()) {
    if (auto stage = package->getAttrOfType<StringAttr>("ac.stage");
        stage && stage.getValue() == "source-i64")
      package->setAttr("ac.stage",
                       StringAttr::get(package.getContext(), "source"));
    return success();
  }
  SmallVector<std::pair<Value, Site>> work;
  for (Operation *op : ops) {
    if (auto occurrence = op->getAttrOfType<DictionaryAttr>("occurrence")) {
      Site site{occurrence,
                op->getAttrOfType<DictionaryAttr>("source_location")};
      sites[op] = site;
      for (Value operand : op->getOperands())
        work.emplace_back(operand, site);
    }
  }
  while (!work.empty()) {
    auto [value, site] = work.pop_back_val();
    auto *op = value.getDefiningOp();
    if (!op || sites.count(op) ||
        op->getName().getDialectNamespace() != "arith")
      continue;
    sites[op] = site;
    for (Value operand : op->getOperands())
      work.emplace_back(operand, site);
  }
  for (Operation *op : ops) {
    // Boundary provenance reaches every live arithmetic producer. Unreached
    // arithmetic is dead source computation and needs no hardware
    // materialization.
    if (!sites.count(op))
      continue;
    if (failed(lower(op)))
      return failure();
  }
  for (Operation *op : llvm::reverse(ops)) {
    if (!op->use_empty())
      return op->emitOpError()
             << "source integer escaped its explicit conversion boundary";
    op->erase();
  }
  if (auto stage = package->getAttrOfType<StringAttr>("ac.stage");
      stage && stage.getValue() == "source-i64")
    package->setAttr("ac.stage",
                     StringAttr::get(package.getContext(), "source"));
  SmallVector<func::FuncOp> helpers;
  package.walk([&](func::FuncOp helper) {
    auto kind = helper->getAttrOfType<StringAttr>("ac.helper_kind");
    if (kind && kind.getValue() == "value")
      helpers.push_back(helper);
  });
  return ac::verifySourceValueHelperClosure(
      helpers, [&](FlatSymbolRefAttr symbol) {
        return package.lookupSymbol<func::FuncOp>(symbol.getValue());
      });
}

class LowerSourceIntegerExpressionsPass
    : public PassWrapper<LowerSourceIntegerExpressionsPass,
                         OperationPass<mlir::ModuleOp>> {
public:
  MLIR_DEFINE_EXPLICIT_INTERNAL_INLINE_TYPE_ID(
      LowerSourceIntegerExpressionsPass)
  StringRef getArgument() const override {
    return "ac-lower-source-integer-expressions";
  }
  StringRef getDescription() const override {
    return "Lower staged i64 source Integer expressions to four-state Bits";
  }
  void getDependentDialects(DialectRegistry &registry) const override {
    registry.insert<ac::ACIRDialect, arith::ArithDialect>();
  }
  void runOnOperation() override {
    auto package = getOperation();
    bool transient = false;
    package.walk([&](Operation *op) {
      transient |=
          isa<ac::SourceIntegerFromBitsOp, ac::SourceIntegerToBitsOp>(op);
    });
    if (!transient && package->getAttrOfType<StringAttr>("ac.stage") !=
                          StringAttr::get(package.getContext(), "source-i64"))
      return;
    uint64_t count = 0;
    ac::detail::SourceCheckBudget budget{ac::HardwareSourceCheckLimits{}};
    auto scan = package.walk([&](Operation *op) {
      if (++count > ac::HardwareSourceCheckLimits{}.maxWorkUnits) {
        op->emitOpError() << "source integer input exceeds analysis budget";
        return WalkResult::interrupt();
      }
      // Reserve the complete per-expression lowering cost before cloning the
      // package or allocating provenance and materialization plans.
      if ((isa<ac::SourceIntegerFromBitsOp, ac::SourceIntegerToBitsOp>(op) ||
           op->getName().getDialectNamespace() == "arith") &&
          failed(budget.debit(64, op)))
        return WalkResult::interrupt();
      return WalkResult::advance();
    });
    if (scan.wasInterrupted()) {
      signalPassFailure();
      return;
    }
    OwningOpRef<mlir::ModuleOp> candidate(
        cast<mlir::ModuleOp>(package->clone()));
    if (failed(Lowering(*candidate).run()) || failed(verify(*candidate))) {
      signalPassFailure();
      return;
    }
    package->setAttrs((*candidate)->getAttrs());
    package.getBodyRegion().takeBody(candidate->getBodyRegion());
  }
};
} // namespace
std::unique_ptr<Pass> createLowerSourceIntegerExpressionsPass() {
  return std::make_unique<LowerSourceIntegerExpressionsPass>();
}
} // namespace acir
