// SPDX-License-Identifier: BSD-3-Clause
#pragma once

#include "gfsim/bits.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <limits>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace gfsim::t_gfsim {

struct Observation {
  std::string_view event;
  std::string value;
};

struct Sample {
  std::string error;
  std::vector<Observation> observations;

  void Fail(std::string_view message) {
    if (error.empty())
      error = message;
  }
};

struct Transaction {
  Sample &sample;
  std::size_t id;
};

// Source fields occupy disjoint intervals in the declared packed layout.
// The candidate read by a Rule contains only its own prior writes. The merged
// proposal is used exclusively at the system-wide Xfer barrier.
template <class T> class State {
  struct Candidate {
    std::size_t owner;
    T value;
  };

  struct WriteRange {
    std::size_t owner;
    std::uint64_t low;
    std::uint64_t high;
  };

public:
  explicit State(std::string_view name) : name_(name) {}

  const T &Q() const { return committed_; }

  void Initialize(const T &initial) { committed_ = initial_ = initial; }

  const T &Read(const Transaction &txn) const {
    auto candidate = std::ranges::find(candidates_, txn.id, &Candidate::owner);
    return candidate == candidates_.end() ? committed_ : candidate->value;
  }

  template <class Update>
  void Write(Transaction &txn, std::uint64_t low, std::uint64_t high,
             Update update) {
    for (const auto &range : ranges_)
      if (range.owner != txn.id && low < range.high && range.low < high) {
        txn.sample.Fail("overlapping state proposals for " +
                        std::string(name_));
        return;
      }

    if (ranges_.empty())
      merged_ = committed_;
    auto candidate = std::ranges::find(candidates_, txn.id, &Candidate::owner);
    if (candidate == candidates_.end()) {
      candidates_.push_back({txn.id, committed_});
      candidate = std::prev(candidates_.end());
    }
    update(candidate->value);
    update(merged_);
    ranges_.push_back({txn.id, low, high});
  }

  void Xfer(bool reset) noexcept {
    if (reset)
      committed_ = initial_;
    else if (!ranges_.empty())
      committed_ = merged_;
    Discard();
  }

  void Discard() noexcept {
    ranges_.clear();
    candidates_.clear();
  }

private:
  std::string_view name_;
  T initial_{};
  T committed_{};
  T merged_{};
  std::vector<Candidate> candidates_;
  std::vector<WriteRange> ranges_;
};

template <class T> class Queue {
  struct Entry {
    T value{};
    std::uint64_t deadline = 0;
  };

public:
  Queue(std::string_view name, std::size_t depth, std::uint64_t latency)
      : name_(name), entries_(depth), latency_(latency) {}

  bool CanPush() const { return count_ < entries_.size(); }
  bool CanPop() const { return ready_ != 0; }

  void Push(Transaction &txn, T value) {
    if (!CanPush() || push_) {
      txn.sample.Fail("conflicting Queue push for " + std::string(name_));
      return;
    }
    pending_ = value;
    push_ = true;
  }

  T Peek(Transaction &txn) const {
    if (CanPop())
      return entries_[read_].value;
    txn.sample.Fail("unavailable Queue head for " + std::string(name_));
    return {};
  }

  T Pop(Transaction &txn) {
    if (pop_ || !CanPop()) {
      txn.sample.Fail("conflicting Queue pop for " + std::string(name_));
      return {};
    }
    pop_ = true;
    return entries_[read_].value;
  }

  void Xfer(bool reset) noexcept {
    if (reset) {
      read_ = write_ = immature_ = count_ = ready_ = 0;
      tick_ = 0;
    } else {
      ++tick_;
      if (ready_ < count_ && entries_[immature_].deadline == tick_) {
        advance(immature_);
        ++ready_;
      }
      if (pop_) {
        advance(read_);
        --ready_;
        --count_;
      }
      if (push_) {
        entries_[write_] = {pending_, tick_ + (latency_ - 1)};
        advance(write_);
        ++count_;
        if (latency_ == 1) {
          advance(immature_);
          ++ready_;
        }
      }
    }
    Discard();
  }

  void Discard() noexcept { push_ = pop_ = false; }

private:
  void advance(std::size_t &index) const {
    if (++index == entries_.size())
      index = 0;
  }

  std::string_view name_;
  std::vector<Entry> entries_;
  std::uint64_t latency_;
  std::uint64_t tick_ = 0;
  std::size_t read_ = 0, write_ = 0, immature_ = 0, count_ = 0, ready_ = 0;
  T pending_{};
  bool push_ = false, pop_ = false;
};

template <unsigned Width> unsigned ShiftAmount(Bits<Width> value) {
  constexpr auto max = std::numeric_limits<unsigned>::max();
  for (unsigned i = 1; i < Bits<Width>::kWords; ++i)
    if (value.word(i) != 0)
      return max;
  return value.value() > max ? max : static_cast<unsigned>(value.value());
}

template <unsigned Width> bool SignedLess(Bits<Width> lhs, Bits<Width> rhs) {
  const bool lhsNegative = lhs.bit(Width - 1);
  const bool rhsNegative = rhs.bit(Width - 1);
  return lhsNegative == rhsNegative ? lhs < rhs : lhsNegative;
}

template <unsigned Width> std::string Format(Bits<Width> bits) {
  std::string text = "0b";
  for (unsigned i = Width; i > 0; --i)
    text += bits.bit(i - 1) ? '1' : '0';
  return text;
}

} // namespace gfsim::t_gfsim
