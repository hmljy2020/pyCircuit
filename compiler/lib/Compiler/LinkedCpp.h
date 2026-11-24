#ifndef ACIR_LIB_COMPILER_LINKEDCPP_H
#define ACIR_LIB_COMPILER_LINKEDCPP_H

#include "mlir/IR/BuiltinOps.h"
#include "llvm/Support/raw_ostream.h"

namespace acir::compiler {
mlir::LogicalResult emitLinkedCpp(mlir::ModuleOp package,
                                  llvm::raw_ostream &out);
}

#endif
