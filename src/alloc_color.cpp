#include "alloc.h"
#include "common.h"

namespace ls {

// TODO (build order step 6). Uses AllocProblem::interference, not the intervals.
// Simplify/select with R colors. When no node has degree < R, remove a node chosen by
// reference count (Interval::weight), and spill it if select cannot color it.
// No coalescing at first.
Allocation allocate_graph_coloring(const AllocProblem&) {
  throw NotImplemented("allocate_graph_coloring: TODO (build order step 6)");
}

}  // namespace ls
