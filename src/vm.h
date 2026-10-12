#pragma once
#include <vector>

#include "alloc.h"
#include "ir.h"

namespace ls {

struct VmConfig {
  int R = 8;        // allocatable registers
  int scratch = 2;  // extra registers outside R, reserved for spill code (see DESIGN.md)
  // Cycle weights are placeholders, not taken from the paper.
  int cost_alu = 1;
  int cost_load = 3;
  int cost_store = 3;
};

struct RunStats {
  long executed = 0;      // VM instructions executed, including spill loads and stores
  long cycles = 0;        // weighted by VmConfig costs
  long spill_loads = 0;
  long spill_stores = 0;
};

// Ground truth: runs the function with unlimited virtual registers.
// Returns the values passed to `print`, in order.
std::vector<long> run_reference(const Function& f, RunStats* stats = nullptr);

// Runs the function as allocated: variables in registers are used directly, spilled
// variables are loaded into / stored from the scratch registers around each use/def.
std::vector<long> run_allocated(const Function& f, const Allocation& a, const VmConfig& cfg,
                                RunStats* stats = nullptr);

}  // namespace ls
