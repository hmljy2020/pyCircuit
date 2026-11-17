#include "mlir/Dialect/Arith/IR/Arith.h"
#include "mlir/Dialect/Func/IR/FuncOps.h"
#include "mlir/IR/Diagnostics.h"
#include "mlir/IR/Verifier.h"
#include "mlir/Parser/Parser.h"
#include "mlir/Pass/Pass.h"
#include "mlir/Pass/PassManager.h"
#include "pycircuit/Dialect/ACIR/ACIRDialect.h"
#include "pycircuit/Dialect/ACIR/ACIROps.h"
#include "pycircuit/Dialect/ACIR/SourceUnitValidation.h"
#include "pycircuit/Transforms/Passes.h"
#include "llvm/Support/raw_ostream.h"
#include "gtest/gtest.h"

namespace {
using namespace mlir;
namespace ac = acir::ac;

const char *integerTransport = R"mlir(
#occ = {site = {definition = @unit.compute, ast_path = []}, expansion = []}
#span = {path = "unit.py", line = 1 : i64, column = 1 : i64, end_line = 1 : i64, end_column = 2 : i64}
#w8 = #ac.static_expr<{kind = "literal", location = #span, origin = #occ, value = {kind = "integer", value = #ac.math_int<8>}}>
#w16 = #ac.static_expr<{kind = "literal", location = #span, origin = #occ, value = {kind = "integer", value = #ac.math_int<16>}}>
!b8 = !ac.bits<#w8>
!b16 = !ac.bits<#w16>
module attributes {ac.stage = "source-i64"} {
  func.func @unit.compute(%input: !b8) -> (!b16, !b8) {
    %x = "ac.source.integer_from_bits"(%input) {domain = {lower = #ac.math_int<0>, upper = #ac.math_int<256>}, is_signed = false, occurrence = #occ, source_location = #span} : (!b8) -> i64
    %three = arith.constant 3 : i64
    %sum = arith.addi %x, %three : i64
    %product = arith.muli %sum, %three : i64
    %difference = arith.subi %product, %three : i64
    %result = "ac.source.integer_to_bits"(%difference) {occurrence = #occ, source_location = #span} : (i64) -> !b16
    %mask = arith.constant 128 : i64
    %shifted = arith.shrui %mask, %three : i64
    %bits = "ac.source.integer_to_bits"(%shifted) {occurrence = #occ, source_location = #span} : (i64) -> !b8
    return %result, %bits : !b16, !b8
  }
}
)mlir";

void load(MLIRContext &context) {
  context.loadDialect<ac::ACIRDialect, arith::ArithDialect, func::FuncDialect>();
}

TEST(SourceIntegerExpressions, PrintedTransportLowersInFreshContext) {
  std::string saved;
  {
    MLIRContext context;
    load(context);
    auto unit = parseSourceString<ModuleOp>(integerTransport, &context);
    ASSERT_TRUE(unit);
    ASSERT_TRUE(succeeded(verify(*unit)));
    llvm::raw_string_ostream stream(saved);
    unit->print(stream);
  }
  MLIRContext fresh;
  load(fresh);
  auto unit = parseSourceString<ModuleOp>(saved, &fresh);
  ASSERT_TRUE(unit);
  PassManager pipeline(&fresh);
  pipeline.addPass(acir::createNormalizeSourceControlFlowPass());
  pipeline.addPass(acir::createLowerSourceIntegerExpressionsPass());
  ASSERT_TRUE(succeeded(pipeline.run(*unit)));
  ASSERT_TRUE(succeeded(verify(*unit)));
  EXPECT_NE((*unit)->getAttrOfType<StringAttr>("ac.stage").getValue(),
            "source-i64");
  unit->walk([](Operation *operation) {
    EXPECT_NE(operation->getName().getDialectNamespace(), "arith");
    EXPECT_FALSE((isa<ac::SourceIntegerFromBitsOp,
                      ac::SourceIntegerToBitsOp>(operation)));
  });
  bool sixteen = false;
  unit->walk([&](ac::BitsConstantOp constant) {
    auto value = constant.getValue().getTree().getAs<DictionaryAttr>("value").getAs<ac::MathIntAttr>("value");
    sixteen |= value && value.getCanonicalValue() == "16";
  });
  EXPECT_TRUE(sixteen);
}

TEST(SourceIntegerExpressions, PublicationRejectsUnloweredTransport) {
  MLIRContext context;
  load(context);
  auto unit = parseSourceString<ModuleOp>(integerTransport, &context);
  ASSERT_TRUE(unit);
  std::string diagnostics;
  ScopedDiagnosticHandler capture(&context, [&](Diagnostic &diagnostic) {
    llvm::raw_string_ostream stream(diagnostics);
    diagnostic.print(stream);
    return success();
  });
  auto error = [&] { return unit->emitError(); };
  EXPECT_TRUE(failed(ac::verifySourceBodyStructure(*unit, error)));
  EXPECT_NE(diagnostics.find("integer expressions"), std::string::npos);
  diagnostics.clear();
  (*unit)->removeAttr("ac.stage");
  EXPECT_TRUE(failed(ac::verifySourceBodyStructure(*unit, error)));
  EXPECT_NE(diagnostics.find("integer expressions"), std::string::npos);
}
} // namespace
