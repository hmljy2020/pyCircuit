#include "Compiler/SourceValueFacts.h"
#include "mlir/IR/Block.h"
#include "mlir/IR/Builders.h"
#include "gtest/gtest.h"

namespace acir::compiler::detail {
namespace {

using mlir::failed;
using mlir::succeeded;
using Kind = ac::detail::ValueKind;
using Resource = TableQueryResource;

class SourceValueFactsTest : public ::testing::Test {
protected:
  mlir::MLIRContext context;
  mlir::Builder builder{&context};
  mlir::Block values;
  TableQueryBudget budget;
  SourceValueFacts facts{budget};
  std::string reason;

  mlir::Value value() {
    return values.addArgument(builder.getI64Type(), builder.getUnknownLoc());
  }
  NumericValue number(mlir::Value value, uint64_t lower, uint64_t upper) {
    // Endpoints exceed one machine word, exercising owned APSInt storage.
    return {value, Kind::Integer,
            IntegerInterval{llvm::APSInt(llvm::APInt(192, lower), true),
                            llvm::APSInt(llvm::APInt(192, upper), true)},
            builder.getI64IntegerAttr(lower)};
  }
  void expectNumber(mlir::Value value, uint64_t lower, uint64_t upper) {
    auto *info = facts.find(value);
    ASSERT_NE(info, nullptr);
    EXPECT_EQ(info->value, value);
    EXPECT_EQ(info->sourceKind, Kind::Integer);
    ASSERT_TRUE(info->interval);
    EXPECT_EQ(info->interval->lower, llvm::APSInt(llvm::APInt(192, lower), true));
    EXPECT_EQ(info->interval->upper, llvm::APSInt(llvm::APInt(192, upper), true));
    EXPECT_EQ(info->closedSourceConstant, builder.getI64IntegerAttr(lower));
  }
  void expectAbsent(mlir::Value value) {
    EXPECT_EQ(facts.find(value), nullptr);
    EXPECT_FALSE(facts.isFixed(value));
    EXPECT_FALSE(facts.isArithmetic(value));
  }
  static auto counters(const TableQueryBudget &ledger) {
    std::array<uint64_t, static_cast<unsigned>(Resource::Count)> result;
    for (unsigned i = 0; i < result.size(); ++i)
      result[i] = ledger.used(static_cast<Resource>(i));
    return result;
  }
  void exhaust(Resource resource) {
    TableQueryCharge charge;
    charge.amounts[static_cast<unsigned>(resource)] =
        TableQueryBudget::limit(resource) - budget.used(resource);
    ASSERT_TRUE(budget.reserve(charge, reason));
  }
};

TEST_F(SourceValueFactsTest, ParentRollbackRestoresCommittedChildChanges) {
  auto old = value(), inserted = value();
  ASSERT_TRUE(succeeded(facts.record(number(old, 3, 11), reason)));
  ASSERT_TRUE(succeeded(facts.markFixed(old, true, reason)));
  auto parent = facts.checkpoint();
  auto narrowed = number(old, 20, 40);
  narrowed.interval = IntegerInterval{llvm::APSInt::getUnsigned(20),
                                     llvm::APSInt::getUnsigned(40)};
  ASSERT_TRUE(succeeded(facts.record(std::move(narrowed), reason)));
  auto child = facts.checkpoint();
  ASSERT_TRUE(succeeded(facts.forget(old, reason)));
  ASSERT_TRUE(succeeded(facts.record(number(inserted, 50, 60), reason)));
  ASSERT_TRUE(succeeded(facts.markArithmetic(inserted, true, reason)));
  facts.commit(child);
  // The parent's next mutation must not lose the child's retained undo state.
  ASSERT_TRUE(succeeded(facts.record(number(old, 70, 90), reason)));
  ASSERT_TRUE(succeeded(facts.markArithmetic(old, true, reason)));
  auto spent = counters(budget);
  facts.rollback(parent);
  expectNumber(old, 3, 11);
  EXPECT_TRUE(facts.isFixed(old));
  EXPECT_FALSE(facts.isArithmetic(old));
  expectAbsent(inserted);
  EXPECT_EQ(counters(budget), spent);
}

TEST_F(SourceValueFactsTest, ChildRollbackAllowsParentOverwriteDeleteAndReinsert) {
  auto current = value(), transient = value();
  ASSERT_TRUE(succeeded(facts.record(number(current, 1, 9), reason)));
  auto parent = facts.checkpoint();
  ASSERT_TRUE(succeeded(facts.record(number(current, 10, 19), reason)));
  ASSERT_TRUE(succeeded(facts.markArithmetic(current, true, reason)));
  auto child = facts.checkpoint();
  ASSERT_TRUE(succeeded(facts.forget(current, reason)));
  ASSERT_TRUE(succeeded(facts.record(number(transient, 30, 39), reason)));
  facts.rollback(child);
  expectNumber(current, 10, 19);
  EXPECT_TRUE(facts.isArithmetic(current));
  expectAbsent(transient);
  ASSERT_TRUE(succeeded(facts.record(number(current, 40, 49), reason)));
  ASSERT_TRUE(succeeded(facts.forget(current, reason)));
  expectAbsent(current);
  ASSERT_TRUE(succeeded(facts.record(number(current, 60, 69), reason)));
  ASSERT_TRUE(succeeded(facts.markFixed(current, true, reason)));
  facts.commit(parent);
  expectNumber(current, 60, 69);
  EXPECT_TRUE(facts.isFixed(current));
  EXPECT_FALSE(facts.isArithmetic(current));
  auto later = facts.checkpoint();
  ASSERT_TRUE(succeeded(facts.forget(current, reason)));
  facts.rollback(later);
  expectNumber(current, 60, 69);
}

TEST_F(SourceValueFactsTest, ReservationFailureLeavesFactsAndNestedCheckpointUsable) {
  auto source = value(), destination = value();
  ASSERT_TRUE(succeeded(facts.record(number(source, 2, 8), reason)));
  ASSERT_TRUE(succeeded(facts.markFixed(source, true, reason)));
  ASSERT_TRUE(succeeded(facts.markArithmetic(source, true, reason)));
  ASSERT_TRUE(succeeded(facts.record(number(destination, 80, 100), reason)));
  auto parent = facts.checkpoint();
  ASSERT_TRUE(succeeded(facts.markFixed(destination, true, reason)));
  auto child = facts.checkpoint();
  exhaust(Resource::PayloadWords);
  auto spent = counters(budget);
  // Mapping combines numeric and flag writes into one atomic mutation.
  EXPECT_TRUE(failed(facts.map(source, destination, false, false, reason)));
  expectNumber(destination, 80, 100);
  EXPECT_TRUE(facts.isFixed(destination));
  EXPECT_FALSE(facts.isArithmetic(destination));
  EXPECT_EQ(counters(budget), spent);
  facts.rollback(child);
  // Existing parent undo storage still permits a flag change after refusal.
  ASSERT_TRUE(succeeded(facts.markArithmetic(destination, true, reason)));
  facts.rollback(parent);
  expectNumber(destination, 80, 100);
  EXPECT_FALSE(facts.isFixed(destination));
  EXPECT_FALSE(facts.isArithmetic(destination));
  expectNumber(source, 2, 8);
}

TEST_F(SourceValueFactsTest, CheckpointCostDependsOnChangesRatherThanHistory) {
  auto measure = [&](unsigned history) {
    TableQueryBudget ledger;
    SourceValueFacts store(ledger);
    for (unsigned i = 0; i < history; ++i) {
      auto v = value();
      EXPECT_TRUE(succeeded(store.record(number(v, i, i + 1), reason)));
    }
    auto changed = value();
    EXPECT_TRUE(succeeded(store.record(number(changed, 4, 15), reason)));
    auto before = counters(ledger);
    auto parent = store.checkpoint();
    auto child = store.checkpoint();
    EXPECT_EQ(counters(ledger), before);
    EXPECT_TRUE(succeeded(store.markFixed(changed, true, reason)));
    auto afterChange = counters(ledger);
    EXPECT_TRUE(succeeded(store.markFixed(changed, true, reason)));
    EXPECT_EQ(counters(ledger), afterChange);
    store.commit(child);
    EXPECT_EQ(counters(ledger), afterChange);
    EXPECT_TRUE(succeeded(store.markArithmetic(changed, true, reason)));
    auto after = counters(ledger);
    store.rollback(parent);
    EXPECT_EQ(counters(ledger), after);
    for (unsigned i = 0; i < after.size(); ++i)
      after[i] -= before[i];
    return after;
  };
  auto small = measure(1);
  auto large = measure(1000);
  EXPECT_EQ(small, large);
  EXPECT_GT(small[static_cast<unsigned>(Resource::Slots)], 0u);
  EXPECT_GT(small[static_cast<unsigned>(Resource::PayloadWords)], 0u);
  EXPECT_GT(small[static_cast<unsigned>(Resource::Work)], 0u);
}

} // namespace
} // namespace acir::compiler::detail
