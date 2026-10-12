#pragma once
#include <vector>

#include "ir.h"

namespace ls {

// Successor block indices of block b (from its last instruction).
std::vector<int> successors(const Function& f, int b);

// Block indices in depth-first order: reverse postorder of a DFS from the entry
// (paper, Sec. 3). Blocks unreachable from the entry are omitted.
std::vector<int> depth_first_order(const Function& f);

// Block indices in the order they appear in the function ("layout order", Table IV).
std::vector<int> layout_order(const Function& f);

}  // namespace ls
