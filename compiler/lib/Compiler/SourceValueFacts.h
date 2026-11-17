#ifndef PYCIRCUIT_COMPILER_SOURCEVALUEFACTS_H
#define PYCIRCUIT_COMPILER_SOURCEVALUEFACTS_H

#include "NumericLowering.h"
#include "TableQueryLowering.h"
#include "llvm/ADT/DenseMap.h"
#include <vector>

namespace acir::compiler::detail {
// Private importer facts. Keys are opaque SSA identities during undo: temporary
// IR may already be gone, so restoration must never inspect a Value's type.
class SourceValueFacts {
public:
  struct State {
    std::optional<NumericValue> numeric;
    bool fixed = false, arithmetic = false;
    uint64_t checkpoint = 0;
    bool empty() const { return !numeric && !fixed && !arithmetic; }
  };
  struct Checkpoint {
    uint64_t id, parent;
    size_t start;
  };
  explicit SourceValueFacts(TableQueryBudget &budget) : budget(budget) {}
  const NumericValue *find(mlir::Value value) const {
    auto it = entries.find(value);
    return it == entries.end() || !it->second.numeric ? nullptr
                                                      : &*it->second.numeric;
  }
  bool isFixed(mlir::Value value) const {
    auto it = entries.find(value);
    return it != entries.end() && it->second.fixed;
  }
  bool isArithmetic(mlir::Value value) const {
    auto it = entries.find(value);
    return it != entries.end() && it->second.arithmetic;
  }
  mlir::LogicalResult record(NumericValue info, std::string &reason) {
    const auto &old = state(info.value);
    return set(info.value, &info, old.fixed, old.arithmetic, reason, &info);
  }
  mlir::LogicalResult forgetNumeric(mlir::Value value, std::string &reason) {
    const auto &old = state(value);
    return set(value, nullptr, old.fixed, old.arithmetic, reason);
  }
  mlir::LogicalResult markFixed(mlir::Value value, bool enabled,
                                std::string &reason) {
    const auto &old = state(value);
    return set(value, old.numeric ? &*old.numeric : nullptr, enabled,
               old.arithmetic, reason);
  }
  mlir::LogicalResult markArithmetic(mlir::Value value, bool enabled,
                                     std::string &reason) {
    const auto &old = state(value);
    return set(value, old.numeric ? &*old.numeric : nullptr, old.fixed, enabled,
               reason);
  }
  mlir::LogicalResult forget(mlir::Value value, std::string &reason) {
    return set(value, nullptr, false, false, reason);
  }
  mlir::LogicalResult map(mlir::Value original, mlir::Value mapped, bool sparse,
                          bool enumeration, std::string &reason) {
    const auto &from = state(original), &old = state(mapped);
    bool meaningful =
        from.numeric && (from.numeric->sourceKind || from.numeric->interval ||
                         from.numeric->closedSourceConstant);
    const NumericValue empty{mapped, {}, {}};
    const NumericValue *numeric = old.numeric ? &*old.numeric : nullptr;
    if (!sparse || meaningful)
      numeric =
          enumeration ? nullptr : (from.numeric ? &*from.numeric : &empty);
    return set(mapped, numeric, old.fixed || from.fixed,
               old.arithmetic || from.arithmetic, reason);
  }
  Checkpoint checkpoint() {
    Checkpoint result{++serial, active, undo.size()};
    active = result.id;
    return result;
  }
  void commit(Checkpoint point) {
    assert(active == point.id);
    active = point.parent;
    if (!active)
      release();
  }
  void rollback(Checkpoint point) {
    assert(active == point.id);
    while (undo.size() > point.start) {
      auto &record = undo.back();
      auto it = entries.find(record.value);
      assert(it != entries.end());
      it->second = std::move(record.old);
      if (it->second.empty() && !it->second.checkpoint)
        entries.erase(it);
      undo.pop_back();
    }
    active = point.parent;
    if (!active)
      release();
  }

private:
  struct Undo {
    mlir::Value value;
    State old;
  };
  const State &state(mlir::Value value) const {
    static const State empty;
    auto it = entries.find(value);
    return it == entries.end() ? empty : it->second;
  }
  static bool sameInteger(const llvm::APSInt &a, const llvm::APSInt &b) {
    return a.getBitWidth() == b.getBitWidth() &&
           a.isUnsigned() == b.isUnsigned() && a == b;
  }
  static bool equal(const NumericValue *x, const NumericValue *y) {
    if (!x || !y)
      return x == y;
    return x->sourceKind == y->sourceKind &&
           x->closedSourceConstant == y->closedSourceConstant &&
           bool(x->interval) == bool(y->interval) &&
           (!x->interval ||
            (sameInteger(x->interval->lower, y->interval->lower) &&
             sameInteger(x->interval->upper, y->interval->upper)));
  }
  mlir::LogicalResult set(mlir::Value value, const NumericValue *numeric,
                          bool fixed, bool arithmetic, std::string &reason,
                          NumericValue *owned = nullptr) {
    auto it = entries.find(value);
    const State &old = state(value);
    if (old.fixed == fixed && old.arithmetic == arithmetic &&
        equal(old.numeric ? &*old.numeric : nullptr, numeric))
      return mlir::success();
    if (active && old.checkpoint != active) {
      TableQueryCharge charge;
      bool fits = TableQueryBudget::add(charge, TableQueryResource::Slots,
                                        (sizeof(Undo) + 7) / 8 * 2) &&
                  TableQueryBudget::add(charge, TableQueryResource::Work, 1);
      if (old.numeric && old.numeric->interval)
        for (const llvm::APSInt *endpoint :
             {&old.numeric->interval->lower, &old.numeric->interval->upper})
          fits &=
              TableQueryBudget::add(charge, TableQueryResource::PayloadWords,
                                    (endpoint->getBitWidth() + 63ULL) / 64);
      if (!fits || !budget.reserve(charge, reason)) {
        if (!fits)
          reason = "facts undo resource counter exhausted";
        return mlir::failure();
      }
      undo.push_back({value, old});
    }
    // Flag-only changes retain numeric storage in place after reserving undo.
    if (it != entries.end() &&
        numeric == (old.numeric ? &*old.numeric : nullptr)) {
      it->second.fixed = fixed;
      it->second.arithmetic = arithmetic;
      it->second.checkpoint = active;
      if (!active && it->second.empty())
        entries.erase(it);
      return mlir::success();
    }
    State next;
    if (numeric) {
      next.numeric = owned ? std::move(*owned) : *numeric;
      next.numeric->value = value;
    }
    next.fixed = fixed;
    next.arithmetic = arithmetic;
    next.checkpoint = active;
    if (!active && next.empty()) {
      if (it != entries.end())
        entries.erase(it);
    } else
      entries[value] = std::move(next);
    return mlir::success();
  }
  void release() {
    // Only touched entries can be logical tombstones. Duplicate keys are safe.
    for (const auto &record : undo) {
      auto it = entries.find(record.value);
      if (it == entries.end())
        continue;
      if (it->second.empty())
        entries.erase(it);
      else
        it->second.checkpoint = 0;
    }
    undo.clear();
  }
  TableQueryBudget &budget;
  llvm::DenseMap<mlir::Value, State> entries;
  std::vector<Undo> undo;
  uint64_t serial = 0, active = 0;
};
} // namespace acir::compiler::detail
#endif
