#include "interference.h"

#include <stdexcept>

namespace ls {

// The classic construction (Chaitin et al. 1981): walk each block backwards from its live-out
// set, keeping the set of variables that are live *after* the instruction being looked at.
//
//   at an instruction that writes d:  d interferes with every variable live after it
//   then update the live set for the point before the instruction:
//       remove d (it is not live before its own definition),
//       add every variable the instruction reads
//
// Two details that are easy to get wrong:
//
//  - The edges are added against the set of variables live AFTER the instruction, not before.
//    `v3 = add v1 v2` where v1 and v2 die right here does not make v3 interfere with them,
//    so v3 may take over v1's register. That is safe because the VM (like real hardware)
//    reads its operands before it writes the result. If v1 is still live after the
//    instruction, v3 interferes with it and cannot share.
//
//  - A definition whose value is never read (a dead definition) still interferes with
//    everything live after it. The register is written whether or not anyone reads it, so it
//    must not hold another live variable's value.
//
// Why defs are enough: if two variables are live at the same point, whichever was written
// last was written while the other was live, so the edge was added at that write. The one
// exception is a variable live on entry to the function, which no instruction writes. The
// design says there are none (no parameters yet), but a program that reads a variable before
// writing it would have some, so they all get edges to each other for safety.
std::vector<std::vector<int>> build_interference(const Function& f, const Liveness& live) {
  const int nb = static_cast<int>(f.blocks.size());
  const int nv = f.num_vars;

  if (static_cast<int>(live.live_in.size()) != nb || static_cast<int>(live.live_out.size()) != nb)
    throw std::invalid_argument("liveness does not match the function (wrong number of blocks)");

  std::vector<std::vector<int>> adj(nv);

  // The adjacency matrix exists only to reject duplicate edges in O(1): the same pair is met
  // many times (once per instruction at which both are live).
  std::vector<std::vector<bool>> edge(nv, std::vector<bool>(nv, false));
  auto add_edge = [&](int a, int b) {
    if (a == b || edge[a][b]) return;
    edge[a][b] = edge[b][a] = true;
    adj[a].push_back(b);
    adj[b].push_back(a);
  };

  // The live set as a "sparse set": a list of members plus each member's position in it, so
  // insert, erase and membership are O(1) and walking the set costs its size, not nv. That
  // keeps the walk proportional to the number of edges found rather than V per definition.
  std::vector<int> members;
  std::vector<int> where(nv, -1);  // position in `members`, or -1
  auto insert = [&](int v) {
    if (where[v] >= 0) return;
    where[v] = static_cast<int>(members.size());
    members.push_back(v);
  };
  auto erase = [&](int v) {
    const int w = where[v];
    if (w < 0) return;
    const int last = members.back();
    members[w] = last;  // move the last member into the hole (a no-op if v is the last one)
    where[last] = w;
    members.pop_back();
    where[v] = -1;
  };

  for (int b = 0; b < nb; ++b) {
    for (int v : members) where[v] = -1;
    members.clear();
    for (int v = 0; v < nv; ++v)
      if (live.live_out[b][v]) insert(v);

    const std::vector<Instr>& code = f.blocks[b].instrs;
    for (size_t k = code.size(); k-- > 0;) {
      const Instr& in = code[k];
      const int d = def(in);
      if (d >= 0) {
        for (int v : members) add_edge(d, v);  // add_edge ignores v == d
        erase(d);
      }
      for (int u : uses(in)) insert(u);
    }
  }

  if (nb > 0) {
    std::vector<int> on_entry;
    for (int v = 0; v < nv; ++v)
      if (live.live_in[0][v]) on_entry.push_back(v);
    for (size_t i = 0; i < on_entry.size(); ++i)
      for (size_t j = i + 1; j < on_entry.size(); ++j) add_edge(on_entry[i], on_entry[j]);
  }

  return adj;
}

}  // namespace ls
