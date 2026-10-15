#pragma once
#include <vector>

#include "interval.h"
#include "ir.h"
#include "liveness.h"

namespace ls {

// The "allocation setup" step for linear scan (Fig. 3): number the instructions along
// block_order, then coarsen liveness into one [start, end] interval per variable.
// Returns intervals sorted by increasing start. Fills Interval::weight.
std::vector<Interval> build_intervals(const Function& f, const Liveness& live,
                                      const std::vector<int>& block_order);

}  // namespace ls
