#include "liveness.h"

#include <algorithm>

#include "cfg.h"

namespace ls {

// Traditional iterative live variable analysis.
//
// Per block:
//   use[b]  variables read in b before b writes them (they need a value from before b)
//   def[b]  variables written anywhere in b
// Then, until nothing changes:
//   live_out[b] = union of live_in[s] over successors s
//   live_in[b]  = use[b] | (live_out[b] & ~def[b])
Liveness compute_liveness(const Function& f) {
  const int nb = static_cast<int>(f.blocks.size());
  const int nv = f.num_vars;

  Liveness L;
  L.live_in.assign(nb, std::vector<bool>(nv, false));
  L.live_out.assign(nb, std::vector<bool>(nv, false));

  // use[b] and def[b]. Within one instruction the reads happen before the write, so
  // `v0 = add v0 v1` puts v0 in use[b] (if not written earlier in b) and then in def[b].
  std::vector<std::vector<bool>> use(nb, std::vector<bool>(nv, false));
  std::vector<std::vector<bool>> def_set(nb, std::vector<bool>(nv, false));
  for (int b = 0; b < nb; ++b) {
    for (const Instr& in : f.blocks[b].instrs) {
      for (int v : uses(in))
        if (!def_set[b][v]) use[b][v] = true;
      int d = def(in);
      if (d >= 0) def_set[b][d] = true;
    }
  }

  // Liveness flows backwards, so visit blocks in reverse depth-first order: a block's
  // successors are then (except across back edges) already up to date when it is visited.
  // Blocks the entry cannot reach are not in that order; they are still analysed, last.
  std::vector<int> order = depth_first_order(f);
  std::reverse(order.begin(), order.end());
  std::vector<bool> placed(nb, false);
  for (int b : order) placed[b] = true;
  for (int b = 0; b < nb; ++b)
    if (!placed[b]) order.push_back(b);

  bool changed = true;
  while (changed) {
    changed = false;
    for (int b : order) {
      std::vector<bool> out(nv, false);
      for (int s : successors(f, b)) {
        const std::vector<bool>& in_s = L.live_in.at(s);  // .at(): a bad target throws
        for (int v = 0; v < nv; ++v)
          if (in_s[v]) out[v] = true;
      }

      std::vector<bool> in(nv, false);
      for (int v = 0; v < nv; ++v) in[v] = use[b][v] || (out[v] && !def_set[b][v]);

      if (out != L.live_out[b] || in != L.live_in[b]) {
        L.live_out[b] = std::move(out);
        L.live_in[b] = std::move(in);
        changed = true;
      }
    }
  }
  return L;
}

}  // namespace ls
