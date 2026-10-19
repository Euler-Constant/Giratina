#include <algorithm>
#include <stdexcept>
#include <string>
#include <vector>

#include "alloc.h"

namespace ls {

namespace {

// State of one linear scan, with the paper's Figure 1 routines as member functions so the
// code can be read next to the figure.
class LinearScan {
 public:
  LinearScan(const AllocProblem& p) : p_(p), R_(std::max(p.R, 0)), a_(p.num_vars) {
    // The pool of free registers. It is a stack, so the most recently freed register is
    // reused first, and register 0 is handed out first.
    for (int r = R_ - 1; r >= 0; --r) free_regs_.push_back(r);
  }

  // LinearScanRegisterAllocation
  Allocation run() {
    check_input();
    for (const Interval& i : p_.intervals) {  // in order of increasing start point
      expire_old_intervals(i);
      if (static_cast<int>(active_.size()) == R_) {
        spill_at_interval(i);
      } else {
        a_.reg[i.var] = free_regs_.back();  // a register removed from the pool of free registers
        free_regs_.pop_back();
        add_to_active(i);
      }
    }
    return a_;
  }

 private:
  // The algorithm's two assumptions, checked so that a violation fails loudly instead of
  // quietly producing a wrong allocation: every interval names a real variable, and the
  // intervals are sorted by increasing start point.
  void check_input() const {
    for (size_t k = 0; k < p_.intervals.size(); ++k) {
      const Interval& iv = p_.intervals[k];
      if (iv.var < 0 || iv.var >= p_.num_vars)
        throw std::invalid_argument("linear scan: interval " + std::to_string(k) + " names a variable that does not exist");
      if (k > 0 && iv.start < p_.intervals[k - 1].start)
        throw std::invalid_argument("linear scan: intervals must be sorted by increasing start point");
    }
  }

  // ExpireOldIntervals(i): intervals in `active` that end before i starts no longer overlap
  // it, so their registers go back to the pool. `active` is sorted by end point, so the ones
  // to remove are exactly a prefix, and the scan stops at the first interval that is still live.
  // An interval ending at i's start point DOES overlap it (endpoints are inclusive), so it stays.
  void expire_old_intervals(const Interval& i) {
    size_t expired = 0;
    while (expired < active_.size() && active_[expired]->end < i.start) {
      free_regs_.push_back(a_.reg[active_[expired]->var]);
      ++expired;
    }
    active_.erase(active_.begin(), active_.begin() + static_cast<std::ptrdiff_t>(expired));
  }

  // SpillAtInterval(i): all R registers are taken by intervals that overlap i, so one of
  // those R intervals, or i itself, must live on the stack. Spill whichever ends last.
  void spill_at_interval(const Interval& i) {
    // With R == 0 `active` is empty and there is nothing to take a register from.
    // On a tie the new interval is the one spilled (the figure's test is a strict >).
    if (active_.empty() || active_.back()->end <= i.end) {
      spill(i);  // location[i] <- new stack location
      return;
    }
    const Interval* victim = active_.back();  // the last interval in active
    a_.reg[i.var] = a_.reg[victim->var];      // register[i] <- register[spill]
    spill(*victim);                           // location[spill] <- new stack location
    active_.pop_back();                       // remove spill from active
    add_to_active(i);
  }

  // A spilled variable lives on the stack for its whole lifetime (there is no splitting),
  // so a variable that already had a register loses it.
  void spill(const Interval& iv) {
    a_.reg[iv.var] = -1;
    a_.slot[iv.var] = next_slot_++;
    ++a_.num_spills;
  }

  // "add i to active, sorted by increasing end point": a linear search for the insertion
  // point, as in the paper's implementation. It looks from the back because new intervals
  // tend to end late. Among equal end points the new interval goes last.
  void add_to_active(const Interval& i) {
    auto pos = active_.end();
    while (pos != active_.begin() && (*(pos - 1))->end > i.end) --pos;
    active_.insert(pos, &i);
  }

  const AllocProblem& p_;
  const int R_;
  Allocation a_;
  std::vector<int> free_regs_;
  std::vector<const Interval*> active_;
  int next_slot_ = 0;
};

}  // namespace

// Linear scan register allocation (Poletto & Sarkar, Fig. 1). One pass over the intervals in
// order of increasing start point, keeping the intervals that overlap the current point and
// hold a register in `active`, sorted by end point. When all R registers are in use, the
// interval that ends furthest away is spilled.
Allocation allocate_linear_scan(const AllocProblem& p) { return LinearScan(p).run(); }

}  // namespace ls
