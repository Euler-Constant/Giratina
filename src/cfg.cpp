#include "cfg.h"

#include <algorithm>
#include <stdexcept>

namespace ls {

std::vector<int> successors(const Function& f, int b) {
  const auto& ins = f.blocks.at(b).instrs;
  if (ins.empty()) return {};
  const Instr& last = ins.back();
  if (last.op == Op::Jmp) return {last.t1};
  if (last.op == Op::Br) return {last.t1, last.t2};
  return {};
}

// Depth-first order (paper, Sec. 3): the reverse of the order in which a DFS from the
// entry finishes visiting blocks. A block is "finished" once everything reachable
// from it has been visited, so it is added to `post` only after all its successors.
//
// Successors are visited in the order successors() returns them (for `br`, the true
// target first), which keeps the result deterministic.
//
// Iterative, with an explicit stack, so a function with thousands of chained blocks
// cannot overflow the call stack.
std::vector<int> depth_first_order(const Function& f) {
  const int n = static_cast<int>(f.blocks.size());
  std::vector<int> post;
  if (n == 0) return post;

  struct Frame {
    int block;
    std::vector<int> succ;  // successors of `block`
    size_t next;            // index of the next successor to look at
  };

  std::vector<bool> visited(n, false);
  std::vector<Frame> stack;
  visited[0] = true;
  stack.push_back({0, successors(f, 0), 0});

  while (!stack.empty()) {
    Frame& top = stack.back();
    if (top.next < top.succ.size()) {
      int s = top.succ[top.next++];
      if (s < 0 || s >= n) throw std::out_of_range("branch target is not a block");
      if (!visited[s]) {
        visited[s] = true;
        stack.push_back({s, successors(f, s), 0});  // `top` is invalid after this line
      }
    } else {
      post.push_back(top.block);  // all successors done: this block is finished
      stack.pop_back();
    }
  }

  std::reverse(post.begin(), post.end());
  return post;
}

// Blocks in the order they appear in the function (Table IV's "linear" order).
// Unlike depth_first_order this includes blocks that are unreachable from the entry.
std::vector<int> layout_order(const Function& f) {
  std::vector<int> order(f.blocks.size());
  for (size_t i = 0; i < order.size(); ++i) order[i] = static_cast<int>(i);
  return order;
}

}  // namespace ls
