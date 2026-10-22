#pragma once
#include <functional>
#include <string>

#include "alloc.h"
#include "ir.h"
#include "validate.h"
#include "vm.h"

namespace ls {

// Which instruction numbering the intervals are built from (paper, Sec. 6.2 / Table IV).
enum class Numbering { DepthFirst, Layout };

// Turns a function into the input every allocator takes: liveness, then a block order,
// then one interval per variable. `R` is the number of allocatable registers (it excludes
// the VM's scratch registers). The interference graph graph coloring needs is built too,
// unless with_interference is false (it does not depend on the numbering, and linear scan
// never looks at it).
AllocProblem make_problem(const Function& f, int R, Numbering numbering = Numbering::DepthFirst,
                          bool with_interference = true);

// Anything that maps a problem to an allocation. allocate_linear_scan and friends fit
// directly, and so do lambdas, which is how the tests plug in throwaway allocators.
using Allocator = std::function<Allocation(const AllocProblem&)>;

struct CheckOptions {
  Numbering numbering = Numbering::DepthFirst;
  // Run validate_allocation before executing anything. Turn this off only to test that
  // the output comparison catches a bad allocation on its own.
  bool run_validator = true;
  // What the validator counts as a conflict. Linear scan promises no two overlapping intervals
  // share a register (Intervals); graph coloring promises the weaker "no two interfering
  // variables do" (Interference), so it must be validated that way or correct colorings that
  // share between overlapping intervals would be rejected.
  SharingRule sharing = SharingRule::Intervals;
};

struct CheckResult {
  bool ok = false;
  std::string why;     // the first thing that went wrong; empty when ok
  int num_spills = 0;  // as reported by the allocator
  RunStats reference;  // filled once the reference run has completed
  RunStats allocated;  // filled once the allocated run has completed
};

// The differential test for one (program, allocator, R) combination:
//   1. build the problem and run the allocator
//   2. static validator: no two conflicting variables (options.sharing) share a register or
//      slot, every variable has one home, num_spills is consistent
//   3. run the program with unlimited virtual registers (ground truth)
//   4. run it again through the allocation on a VM with R registers
//   5. the two must print exactly the same values
//   6. cost accounting must add up: the allocated run executes the reference's
//      instructions plus exactly one instruction per spill load and spill store, and
//      likewise for cycles; a run with no spills costs exactly what the reference costs
// Never throws for a bad allocation or a failing run: those come back as ok == false with
// an explanation in `why`. Performance is irrelevant here, so never time this function.
CheckResult check_allocation(const Function& f, int R, const Allocator& allocate,
                             const CheckOptions& options = CheckOptions());

}  // namespace ls
