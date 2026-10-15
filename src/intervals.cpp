#include "intervals.h"

#include <algorithm>
#include <climits>
#include <stdexcept>

#include "cfg.h"

namespace ls {

namespace {

// Weight of one operand mention = kLoopWeight ^ (loop nesting depth of its block).
// The paper says only "estimated usage counts" without a formula, so this is our own
// choice (recorded as a gap in DESIGN.md). Depth is capped so deeply nested loops
// cannot overflow a double.
constexpr double kLoopWeight = 10.0;
constexpr int kMaxLoopDepth = 8;

// Loop nesting depth of every block (0 = not in any loop). Blocks the entry cannot
// reach get 0.
//
// An edge u -> h is a *back edge* when h is at or before u in depth-first order. For a
// depth-first search this is exactly an edge to an ancestor in the DFS tree: tree,
// forward and cross edges all go to a block that finishes earlier, hence appears later
// in the reverse-finish-order. The natural loop of a back edge u -> h is h plus every
// block that can reach u without passing through h; we find it by walking predecessors
// backwards from u and stopping at h. Several back edges to the same header (a loop
// with two `continue`s) form ONE loop, so their bodies are merged before counting.
//
// For irreducible control flow (a loop with two entry points) the "header" is whichever
// block the DFS happened to enter first, and the walk can cover more blocks than a
// textbook natural loop would. This is only used to weight spill decisions, so that is
// acceptable.
std::vector<int> loop_depths(const Function& f) {
  const int nb = static_cast<int>(f.blocks.size());
  std::vector<int> depth(nb, 0);

  const std::vector<int> order = depth_first_order(f);
  std::vector<int> pos(nb, -1);
  for (size_t i = 0; i < order.size(); ++i) pos[order[i]] = static_cast<int>(i);

  // Predecessor lists, counting only reachable predecessors.
  std::vector<std::vector<int>> preds(nb);
  for (int b : order)
    for (int s : successors(f, b)) preds[s].push_back(b);

  // body[h] is non-empty only if h is a loop header.
  std::vector<std::vector<bool>> body(nb);
  for (int u : order) {
    for (int h : successors(f, u)) {
      if (pos[h] > pos[u]) continue;  // forward, tree, or cross edge: not a back edge
      if (body[h].empty()) body[h].assign(nb, false);
      body[h][h] = true;

      std::vector<int> work;
      if (!body[h][u]) {  // u == h (a self-loop) is already in the body
        body[h][u] = true;
        work.push_back(u);
      }
      while (!work.empty()) {
        int x = work.back();
        work.pop_back();
        for (int p : preds[x]) {
          if (!body[h][p]) {  // h is already marked, so the walk stops there
            body[h][p] = true;
            work.push_back(p);
          }
        }
      }
    }
  }

  for (int h = 0; h < nb; ++h) {
    if (body[h].empty()) continue;
    for (int b = 0; b < nb; ++b)
      if (body[h][b]) ++depth[b];
  }
  return depth;
}

}  // namespace

// The "allocation setup" step for linear scan: number the instructions, then coarsen
// liveness into one [start, end] interval per variable.
//
// A variable's interval is the smallest range that contains
//   - every instruction that reads or writes it,
//   - the first instruction of every block it is live into,
//   - the last instruction of every block it is live out of.
// That is exactly [lowest position where it is live, highest position where it is live]:
// inside a block a variable is live from its definition (or the block's first
// instruction, if it is live-in) to its last read (or the block's last instruction, if
// it is live-out), so the extreme positions are always one of the points above.
// Holes in between are deliberately ignored; that is what makes the intervals "coarse".
std::vector<Interval> build_intervals(const Function& f, const Liveness& live,
                                      const std::vector<int>& block_order) {
  const int nb = static_cast<int>(f.blocks.size());
  const int nv = f.num_vars;

  if (static_cast<int>(live.live_in.size()) != nb || static_cast<int>(live.live_out.size()) != nb)
    throw std::invalid_argument("liveness does not match the function (wrong number of blocks)");

  // 1. Number instructions consecutively along block_order.
  std::vector<int> first(nb, -1), last(nb, -1);  // inclusive range of each block; -1 = none
  std::vector<bool> in_order(nb, false);
  int next = 0;
  for (int b : block_order) {
    if (b < 0 || b >= nb) throw std::out_of_range("block_order names a block that does not exist");
    if (in_order[b]) throw std::invalid_argument("block_order lists a block twice");
    in_order[b] = true;
    const int n = static_cast<int>(f.blocks[b].instrs.size());
    if (n == 0) continue;  // an empty block gets no numbers (the parser never produces one)
    first[b] = next;
    next += n;
    last[b] = next - 1;
  }

  // Every successor of a numbered block must be numbered too. Otherwise a variable that
  // is live into the missing block would silently get too short an interval.
  for (int b : block_order)
    for (int s : successors(f, b))
      if (!in_order.at(s))
        throw std::invalid_argument("block_order is missing a block that is reachable from it");

  // 2. Hull of every position where each variable occurs or is live; usage weights.
  std::vector<int> lo(nv, INT_MAX), hi(nv, -1);
  std::vector<double> weight(nv, 0.0);
  auto touch = [&](int v, int p) {
    lo[v] = std::min(lo[v], p);
    hi[v] = std::max(hi[v], p);
  };

  const std::vector<int> depth = loop_depths(f);

  for (int b : block_order) {
    if (first[b] < 0) continue;

    double w = 1.0;
    for (int i = 0; i < std::min(depth[b], kMaxLoopDepth); ++i) w *= kLoopWeight;

    // Every operand mention: reads first, then the write, matching instruction semantics.
    int p = first[b];
    for (const Instr& in : f.blocks[b].instrs) {
      for (int u : uses(in)) {
        touch(u, p);
        weight[u] += w;
      }
      int d = def(in);
      if (d >= 0) {
        touch(d, p);
        weight[d] += w;
      }
      ++p;
    }

    // Live across the block boundaries.
    for (int v = 0; v < nv; ++v) {
      if (live.live_in[b][v]) touch(v, first[b]);
      if (live.live_out[b][v]) touch(v, last[b]);
    }
  }

  // 3. One interval per variable. A variable that never occurs in numbered code (only in
  // unreachable blocks, or an id never used at all) gets start = end = -1: it overlaps
  // nothing, takes no real register time, and still has an entry so it gets a home.
  std::vector<Interval> out;
  out.reserve(nv);
  for (int v = 0; v < nv; ++v) {
    Interval iv;
    iv.var = v;
    iv.start = hi[v] < 0 ? -1 : lo[v];
    iv.end = hi[v];
    iv.weight = weight[v];
    out.push_back(iv);
  }

  // Increasing start, as linear scan requires. Ties broken by variable id for determinism.
  std::sort(out.begin(), out.end(), [](const Interval& a, const Interval& b) {
    return a.start != b.start ? a.start < b.start : a.var < b.var;
  });
  return out;
}

}  // namespace ls
