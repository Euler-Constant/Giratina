#pragma once
#include <vector>

#include "ir.h"
#include "liveness.h"

namespace ls {

// The interference graph of a function, as adjacency lists indexed by variable.
//
// Two variables interfere when one of them is live at a point where the other is written, so
// they must not share a register. The graph is a simple undirected graph: no self-edges, no
// duplicate edges, and v is in adj[u] exactly when u is in adj[v].
//
// This is the "allocation setup" step for graph coloring, the counterpart of build_intervals
// for linear scan. It is much more precise than intervals: a variable that is dead in the
// middle of its interval (a "hole") does not interfere with variables defined in the hole.
// It also costs more: the matrix that removes duplicate edges is O(V^2) bits, which is the
// quadratic behaviour the paper blames for coloring's slowdown on large problems (Sec. 5.2.3).
std::vector<std::vector<int>> build_interference(const Function& f, const Liveness& live);

}  // namespace ls
