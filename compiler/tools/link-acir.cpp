#include "Compiler/LinkedSourceACIR.h"
#include "mlir/Dialect/Arith/IR/Arith.h"
#include "mlir/Dialect/Func/IR/FuncOps.h"
#include "mlir/Parser/Parser.h"
#include "pycircuit/Dialect/ACIR/ACIRDialect.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/raw_ostream.h"

using namespace mlir;

namespace {
namespace cl = llvm::cl;

cl::list<std::string> bodies("body", cl::desc("Source unit body path"));
cl::list<std::string> headers("header", cl::desc("Owning interface path"));
cl::opt<std::string> top("top", cl::desc("Qualified top Module name"));
cl::opt<std::string> output("output",
                            cl::desc("Fresh linked-source ACIR file"));
cl::opt<bool> verifyOnly("verify-only",
                         cl::desc("Verify a linked-source artifact"));
cl::opt<std::string> input(cl::Positional, cl::desc("<linked.ac>"),
                           cl::Optional);

LogicalResult publish(ModuleOp linked, StringRef path) {
  int descriptor;
  auto error = llvm::sys::fs::openFileForWrite(
      path, descriptor, llvm::sys::fs::CD_CreateNew, llvm::sys::fs::OF_None);
  if (error) {
    llvm::errs() << "error: cannot create output '" << path
                 << "': " << error.message() << '\n';
    return failure();
  }
  llvm::raw_fd_ostream stream(descriptor, true);
  linked.print(stream, OpPrintingFlags().enableDebugInfo());
  stream << '\n';
  stream.close();
  if (!stream.has_error())
    return success();
  llvm::errs() << "error: could not write output '" << path
               << "': " << stream.error().message() << '\n';
  stream.clear_error();
  if (auto cleanupError = llvm::sys::fs::remove(path))
    llvm::errs() << "error: could not remove incomplete output '" << path
                 << "': " << cleanupError.message() << '\n';
  return failure();
}
} // namespace

int main(int argc, char **argv) {
  cl::ParseCommandLineOptions(argc, argv, "pyCircuit linked source ACIR\n");
  if (verifyOnly ? input.empty() || !bodies.empty() || !headers.empty() ||
                       top.getNumOccurrences() || output.getNumOccurrences()
                 : input.getNumOccurrences() || bodies.empty() ||
                       bodies.size() != headers.size() || top.empty() ||
                       output.empty() || output == "-") {
    llvm::errs()
        << "usage: pycircuit-link-acir (--body BODY --header HEADER)... "
           "--top NAME --output PATH\n"
           "       pycircuit-link-acir --verify-only LINKED.ac\n";
    return 2;
  }

  DialectRegistry dialects;
  dialects
      .insert<acir::ac::ACIRDialect, arith::ArithDialect, func::FuncDialect>();
  MLIRContext context(dialects);
  if (verifyOnly) {
    auto package = parseSourceFile<ModuleOp>(input.getValue(), &context);
    return !package || failed(acir::compiler::verifyLinkedSourceACIR(*package));
  }

  SmallVector<OwningOpRef<ModuleOp>> inputs;
  SmallVector<acir::compiler::SourceLinkUnit> units;
  for (auto [bodyPath, headerPath] : llvm::zip(bodies, headers)) {
    auto body = parseSourceFile<ModuleOp>(bodyPath, &context);
    auto header = parseSourceFile<ModuleOp>(headerPath, &context);
    if (!body || !header)
      return 1;
    units.push_back({*body, *header});
    inputs.push_back(std::move(body));
    inputs.push_back(std::move(header));
  }
  auto linked = acir::compiler::linkSourceACIR(units, top.getValue(), [&] {
    return emitError(UnknownLoc::get(&context));
  });
  return failed(linked) || failed(publish(**linked, output.getValue()));
}
