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

// Simple graph coloring: no coalescing, spill choice by reference count (paper 5.1.1).
Allocation allocate_graph_coloring(const AllocProblem& p);

}  // namespace ls
