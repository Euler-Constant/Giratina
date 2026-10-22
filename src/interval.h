#pragma once

namespace ls {

// A live interval [start, end], endpoints inclusive, in instruction numbers.
// Two intervals overlap iff a.start <= b.end && b.start <= a.end.
struct Interval {
  int var;
  int start;
  int end;
  double weight = 0.0;  // estimated usage count (usage-count allocator, Table V heuristic)
};

}  // namespace ls
