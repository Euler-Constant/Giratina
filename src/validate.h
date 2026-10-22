#pragma once
#include <string>

#include "alloc.h"

namespace ls {

// What counts as "may not share a register or a stack slot".
enum class SharingRule {
  // Two variables whose coarse intervals overlap. This is linear scan's own contract, and it is
  // conservative: an interval covers a variable's whole span, holes included.
  Intervals,
  // Two variables joined by an edge of AllocProblem::interference. This is graph coloring's
  // contract and is more precise: variables that are never live together may share even when
  // their intervals overlap. Needs the problem's interference lists.
  Interference,
};

// Static check of an allocation against the problem it was computed from:
//  - every variable in the problem has exactly one home (a register in [0, R) or a stack slot)
//  - num_spills equals the number of variables without a register
//  - no two variables that conflict under `rule` share a register or a stack slot
// Returns false and fills *why (if non-null) on the first violation. O(n^2) for Intervals: never
// time this.
bool validate_allocation(const AllocProblem& p, const Allocation& a, std::string* why,
                         SharingRule rule = SharingRule::Intervals);

}  // namespace ls
