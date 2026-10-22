#include "validate.h"

namespace ls {

bool validate_allocation(const AllocProblem& p, const Allocation& a, std::string* why, SharingRule rule) {
  auto fail = [&](const std::string& msg) {
    if (why) *why = msg;
    return false;
  };

  if (static_cast<int>(a.reg.size()) != p.num_vars || static_cast<int>(a.slot.size()) != p.num_vars)
    return fail("allocation size does not match num_vars");

  int spilled = 0;
  for (const Interval& iv : p.intervals) {
    if (iv.var < 0 || iv.var >= p.num_vars) return fail("interval refers to an unknown variable");
    const std::string v = "v" + std::to_string(iv.var);
    const int r = a.reg[iv.var];
    const int s = a.slot[iv.var];
    if (r >= p.R) return fail(v + " assigned register " + std::to_string(r) + " but R = " + std::to_string(p.R));
    if (r < 0 && s < 0) return fail(v + " has neither a register nor a stack slot");
    if (r >= 0 && s >= 0) return fail(v + " has both a register and a stack slot");
    if (r < 0) ++spilled;
  }
  if (spilled != a.num_spills)
    return fail("num_spills is " + std::to_string(a.num_spills) + " but " + std::to_string(spilled) +
                " variables have no register");

  // Two variables that conflict need different registers, and if both are spilled, different
  // stack slots: a slot is storage just as a register is.
  auto clash = [&](int x, int y, const char* how) {
    const std::string names = "v" + std::to_string(x) + " and v" + std::to_string(y) + how;
    if (a.reg[x] >= 0 && a.reg[x] == a.reg[y]) return fail(names + "share register " + std::to_string(a.reg[x]));
    if (a.slot[x] >= 0 && a.slot[x] == a.slot[y]) return fail(names + "share stack slot " + std::to_string(a.slot[x]));
    return true;
  };

  if (rule == SharingRule::Intervals) {
    const auto& iv = p.intervals;
    for (size_t i = 0; i < iv.size(); ++i)
      for (size_t j = i + 1; j < iv.size(); ++j) {
        const bool overlap = iv[i].start <= iv[j].end && iv[j].start <= iv[i].end;
        if (overlap && !clash(iv[i].var, iv[j].var, " overlap but ")) return false;
      }
    return true;
  }

  // Interference: every edge, in both directions, so a lopsided graph is not silently half-checked.
  if (static_cast<int>(p.interference.size()) != p.num_vars)
    return fail("the problem has no interference graph to validate against");
  for (int u = 0; u < p.num_vars; ++u)
    for (int v : p.interference[u]) {
      if (v < 0 || v >= p.num_vars) return fail("interference graph names an unknown variable");
      if (v == u) return fail("v" + std::to_string(u) + " interferes with itself");
      if (!clash(u, v, " interfere but ")) return false;
    }
  return true;
}

}  // namespace ls
