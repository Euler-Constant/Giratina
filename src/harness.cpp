#include "harness.h"

#include <exception>
#include <vector>

#include "cfg.h"
#include "intervals.h"
#include "liveness.h"
#include "validate.h"

namespace ls {

AllocProblem make_problem(const Function& f, int R, Numbering numbering) {
  const Liveness live = compute_liveness(f);
  const std::vector<int> order =
      numbering == Numbering::DepthFirst ? depth_first_order(f) : layout_order(f);

  AllocProblem p;
  p.num_vars = f.num_vars;
  p.R = R;
  p.intervals = build_intervals(f, live, order);
  return p;
}

namespace {

// "[1, 2, 3, ...]" for the first few values, so a mismatch message stays readable.
std::string show(const std::vector<long>& v) {
  std::string s = "[";
  for (size_t i = 0; i < v.size() && i < 8; ++i) s += (i ? ", " : "") + std::to_string(v[i]);
  if (v.size() > 8) s += ", ... (" + std::to_string(v.size()) + " values)";
  return s + "]";
}

}  // namespace

CheckResult check_allocation(const Function& f, int R, const Allocator& allocate,
                             const CheckOptions& options) {
  CheckResult r;
  auto fail = [&](const std::string& why) {
    r.ok = false;
    r.why = why;
    return r;
  };

  // 1. Problem and allocation.
  AllocProblem p;
  try {
    p = make_problem(f, R, options.numbering);
  } catch (const std::exception& e) {
    return fail(std::string("building the allocation problem failed: ") + e.what());
  }

  Allocation a;
  try {
    a = allocate(p);
  } catch (const std::exception& e) {
    return fail(std::string("allocator threw: ") + e.what());
  }
  r.num_spills = a.num_spills;

  // 2. Static validator.
  if (options.run_validator) {
    std::string why;
    if (!validate_allocation(p, a, &why)) return fail("validator: " + why);
  }

  // 3. Ground truth.
  std::vector<long> want;
  try {
    want = run_reference(f, &r.reference);
  } catch (const std::exception& e) {
    return fail(std::string("reference run failed: ") + e.what());
  }

  // 4. The allocated run, on a VM with exactly R allocatable registers.
  VmConfig cfg;
  cfg.R = R;
  std::vector<long> got;
  try {
    got = run_allocated(f, a, cfg, &r.allocated);
  } catch (const std::exception& e) {
    return fail(std::string("allocated run failed: ") + e.what());
  }

  // 5. Same output?
  if (got != want)
    return fail("output differs: reference printed " + show(want) + ", allocated run printed " + show(got));

  // 6. Cost accounting.
  const RunStats& ref = r.reference;
  const RunStats& alloc = r.allocated;
  if (ref.spill_loads != 0 || ref.spill_stores != 0)
    return fail("reference run reported spill traffic, so the reference interpreter is wrong");
  if (alloc.executed != ref.executed + alloc.spill_loads + alloc.spill_stores)
    return fail("executed count does not add up: " + std::to_string(alloc.executed) + " != " +
                std::to_string(ref.executed) + " + " + std::to_string(alloc.spill_loads) + " loads + " +
                std::to_string(alloc.spill_stores) + " stores");
  if (alloc.cycles != ref.cycles + alloc.spill_loads * cfg.cost_load + alloc.spill_stores * cfg.cost_store)
    return fail("cycle count does not add up for the spill traffic");
  if (a.num_spills == 0 && (alloc.spill_loads != 0 || alloc.spill_stores != 0))
    return fail("allocator reports no spills, yet the run did spill loads or stores");

  r.ok = true;
  return r;
}

}  // namespace ls
