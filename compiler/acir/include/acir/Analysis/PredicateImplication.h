#ifndef ACIR_ANALYSIS_PREDICATEIMPLICATION_H
#define ACIR_ANALYSIS_PREDICATEIMPLICATION_H

#include "acir/Dialect/ACIR/ACIROps.h"
#include "llvm/ADT/DenseSet.h"
#include "llvm/ADT/SmallVector.h"
#include <optional>

namespace acir {

// A deliberately conservative proof over typed Boolean DAGs. Unknown nodes
// are opaque identities. Each node is visited once, including shared DAGs.
// Adapters independently inspect live SSA or the frozen QueueGraph plan.
template <typename Node, typename IsBool, typename Constant, typename Conjuncts>
bool provesPredicateImplication(Node present, Node candidate, IsBool isBool,
                                Constant constant, Conjuncts conjuncts) {
  if (!isBool(present) || !isBool(candidate))
    return false;
  if (present == candidate || constant(candidate) == true)
    return true;
  llvm::DenseSet<Node> facts;
  llvm::SmallVector<Node> pending{present};
  while (!pending.empty()) {
    Node node = pending.pop_back_val();
    if (!isBool(node))
      return false;
    if (!facts.insert(node).second)
      continue;
    if (constant(node) == false)
      return true;
    auto operands = conjuncts(node);
    pending.append(operands.begin(), operands.end());
  }
  llvm::DenseSet<Node> visited;
  pending.push_back(candidate);
  while (!pending.empty()) {
    Node node = pending.pop_back_val();
    if (!isBool(node))
      return false;
    if (!visited.insert(node).second || facts.contains(node) ||
        constant(node) == true)
      continue;
    auto operands = conjuncts(node);
    if (operands.empty())
      return false;
    pending.append(operands.begin(), operands.end());
  }
  return true;
}

inline bool provesPredicateImplication(mlir::Value present,
                                       mlir::Value candidate) {
  return provesPredicateImplication(
      present, candidate,
      [](mlir::Value value) {
        auto type = mlir::dyn_cast<ac::VarType>(value.getType());
        return type && type.getElementType().isInteger(1);
      },
      [](mlir::Value value) -> std::optional<bool> {
        auto op = value.getDefiningOp<ac::VarConstantOp>();
        auto attr = op ? mlir::dyn_cast<mlir::IntegerAttr>(op.getValue())
                       : mlir::IntegerAttr();
        return attr ? std::optional<bool>(!attr.getValue().isZero())
                    : std::nullopt;
      },
      [](mlir::Value value) {
        llvm::SmallVector<mlir::Value, 2> operands;
        auto *op = value.getDefiningOp();
        if (op && mlir::isa<ac::VarAndOp, ac::VarMulOp>(op))
          operands.append(op->operand_begin(), op->operand_end());
        return operands;
      });
}

} // namespace acir
#endif // ACIR_ANALYSIS_PREDICATEIMPLICATION_H
