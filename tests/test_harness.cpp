// Tests for the differential harness (src/harness.*).
//
// The harness decides whether an allocator is correct, so it has to be tested first: with
// allocators that are correct by construction (they must always pass) and with allocators
// that are deliberately broken (they must always be caught). The real allocators do not
// need to exist yet; everything here is a throwaway lambda.
#include <algorithm>
#include <fstream>
#include <iostream>
#include <random>
#include <sstream>
#include <string>
#include <vector>

#include "common.h"
#include "harness.h"
#include "parser.h"
#include "programs.h"

using namespace ls;

static int g_bad = 0;
static void check(bool ok, const std::string& what) {
  if (!ok) ++g_bad;
  std::cout << (ok ? "ok    " : "FAIL  ") << what << "\n";
}

static bool overlaps(const Interval& a, const Interval& b) { return a.start <= b.end && b.start <= a.end; }

// ---------------------------------------------------------------------------------------
// Allocators that are correct by construction.
// ---------------------------------------------------------------------------------------

// Greedy interval colouring: each interval, in order of start, takes a register that no
// overlapping, already-placed interval uses. With spill_prob > 0 it also spills at random
// even when a register is free, to exercise the spill paths of the VM. With reuse_slots,
// spilled intervals that do not overlap share a stack slot, which only works if the
// intervals really cover every position where the variable is live.
static Allocator greedy(unsigned seed, double spill_prob, bool reuse_slots) {
  return [=](const AllocProblem& p) {
    std::mt19937 rng(seed);
    std::uniform_real_distribution<double> coin(0.0, 1.0);
    Allocation a(p.num_vars);
    std::vector<const Interval*> in_reg, spilled;
    int next_slot = 0;

    for (const Interval& iv : p.intervals) {
      std::vector<bool> used(p.R, false);
      for (const Interval* o : in_reg)
        if (overlaps(*o, iv)) used[a.reg[o->var]] = true;
      std::vector<int> free_regs;
      for (int r = 0; r < p.R; ++r)
        if (!used[r]) free_regs.push_back(r);

      const bool spill = free_regs.empty() || (spill_prob > 0 && coin(rng) < spill_prob);
      if (spill) {
        int slot;
        if (reuse_slots) {  // lowest slot not used by an overlapping spilled interval
          std::vector<bool> taken(spilled.size() + 1, false);
          for (const Interval* o : spilled)
            if (overlaps(*o, iv)) taken[a.slot[o->var]] = true;
          slot = 0;
          while (taken[slot]) ++slot;
        } else {
          slot = next_slot++;
        }
        a.slot[iv.var] = slot;
        ++a.num_spills;
        spilled.push_back(&iv);
      } else {
        a.reg[iv.var] = spill_prob > 0 ? free_regs[rng() % free_regs.size()] : free_regs[0];
        in_reg.push_back(&iv);
      }
    }
    return a;
  };
}

static Allocator spill_everything() {
  return [](const AllocProblem& p) {
    Allocation a(p.num_vars);
    for (const Interval& iv : p.intervals) a.slot[iv.var] = a.num_spills++;
    return a;
  };
}

// ---------------------------------------------------------------------------------------
// Allocators that are deliberately wrong.
// ---------------------------------------------------------------------------------------
static Allocation all_in_register_0(const AllocProblem& p) {
  Allocation a(p.num_vars);
  for (const Interval& iv : p.intervals) a.reg[iv.var] = 0;
  return a;
}
static Allocation all_in_slot_0(const AllocProblem& p) {
  Allocation a(p.num_vars);
  for (const Interval& iv : p.intervals) { a.slot[iv.var] = 0; ++a.num_spills; }
  return a;
}
static Allocation register_out_of_range(const AllocProblem& p) {
  Allocation a(p.num_vars);
  for (const Interval& iv : p.intervals) a.reg[iv.var] = p.R;  // valid registers are 0 .. R-1
  return a;
}

static bool has_overlap(const AllocProblem& p) {
  for (size_t i = 0; i < p.intervals.size(); ++i)
    for (size_t j = i + 1; j < p.intervals.size(); ++j)
      if (overlaps(p.intervals[i], p.intervals[j])) return true;
  return false;
}

int main() {
  const std::vector<int> Rs = {1, 2, 3, 4, 8, 32};
  const Numbering numberings[] = {Numbering::DepthFirst, Numbering::Layout};

  std::ifstream in(std::string(LINSCAN_SOURCE_DIR) + "/tests/sum_loop.ir");
  if (!in) { std::cout << "cannot open tests/sum_loop.ir\n"; return 2; }
  const Function sum_loop = parse_function(in);

  const std::vector<Named> programs = fixed_programs(LINSCAN_SOURCE_DIR);

  // ---- 1. correct allocators pass everywhere, and the sweep really exercises spilling ----
  struct NamedAlloc { std::string name; Allocator fn; };
  const std::vector<NamedAlloc> good = {
      {"greedy", greedy(1, 0.0, false)},
      {"greedy+random spills+slot reuse", greedy(2, 0.4, true)},
      {"greedy+heavy random spills", greedy(3, 0.8, false)},
      {"spill everything", spill_everything()},
  };
  int checks = 0, failures = 0, with_spills = 0;
  std::string first_failure;
  for (const Named& pr : programs) {
    for (const NamedAlloc& al : good) {
      for (int R : Rs) {
        for (Numbering nu : numberings) {
          CheckOptions opt; opt.numbering = nu;
          CheckResult r = check_allocation(pr.f, R, al.fn, opt);
          ++checks;
          if (r.num_spills > 0) ++with_spills;
          if (!r.ok && failures++ == 0) first_failure = pr.name + " / " + al.name + " / R=" + std::to_string(R) + ": " + r.why;
        }
      }
    }
  }
  check(failures == 0, "fixed programs: " + std::to_string(checks) + " combinations of program x allocator x R x numbering all pass (" +
                           std::to_string(with_spills) + " with spills)" + (failures ? "  FIRST FAILURE: " + first_failure : ""));

  {  // with plenty of registers nothing spills and the cost equals the reference exactly
    CheckResult r = check_allocation(sum_loop, 32, greedy(1, 0.0, false));
    check(r.ok && r.num_spills == 0 && r.allocated.executed == r.reference.executed && r.allocated.cycles == r.reference.cycles,
          "sum_loop at R=32: no spills, costs exactly what the reference costs (" + std::to_string(r.reference.executed) + " instructions)");
    CheckResult tight = check_allocation(sum_loop, 2, greedy(1, 0.0, false));
    check(tight.ok && tight.num_spills > 0 && tight.allocated.executed > tight.reference.executed,
          "sum_loop at R=2: " + std::to_string(tight.num_spills) + " spilled, " + std::to_string(tight.allocated.executed) + " executed vs " +
              std::to_string(tight.reference.executed) + " in the reference");
  }

  // ---- 2. broken allocators are caught, with a useful message ----
  {
    auto why_of = [&](const Function& f, int R, const Allocator& a, bool validator) {
      CheckOptions o; o.run_validator = validator;
      CheckResult r = check_allocation(f, R, a, o);
      return std::make_pair(r.ok, r.why);
    };
    auto r1 = why_of(sum_loop, 4, all_in_register_0, true);
    check(!r1.first && r1.second.find("share register") != std::string::npos, "register clash caught by the validator: " + r1.second);
    auto r2 = why_of(sum_loop, 4, all_in_slot_0, true);
    check(!r2.first && r2.second.find("share stack slot") != std::string::npos, "stack-slot clash caught by the validator: " + r2.second);
    auto r3 = why_of(sum_loop, 4, register_out_of_range, true);
    check(!r3.first && r3.second.find("R = 4") != std::string::npos, "register outside R caught: " + r3.second);
    auto r4 = why_of(sum_loop, 4, [](const AllocProblem&) -> Allocation { throw std::runtime_error("boom"); }, true);
    check(!r4.first && r4.second.find("allocator threw: boom") != std::string::npos, "allocator that throws is reported, not propagated: " + r4.second);
    auto r5 = why_of(sum_loop, 4, [](const AllocProblem&) -> Allocation { throw NotImplemented("stub"); }, true);
    check(!r5.first && r5.second.find("allocator threw") != std::string::npos, "an unimplemented stub is reported too: " + r5.second);
    auto r6 = why_of(sum_loop, 4, [](const AllocProblem&) { return Allocation(2); }, true);
    check(!r6.first && r6.second.find("size") != std::string::npos, "wrong-sized allocation caught: " + r6.second);

    Function no_terminator;  // hand-built: the parser would reject this program
    no_terminator.num_vars = 1;
    no_terminator.blocks.resize(1);
    no_terminator.blocks[0].name = "a";
    Instr c; c.op = Op::Const; c.dst = 0; c.imm = 1;
    no_terminator.blocks[0].instrs.push_back(c);
    auto r7 = why_of(no_terminator, 4, greedy(1, 0, false), true);
    check(!r7.first && r7.second.find("reference run failed") != std::string::npos, "a broken program blames the reference run: " + r7.second);

    // Output comparison alone (validator off) on a straight-line program with distinct values.
    const Function distinct = parse("func t\nblock a:\n  v0 = const 3\n  v1 = const 5\n  v2 = add v0 v1\n  v3 = mul v2 v0\n"
                                    "  print v3\n  print v2\n  print v1\n  ret\n");
    auto r8 = why_of(distinct, 4, all_in_register_0, false);
    check(!r8.first && r8.second.find("output differs") != std::string::npos, "validator off, distinct values: the output comparison catches a register clash: " + r8.second);
    auto r9 = why_of(distinct, 4, all_in_slot_0, false);
    check(!r9.first && r9.second.find("output differs") != std::string::npos, "validator off, distinct values: the output comparison catches a slot clash: " + r9.second);

    // ... but it can be fooled when the clobbered variables happen to hold equal values.
    const Function equal_values = parse("func t\nblock a:\n  v0 = const 5\n  v1 = const 5\n  v2 = add v0 v1\n  print v2\n  ret\n");
    auto r10 = why_of(equal_values, 4, all_in_register_0, false);
    auto r11 = why_of(equal_values, 4, all_in_register_0, true);
    check(r10.first, "validator off, EQUAL values: the same register clash goes unnoticed (output is 10 either way)");
    check(!r11.first, "validator on: the same allocation is caught");
  }

  // ---- 3. random terminating programs ----
  {
    std::mt19937 rng(20261016);
    const int kPrograms = 500;
    int combos = 0, bad_combos = 0, spilled_combos = 0, loops = 0;
    long spill_loads = 0, spill_stores = 0, shared_regs = 0, shared_slots = 0;
    std::string first;

    // Counts how often an allocation puts two variables in one register or one stack slot.
    // That is only legal when their intervals are disjoint, so these are the cases that
    // depend on every interval covering everywhere its variable is live.
    auto counting = [&](Allocator inner) {
      return Allocator([&, inner](const AllocProblem& p) {
        Allocation a = inner(p);
        std::vector<int> reg_use(p.R, 0), slot_use(p.num_vars + 1, 0);
        for (int v = 0; v < p.num_vars; ++v) {
          if (a.reg[v] >= 0 && ++reg_use[a.reg[v]] == 2) ++shared_regs;
          if (a.slot[v] >= 0 && ++slot_use[a.slot[v]] == 2) ++shared_slots;
        }
        return a;
      });
    };

    for (int it = 0; it < kPrograms; ++it) {
      const bool with_loop = it % 2 == 0;
      const std::string src = random_program(rng, with_loop);
      const Function f = parse(src);
      if (with_loop) ++loops;
      for (int k = 0; k < static_cast<int>(good.size()); ++k) {
        const Allocator fn = counting(k == 1 ? greedy(it, 0.4, true) : k == 2 ? greedy(it, 0.8, false) : good[k].fn);
        for (int R : Rs) {
          for (Numbering nu : numberings) {
            CheckOptions opt; opt.numbering = nu;
            CheckResult r = check_allocation(f, R, fn, opt);
            ++combos;
            if (r.num_spills > 0) ++spilled_combos;
            spill_loads += r.allocated.spill_loads;
            spill_stores += r.allocated.spill_stores;
            if (!r.ok && bad_combos++ == 0) first = good[k].name + " R=" + std::to_string(R) + ": " + r.why + "\n" + src;
          }
        }
      }
    }
    check(bad_combos == 0, "random programs: " + std::to_string(kPrograms) + " programs (" + std::to_string(loops) + " with a loop), " + std::to_string(combos) +
                               " combinations, " + std::to_string(bad_combos) + " failures" + (bad_combos ? "\n" + first : ""));
    check(spilled_combos > combos / 2 && spill_loads > 0 && spill_stores > 0,
          "spilling was exercised: " + std::to_string(spilled_combos) + " combinations spilled, " + std::to_string(spill_loads) + " spill loads, " + std::to_string(spill_stores) + " spill stores");
    check(shared_regs > 1000 && shared_slots > 100,
          "reuse was exercised: " + std::to_string(shared_regs) + " registers and " + std::to_string(shared_slots) +
              " stack slots were legitimately shared by variables with disjoint intervals");
  }

  // ---- 4. how often does each check catch a broken allocator on random programs? ----
  {
    std::mt19937 rng(77);
    int clashes = 0, validator_caught = 0, validator_wrong = 0, output_only_caught = 0, output_only_missed = 0;
    for (int it = 0; it < 600; ++it) {
      const Function loopy = parse(random_program(rng, true));
      AllocProblem p = make_problem(loopy, 4);
      const bool overlapping = has_overlap(p);
      CheckResult v = check_allocation(loopy, 4, all_in_register_0);  // validator on: never even runs the program
      if (v.ok == overlapping) ++validator_wrong;                       // must fail exactly when intervals overlap
      if (!v.ok) ++validator_caught;

      // Straight-line programs always terminate, so the output comparison can run on its own.
      const Function straight = parse(random_program(rng, false));
      if (has_overlap(make_problem(straight, 4))) {
        ++clashes;
        CheckOptions o; o.run_validator = false;
        if (check_allocation(straight, 4, all_in_register_0, o).ok) ++output_only_missed; else ++output_only_caught;
      }
    }
    check(validator_wrong == 0, "validator on, register clash: fails exactly when two intervals overlap (" + std::to_string(validator_caught) + " of 600 programs have overlaps)");
    check(output_only_caught > 0 && output_only_caught + output_only_missed == clashes,
          "validator off: output comparison alone caught " + std::to_string(output_only_caught) + " of " + std::to_string(clashes) +
              " register clashes and missed " + std::to_string(output_only_missed) + " (that gap is why both checks exist)");
  }

  std::cout << (g_bad ? "FAILED" : "all passed") << "\n";
  return g_bad ? 1 : 0;
}
