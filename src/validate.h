#pragma once
#include <string>

#include "alloc.h"

namespace ls {

// Static check of an allocation against the intervals it was computed from:
//  - every variable in the problem has exactly one home (a register in [0, R) or a stack slot)
//  - no two overlapping intervals share a register
//  - num_spills equals the number of variables without a register
// Returns false and fills *why (if non-null) on the first violation. O(n^2): never time this.
bool validate_allocation(const AllocProblem& p, const Allocation& a, std::string* why);

}  // namespace ls
