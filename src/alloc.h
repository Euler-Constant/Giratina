#pragma once
#include <vector>

#include "interval.h"

namespace ls {

struct AllocProblem {
  int num_vars = 0;
  int R = 0;                                   // allocatable registers; excludes the VM's scratch registers
  std::vector<Interval> intervals;             // one per variable, sorted by increasing start
  std::vector<std::vector<int>> interference;  // adjacency lists; used by graph coloring only
};

struct Allocation {
  std::vector<int> reg;   // reg[v] in [0, R), or -1 if v lives on the stack
  std::vector<int> slot;  // slot[v] >= 0 if spilled, else -1
  int num_spills = 0;

  explicit Allocation(int n = 0) : reg(n, -1), slot(n, -1) {}
};

// Baseline: the k most-used variables get registers, the rest go on the stack (paper 5.1.1).
Allocation allocate_usage_count(const AllocProblem& p);

// The paper's Fig. 1.
Allocation allocate_linear_scan(const AllocProblem& p);

// How graph coloring picks the variable to set aside when every remaining node has R or more
// neighbours. The paper says only that reference counts are taken into account (Sec. 5.1.1).
enum class SpillChoice {
  LowestWeight,           // the variable with the smallest estimated usage count
  LowestWeightPerDegree,  // Chaitin's ratio: usage count divided by the number of neighbours
};

// Simple graph coloring: no coalescing, spill choice by reference count (paper 5.1.1).
// Needs AllocProblem::interference (see build_interference); uses the intervals only for their
// weights. Same as allocate_graph_coloring_with(p, SpillChoice::LowestWeight).
Allocation allocate_graph_coloring(const AllocProblem& p);
Allocation allocate_graph_coloring_with(const AllocProblem& p, SpillChoice choice);

}  // namespace ls
