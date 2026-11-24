#include "Compiler/LinkedCpp.h"
#include "mlir/Dialect/Arith/IR/Arith.h"
#include "mlir/Dialect/Func/IR/FuncOps.h"
#include "mlir/Parser/Parser.h"
#include "pycircuit/Dialect/ACIR/ACIRDialect.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/FileSystem.h"

using namespace mlir;

namespace {
namespace cl = llvm::cl;
cl::opt<std::string> input(cl::Positional, cl::Required,
                           cl::desc("inferred linked-source ACIR"));
cl::opt<std::string> output("output", cl::Required,
                            cl::desc("Fresh generated C++ header"));
} // namespace

int main(int argc, char **argv) {
  cl::ParseCommandLineOptions(argc, argv, "pyCircuit t-gfsim emitter\n");
  if (output.empty() || output == "-") {
    llvm::errs() << "error: --output requires a fresh file path\n";
    return 2;
  }
  DialectRegistry registry;
  registry
      .insert<acir::ac::ACIRDialect, arith::ArithDialect, func::FuncDialect>();
  MLIRContext context(registry);
  auto package = parseSourceFile<ModuleOp>(input.getValue(), &context);
  std::string cpp;
  llvm::raw_string_ostream buffer(cpp);
  if (!package || failed(acir::compiler::emitLinkedCpp(*package, buffer)))
    return 1;

  int descriptor;
  if (auto error = llvm::sys::fs::openFileForWrite(output, descriptor,
                                                   llvm::sys::fs::CD_CreateNew,
                                                   llvm::sys::fs::OF_None)) {
    llvm::errs() << "error: cannot create output '" << output
                 << "': " << error.message() << '\n';
    return 1;
  }
  llvm::raw_fd_ostream stream(descriptor, true);
  stream << cpp;
  stream.close();
  if (!stream.has_error())
    return 0;
  llvm::errs() << "error: cannot write output '" << output
               << "': " << stream.error().message() << '\n';
  stream.clear_error();
  if (auto error = llvm::sys::fs::remove(output))
    llvm::errs() << "error: cannot remove incomplete output '" << output
                 << "': " << error.message() << '\n';
  return 1;
}
