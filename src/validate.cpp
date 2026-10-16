#include "validate.h"

namespace ls {

bool validate_allocation(const AllocProblem& p, const Allocation& a, std::string* why) {
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

  const auto& iv = p.intervals;
  for (size_t i = 0; i < iv.size(); ++i) {
    for (size_t j = i + 1; j < iv.size(); ++j) {
      const bool overlap = iv[i].start <= iv[j].end && iv[j].start <= iv[i].end;
      if (!overlap) continue;
      const int ri = a.reg[iv[i].var];
      const int rj = a.reg[iv[j].var];
      if (ri >= 0 && ri == rj)
        return fail("v" + std::to_string(iv[i].var) + " and v" + std::to_string(iv[j].var) +
                    " overlap but share register " + std::to_string(ri));
      // Spilled variables live in stack slots, and two live at the same time need
      // different slots just as much as they need different registers.
      const int si = a.slot[iv[i].var];
      const int sj = a.slot[iv[j].var];
      if (si >= 0 && si == sj)
        return fail("v" + std::to_string(iv[i].var) + " and v" + std::to_string(iv[j].var) +
                    " overlap but share stack slot " + std::to_string(si));
    }
  }
  return true;
}

}  // namespace ls
