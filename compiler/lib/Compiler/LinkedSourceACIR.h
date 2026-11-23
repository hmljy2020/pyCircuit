#ifndef ACIR_LIB_COMPILER_LINKEDSOURCEACIR_H
#define ACIR_LIB_COMPILER_LINKEDSOURCEACIR_H

#include "SourceLink.h"

namespace acir::compiler {

// Assembly shared by native linkers, after source-unit admission.
mlir::FailureOr<mlir::OwningOpRef<mlir::ModuleOp>>
mergeSourceLinkDefinitions(llvm::ArrayRef<SourceLinkUnit> units);
ac::ModuleOp findSourceLinkEntry(mlir::ModuleOp package, llvm::StringRef top,
                                 ac::detail::EmitError emitError);
void addSourceLinkEntry(mlir::ModuleOp package, ac::ModuleOp root,
                        llvm::ArrayRef<mlir::Attribute> arguments,
                        llvm::StringRef resetSemantics);

mlir::FailureOr<mlir::OwningOpRef<mlir::ModuleOp>>
linkSourceACIR(llvm::ArrayRef<SourceLinkUnit> units, llvm::StringRef top,
               ac::detail::EmitError emitError);

mlir::LogicalResult verifyLinkedSourceACIR(mlir::ModuleOp package);

} // namespace acir::compiler

#endif
