#include <algorithm>
#include <vector>

#include "alloc.h"

namespace ls {

// The "usage count" baseline (paper, Sec. 5.1.1): the R variables with the highest
// estimated usage counts each get a register, and every other variable lives on the stack.
//
// Two things make it a weak baseline, and both are on purpose:
//  - A register belongs to its variable for the whole program. Two variables whose
//    intervals do not overlap still never share one, so a register is wasted whenever its
//    variable is not live.
//  - A spilled variable is spilled everywhere: every read is a stack load and every write
//    is a stack store, even in a stretch where a register happens to be free.
// In exchange it is trivial and fast, which is why the paper uses it as a floor to compare
// linear scan against. It only looks at Interval::weight, never at where intervals start or
// end, so the numbering (depth-first or layout) cannot change its result.
Allocation allocate_usage_count(const AllocProblem& p) {
  Allocation a(p.num_vars);

  // Highest estimated usage count first. Ties are broken by variable id, so the same
  // problem always gives the same allocation.
  std::vector<const Interval*> ranked;
  ranked.reserve(p.intervals.size());
  for (const Interval& iv : p.intervals) ranked.push_back(&iv);
  std::sort(ranked.begin(), ranked.end(), [](const Interval* x, const Interval* y) {
    return x->weight != y->weight ? x->weight > y->weight : x->var < y->var;
  });

  // The first R in that order get registers 0, 1, 2, ... (one each, never shared). The
  // rest get their own stack slot. If R is zero or negative nothing gets a register; if R
  // is at least the number of variables nothing is spilled.
  int next_slot = 0;
  for (size_t rank = 0; rank < ranked.size(); ++rank) {
    const int v = ranked[rank]->var;
    if (static_cast<int>(rank) < p.R) {
      a.reg[v] = static_cast<int>(rank);
    } else {
      a.slot[v] = next_slot++;
      ++a.num_spills;
    }
  }
  return a;
}

}  // namespace ls
