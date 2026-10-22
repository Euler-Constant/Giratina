#include <algorithm>
#include <stdexcept>
#include <string>
#include <vector>

#include "alloc.h"

namespace ls {

namespace {

// Graph coloring register allocation in the style of Chaitin, with Briggs' optimistic twist.
//
// Colors are registers 0 .. R-1, nodes are variables, and an edge means "may not share".
// A node with fewer than R neighbours can always be colored, whatever colors its neighbours
// get, because at least one of the R registers is left over. So:
//
//   simplify: repeatedly remove a node with fewer than R remaining neighbours and push it on
//             a stack. Removing it lowers its neighbours' degrees, which may make them
//             removable in turn.
//   If every remaining node has R or more neighbours, simplify is stuck. Pick one node as a
//   "potential spill" by the spill heuristic and remove it anyway, optimistically hoping its
//   neighbours will not use up all R colors (Briggs et al. 1994). Chaitin's original would
//   spill it for certain at this point.
//
//   select:   pop the stack (the last node removed comes back first) and give each node the
//             lowest-numbered register that none of its already-colored neighbours has. A node
//             removed with degree < R always finds one. A potential spill may or may not; if
//             it does not, it is an actual spill and lives on the stack.
//
// There is no "rebuild the graph and try again" loop, which classic Chaitin allocators need
// because spill code introduces new short-lived variables that need registers themselves.
// Here a spilled variable is accessed through the VM's two scratch registers, which are not
// among the R allocatable ones (DESIGN.md), so spilling never changes the graph.
//
// Not done, as the paper's allocator does not: coalescing, live-range splitting.
// The paper does not say which variant of simplify/select it implements; this is one
// reasonable reading and is recorded as a gap in DESIGN.md.
class GraphColoring {
 public:
  GraphColoring(const AllocProblem& p, SpillChoice choice)
      : p_(p),
        n_(p.num_vars),
        R_(std::max(p.R, 0)),
        choice_(choice),
        a_(p.num_vars),
        weight_(p.num_vars, 0.0),
        degree_(p.num_vars, 0),
        removed_(p.num_vars, 0),
        in_low_(p.num_vars, 0) {}

  Allocation run() {
    check_input();
    simplify();
    select();
    return a_;
  }

 private:
  // The preconditions, checked so that a violation fails loudly. Not checked, because it
  // would cost as much as the allocation itself: that the graph is symmetric and has no
  // duplicate edges (build_interference guarantees it, and the tests verify that it does).
  void check_input() {
    if (static_cast<int>(p_.interference.size()) != n_)
      throw std::invalid_argument(
          "graph coloring: the problem has no interference graph (it needs one list per variable; "
          "build it with build_interference)");
    for (int u = 0; u < n_; ++u)
      for (int v : p_.interference[u]) {
        if (v < 0 || v >= n_)
          throw std::invalid_argument("graph coloring: v" + std::to_string(u) + " interferes with a variable that does not exist");
        if (v == u)
          throw std::invalid_argument("graph coloring: v" + std::to_string(u) + " interferes with itself");
      }
    for (const Interval& iv : p_.intervals) {
      if (iv.var < 0 || iv.var >= n_)
        throw std::invalid_argument("graph coloring: an interval names a variable that does not exist");
      weight_[iv.var] = iv.weight;  // the estimated usage count; the only thing used from intervals
    }
  }

  // ---------------------------------------------------------------- simplify
  void simplify() {
    for (int v = 0; v < n_; ++v) {
      degree_[v] = static_cast<int>(p_.interference[v].size());
      if (degree_[v] < R_) push_low(v);
    }

    int remaining = n_;
    while (remaining > 0) {
      int v;
      if (!low_.empty()) {
        v = low_.back();  // safe to remove: fewer than R neighbours left
        low_.pop_back();
      } else {
        v = pick_potential_spill();  // stuck: everything left has R or more neighbours
      }
      remove(v);
      --remaining;
    }
  }

  // Invariant: a node is in low_ exactly when it is not removed and has fewer than R remaining
  // neighbours, and in_low_ stops a node from being added twice.
  void push_low(int v) {
    in_low_[v] = 1;
    low_.push_back(v);
  }

  void remove(int v) {
    removed_[v] = 1;
    stack_.push_back(v);
    for (int u : p_.interference[v]) {
      if (removed_[u]) continue;
      --degree_[u];
      if (degree_[u] < R_ && !in_low_[u]) push_low(u);
    }
  }

  // Which node to set aside when simplify is stuck. The best candidate has the lowest key;
  // ties go to the node with more neighbours (setting it aside helps its neighbours most), then
  // to the lower variable number so the result is deterministic. A linear scan: this happens at
  // most once per node, and the graph itself already costs more than that to build.
  int pick_potential_spill() const {
    int best = -1;
    double best_key = 0;
    for (int v = 0; v < n_; ++v) {
      if (removed_[v]) continue;
      // degree_ >= R_ here, but with R == 0 it can be 0, so never divide by less than 1.
      const double key = choice_ == SpillChoice::LowestWeight ? weight_[v] : weight_[v] / std::max(degree_[v], 1);
      if (best < 0 || key < best_key || (key == best_key && degree_[v] > degree_[best])) {
        best = v;
        best_key = key;
      }
    }
    return best;
  }

  // ------------------------------------------------------------------ select
  void select() {
    // stamp[c] == v means "color c is taken by a neighbour of v". Stamping with the node's own
    // number avoids clearing the array for every node.
    std::vector<int> stamp(R_, -1);
    int next_slot = 0;
    for (size_t k = stack_.size(); k-- > 0;) {  // last removed first
      const int v = stack_[k];
      // reg[u] is -1 for a neighbour that is not colored yet (still lower on the stack) and for
      // one that was spilled; neither takes a color away from v.
      for (int u : p_.interference[v])
        if (a_.reg[u] >= 0) stamp[a_.reg[u]] = v;

      int color = -1;
      for (int c = 0; c < R_; ++c)
        if (stamp[c] != v) {
          color = c;
          break;
        }

      if (color >= 0) {
        a_.reg[v] = color;
      } else {  // an actual spill: all R colors are taken by neighbours
        a_.slot[v] = next_slot++;
        ++a_.num_spills;
      }
    }
  }

  const AllocProblem& p_;
  const int n_;
  const int R_;
  const SpillChoice choice_;
  Allocation a_;
  std::vector<double> weight_;
  std::vector<int> degree_;           // neighbours not yet removed
  std::vector<char> removed_;
  std::vector<char> in_low_;
  std::vector<int> low_;              // removable nodes (degree < R), used as a stack
  std::vector<int> stack_;            // removal order; select pops it
};

}  // namespace

Allocation allocate_graph_coloring_with(const AllocProblem& p, SpillChoice choice) {
  return GraphColoring(p, choice).run();
}

Allocation allocate_graph_coloring(const AllocProblem& p) {
  return allocate_graph_coloring_with(p, SpillChoice::LowestWeight);
}

}  // namespace ls
