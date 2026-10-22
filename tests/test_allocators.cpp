// Tests for the real allocators. Each one is run through the differential harness on every
// fixed program and on random programs, at several register counts and both numberings;
// that is what decides whether it is correct. Allocator-specific behaviour is tested
// separately (one section per allocator below).
//
// To add another allocator, add a line to `allocators` in main().
#include <algorithm>
#include <iostream>
#include <random>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>

#include "alloc.h"
#include "harness.h"
#include "interference.h"
#include "liveness.h"
#include "programs.h"
#include "validate.h"

using namespace ls;

static int g_bad = 0;
static void check(bool ok, const std::string& what) {
  if (!ok) ++g_bad;
  std::cout << (ok ? "ok    " : "FAIL  ") << what << "\n";
}

static Interval iv(int var, int start, int end, double weight) {
  Interval i;
  i.var = var;
  i.start = start;
  i.end = end;
  i.weight = weight;
  return i;
}

// A problem from (start, end) pairs, which must be listed in order of increasing start.
// Variable ids are the positions in the list, all weights 1.
static AllocProblem problem(int R, const std::vector<std::pair<int, int>>& ends) {
  AllocProblem p;
  p.num_vars = static_cast<int>(ends.size());
  p.R = R;
  for (size_t k = 0; k < ends.size(); ++k) p.intervals.push_back(iv(static_cast<int>(k), ends[k].first, ends[k].second, 1));
  return p;
}

// The fewest variables that must live on the stack for R registers, by trying every subset of
// the intervals: keep the largest subset in which no point is covered by more than R of them.
static int fewest_spills(const std::vector<Interval>& ivs, int R) {
  const int n = static_cast<int>(ivs.size());
  int best = 0;
  for (unsigned mask = 0; mask < (1u << n); ++mask) {
    int size = 0;
    for (int i = 0; i < n; ++i) size += (mask >> i) & 1;
    if (size <= best) continue;
    bool ok = true;
    for (int i = 0; i < n && ok; ++i) {
      if (!((mask >> i) & 1)) continue;
      int covering = 0;  // the busiest point is always some interval's start point
      for (int j = 0; j < n; ++j)
        if (((mask >> j) & 1) && ivs[j].start <= ivs[i].start && ivs[i].start <= ivs[j].end) ++covering;
      if (covering > R) ok = false;
    }
    if (ok) best = size;
  }
  return n - best;
}

// A problem that carries only an interference graph (and usage-count weights), for hand-built graphs.
// Weights default to 1. The intervals exist only to carry the weights.
static AllocProblem graph(int R, int n, const std::vector<std::pair<int, int>>& edges, std::vector<double> w = {}) {
  AllocProblem p;
  p.num_vars = n;
  p.R = R;
  p.interference.assign(n, {});
  for (const auto& e : edges) {
    p.interference[e.first].push_back(e.second);
    p.interference[e.second].push_back(e.first);
  }
  w.resize(n, 1.0);
  for (int v = 0; v < n; ++v) p.intervals.push_back(iv(v, v, v, w[v]));
  return p;
}

using EdgeSet = std::set<std::pair<int, int>>;

// The interference graph by its definition, using none of the project's liveness or CFG code:
// d interferes with v when d is written by an instruction after which v can still be read before
// it is written again. That is found by searching the control flow graph one instruction at a
// time, so it shares no algorithm with compute_liveness or build_interference.
static EdgeSet oracle_edges(const Function& f) {
  std::vector<int> base(f.blocks.size() + 1, 0);  // node number of each block's first instruction
  for (size_t b = 0; b < f.blocks.size(); ++b) base[b + 1] = base[b] + static_cast<int>(f.blocks[b].instrs.size());
  const int N = base.back();
  std::vector<const Instr*> at(N);
  std::vector<std::vector<int>> next(N);
  for (size_t b = 0; b < f.blocks.size(); ++b) {
    const auto& code = f.blocks[b].instrs;
    for (size_t k = 0; k < code.size(); ++k) {
      const int node = base[b] + static_cast<int>(k);
      const Instr& in = code[k];
      at[node] = &in;
      if (k + 1 < code.size()) next[node] = {node + 1};
      else if (in.op == Op::Jmp) next[node] = {base[in.t1]};
      else if (in.op == Op::Br) next[node] = {base[in.t1], base[in.t2]};
    }
  }
  // Starting at `start`, is v read before it is next written, along some path?
  auto read_before_written = [&](std::vector<int> start, int v) {
    std::vector<char> seen(N, 0);
    while (!start.empty()) {
      const int s = start.back();
      start.pop_back();
      if (seen[s]) continue;
      seen[s] = 1;
      for (int u : uses(*at[s])) if (u == v) return true;
      if (def(*at[s]) == v) continue;  // written first: the old value is dead on this path
      for (int t : next[s]) start.push_back(t);
    }
    return false;
  };

  EdgeSet edges;
  auto add = [&](int a, int b) { if (a != b) edges.insert({std::min(a, b), std::max(a, b)}); };
  for (int p = 0; p < N; ++p) {
    const int d = def(*at[p]);
    if (d < 0) continue;
    for (int v = 0; v < f.num_vars; ++v)
      if (v != d && read_before_written(next[p], v)) add(d, v);
  }
  std::vector<int> on_entry;  // variables read before any write (the design has none, but be safe)
  for (int v = 0; v < f.num_vars; ++v)
    if (N > 0 && read_before_written({0}, v)) on_entry.push_back(v);
  for (size_t i = 0; i < on_entry.size(); ++i)
    for (size_t j = i + 1; j < on_entry.size(); ++j) add(on_entry[i], on_entry[j]);
  return edges;
}

// The edges the project's builder produced, plus a list of structural complaints about the lists.
static EdgeSet built_edges(const Function& f, std::string* complaint) {
  const auto adj = build_interference(f, compute_liveness(f));
  EdgeSet edges;
  if (static_cast<int>(adj.size()) != f.num_vars) *complaint = "one list per variable expected";
  for (size_t u = 0; u < adj.size(); ++u) {
    std::set<int> seen;
    for (int v : adj[u]) {
      if (v < 0 || v >= f.num_vars) { *complaint = "neighbour out of range"; continue; }
      if (v == static_cast<int>(u)) *complaint = "self-edge on v" + std::to_string(u);
      if (!seen.insert(v).second) *complaint = "duplicate edge v" + std::to_string(u) + "-v" + std::to_string(v);
      if (std::find(adj[v].begin(), adj[v].end(), static_cast<int>(u)) == adj[v].end())
        *complaint = "edge v" + std::to_string(u) + "-v" + std::to_string(v) + " is not symmetric";
      edges.insert({std::min<int>(u, v), std::max<int>(u, v)});
    }
  }
  return edges;
}

static std::string show_edges(const EdgeSet& e) {
  std::string s = "{";
  for (const auto& x : e) s += " " + std::to_string(x.first) + "-" + std::to_string(x.second);
  return s + " }";
}

// The smallest k such that the graph can be reduced to nothing by repeatedly removing a node with
// fewer than k remaining neighbours. This is the property simplify relies on, computed here by a
// separate min-degree peeling ("degeneracy"): removing the minimum-degree node each time yields the
// largest minimum degree seen, and the graph is reducible with R colors exactly when that is < R.
static int degeneracy(const std::vector<std::vector<int>>& adj) {
  const int n = static_cast<int>(adj.size());
  std::vector<int> deg(n);
  std::vector<char> gone(n, 0);
  for (int v = 0; v < n; ++v) deg[v] = static_cast<int>(adj[v].size());
  int worst = 0;
  for (int round = 0; round < n; ++round) {
    int pick = -1;
    for (int v = 0; v < n; ++v)
      if (!gone[v] && (pick < 0 || deg[v] < deg[pick])) pick = v;
    worst = std::max(worst, deg[pick]);
    gone[pick] = 1;
    for (int u : adj[pick]) if (!gone[u]) --deg[u];
  }
  return worst;
}

static std::string describe(const Allocation& a) {
  std::string s;
  for (size_t v = 0; v < a.reg.size(); ++v) {
    s += "v" + std::to_string(v) + (a.reg[v] >= 0 ? "->r" + std::to_string(a.reg[v]) : "->slot" + std::to_string(a.slot[v])) + " ";
  }
  return s;
}

int main() {
  const std::vector<int> Rs = {1, 2, 3, 4, 8, 32};
  const Numbering numberings[] = {Numbering::DepthFirst, Numbering::Layout};

  // ======================= usage-count: behaviour on hand-built problems =======================
  {
    // Four variables whose intervals are pairwise DISJOINT, so a smarter allocator could use one register.
    AllocProblem p;
    p.num_vars = 4;
    p.R = 2;
    p.intervals = {iv(0, 0, 1, 5), iv(1, 2, 3, 1), iv(2, 4, 5, 9), iv(3, 6, 7, 3)};
    Allocation a = allocate_usage_count(p);
    std::string why;
    check(validate_allocation(p, a, &why), "usage-count: result validates " + why);
    check(a.reg[2] == 0 && a.reg[0] == 1 && a.reg[1] < 0 && a.reg[3] < 0,
          "usage-count: weights 5,1,9,3 at R=2 -> v2 (9) and v0 (5) get registers: " + describe(a));
    check(a.num_spills == 2 && a.slot[3] == 0 && a.slot[1] == 1, "usage-count: the other two are spilled, one slot each, in rank order");
    check(a.reg[2] != a.reg[0], "usage-count: each register belongs to one variable (no sharing even for disjoint intervals)");

    // The same problem with the intervals listed in a different order (they are normally sorted by start).
    AllocProblem q = p;
    std::reverse(q.intervals.begin(), q.intervals.end());
    Allocation b = allocate_usage_count(q);
    check(b.reg == a.reg && b.slot == a.slot, "usage-count: independent of the order the intervals are listed in");

    p.intervals = {iv(0, 0, 1, 4), iv(1, 0, 1, 4), iv(2, 0, 1, 4), iv(3, 0, 1, 4)};
    Allocation t = allocate_usage_count(p);
    check(t.reg[0] == 0 && t.reg[1] == 1 && t.reg[2] < 0 && t.reg[3] < 0, "usage-count: equal weights are broken by variable id: " + describe(t));

    p.R = 0;
    Allocation none = allocate_usage_count(p);
    check(none.num_spills == 4 && std::all_of(none.reg.begin(), none.reg.end(), [](int r) { return r < 0; }), "usage-count: R = 0 spills everything");
    p.R = 10;
    Allocation all = allocate_usage_count(p);
    check(all.num_spills == 0 && std::all_of(all.slot.begin(), all.slot.end(), [](int s) { return s < 0; }), "usage-count: R >= number of variables spills nothing");
  }

  // ======================= linear scan: each decision of Figure 1 =======================
  {
    auto spilled = [](const Allocation& a, int v) { return a.reg[v] < 0; };
    std::string why;

    {  // SpillAtInterval, the interval in active ends last: it is spilled and the new one takes its register.
      AllocProblem p = problem(1, {{0, 10}, {2, 5}});
      Allocation a = allocate_linear_scan(p);
      check(validate_allocation(p, a, &why) && spilled(a, 0) && a.reg[1] == 0 && a.num_spills == 1,
            "linear scan: active interval ends later -> it is spilled, the new one takes its register: " + describe(a));
    }
    {  // The new interval ends last: it is the one spilled.
      AllocProblem p = problem(1, {{0, 5}, {2, 9}});
      Allocation a = allocate_linear_scan(p);
      check(a.reg[0] == 0 && spilled(a, 1) && a.num_spills == 1, "linear scan: new interval ends later -> the new interval is spilled: " + describe(a));
    }
    {  // Tie on end points: the new interval is spilled (the figure's test is a strict >).
      AllocProblem p = problem(1, {{0, 5}, {2, 5}});
      Allocation a = allocate_linear_scan(p);
      check(a.reg[0] == 0 && spilled(a, 1), "linear scan: equal end points -> the new interval is spilled: " + describe(a));
    }
    {  // ExpireOldIntervals boundary: an interval ending one before the next starts is expired ...
      AllocProblem p = problem(1, {{0, 4}, {5, 9}});
      Allocation a = allocate_linear_scan(p);
      check(a.num_spills == 0 && a.reg[0] == 0 && a.reg[1] == 0, "linear scan: end 4, next start 5 -> register reused, no spill: " + describe(a));
    }
    {  // ... but one ending exactly where the next starts still overlaps it.
      AllocProblem p = problem(1, {{0, 5}, {5, 9}});
      Allocation a = allocate_linear_scan(p);
      check(a.reg[0] == 0 && spilled(a, 1), "linear scan: end 5, next start 5 -> still overlapping, so a spill: " + describe(a));
    }
    {  // Several intervals expire together and all their registers become available.
      AllocProblem p = problem(3, {{0, 2}, {1, 3}, {1, 4}, {6, 8}, {6, 9}, {6, 7}});
      Allocation a = allocate_linear_scan(p);
      check(validate_allocation(p, a, &why) && a.num_spills == 0, "linear scan: three intervals expire at once, three new ones fit: " + describe(a));
    }
    {  // The spilled interval's old register is not left behind on the variable.
      AllocProblem p = problem(2, {{0, 20}, {1, 3}, {2, 4}});
      Allocation a = allocate_linear_scan(p);
      // v0 gets reg 0, v1 gets reg 1; at v2 both are busy and v0 ends last, so v0 is evicted and
      // v2 inherits reg 0. v0 must end with a slot and NO register.
      check(validate_allocation(p, a, &why) && spilled(a, 0) && a.slot[0] == 0 && a.reg[1] == 1 && a.reg[2] == 0 && a.slot[1] < 0 && a.slot[2] < 0 && a.num_spills == 1,
            "linear scan: an evicted variable ends with a slot only, the newcomer inherits its register: " + describe(a));
    }
    {  // R = 0 spills everything; plenty of registers spills nothing.
      AllocProblem p = problem(0, {{0, 3}, {1, 4}, {2, 5}});
      Allocation a = allocate_linear_scan(p);
      check(a.num_spills == 3 && validate_allocation(p, a, &why), "linear scan: R = 0 spills everything");
      p.R = 5;
      Allocation b = allocate_linear_scan(p);
      check(b.num_spills == 0 && validate_allocation(p, b, &why), "linear scan: R large spills nothing");
    }
    {  // Bad input fails loudly.
      auto throws = [](const AllocProblem& p) {
        try { allocate_linear_scan(p); } catch (const std::invalid_argument&) { return true; } catch (...) {}
        return false;
      };
      check(throws(problem(2, {{5, 6}, {0, 1}})), "linear scan: intervals not sorted by start are rejected");
      AllocProblem bad = problem(2, {{0, 1}});
      bad.intervals[0].var = 7;
      check(throws(bad), "linear scan: an interval naming a nonexistent variable is rejected");
    }

    // The paper (Sec. 4.1) says the furthest-end heuristic spills the fewest live ranges for simple
    // intervals. Check that against an exhaustive search on 3000 random interval sets.
    std::mt19937 rng(5);
    int sets = 0, not_optimal = 0;
    for (int it = 0; it < 3000; ++it) {
      const int n = 1 + static_cast<int>(rng() % 11);
      std::vector<std::pair<int, int>> ends;
      for (int k = 0; k < n; ++k) {
        const int s = static_cast<int>(rng() % 15);
        ends.push_back({s, s + static_cast<int>(rng() % 9)});
      }
      std::sort(ends.begin(), ends.end());
      for (int R = 0; R <= 4; ++R) {
        AllocProblem p = problem(R, ends);
        Allocation a = allocate_linear_scan(p);
        ++sets;
        if (!validate_allocation(p, a, nullptr) || a.num_spills != fewest_spills(p.intervals, R)) ++not_optimal;
      }
    }
    check(not_optimal == 0, "linear scan: " + std::to_string(sets) + " random interval sets x R, always valid and spilling the fewest variables possible (" + std::to_string(not_optimal) + " exceptions)");
  }

  // ======================= graph coloring: the interference graph =======================
  {
    // Hand-checked: operands that die at an instruction do not interfere with its result, a dead
    // definition still interferes with whatever is live after it.
    const Function f = parse(
        "func t\nblock entry:\n"
        "  v0 = const 1\n  v1 = const 2\n"
        "  v2 = add v0 v1\n"   // v0 and v1 die here, so v2 may reuse one of their registers
        "  v3 = const 9\n"     // never read, but it is written while v2 is live
        "  print v2\n  ret\n");
    std::string complaint;
    const EdgeSet got = built_edges(f, &complaint);
    const EdgeSet want = {{0, 1}, {2, 3}};
    check(got == want && complaint.empty(), "interference: operands dying at a def get no edge to it; a dead def still interferes (got " + show_edges(got) + " " + complaint + ")");

    // A hole: v1 is born and dies inside v0's interval while v0 is dead, so the intervals overlap
    // but the variables never interfere. This is exactly where coloring beats linear scan.
    const Function h = parse(
        "func t\nblock entry:\n"
        "  v0 = const 1\n  print v0\n"
        "  v1 = const 2\n  print v1\n"
        "  v0 = const 3\n  print v0\n  ret\n");
    check(built_edges(h, &complaint).empty(), "interference: a variable inside another's hole does not interfere with it");
    const AllocProblem hp = make_problem(h, 1);
    check(hp.intervals.size() == 2 && hp.intervals[0].end >= hp.intervals[1].start, "  ... although the two coarse intervals overlap");
    const Allocation hc = allocate_graph_coloring(hp);
    check(validate_allocation(hp, hc, &complaint, SharingRule::Interference) && hc.num_spills == 0 && hc.reg[0] == hc.reg[1],
          "  ... so graph coloring puts both in the one register, with no spill (linear scan spills " + std::to_string(allocate_linear_scan(hp).num_spills) + ")");

    // Against the definition-based oracle, which shares no code with the real builder.
    std::mt19937 rng(77);
    int programs_checked = 0, edges_checked = 0, wrong = 0;
    std::string first;
    auto compare = [&](const Function& fn, const std::string& label) {
      std::string c;
      const EdgeSet a = built_edges(fn, &c), b = oracle_edges(fn);
      ++programs_checked;
      edges_checked += static_cast<int>(b.size());
      if ((a != b || !c.empty()) && wrong++ == 0) first = label + ": builder " + show_edges(a) + " oracle " + show_edges(b) + " " + c;
    };
    for (const Named& pr : fixed_programs(LINSCAN_SOURCE_DIR)) compare(pr.f, pr.name);
    for (int it = 0; it < 400; ++it) {
      const std::string src = random_program(rng, it % 2 == 0);
      compare(parse(src), "random\n" + src);
    }
    check(wrong == 0, "interference: builder equals the oracle on " + std::to_string(programs_checked) + " programs (" + std::to_string(edges_checked) + " edges)" + (wrong ? "  FIRST: " + first : ""));
  }

  // ======================= graph coloring: each decision of simplify / select =======================
  {
    std::string why;
    auto ok = [&](const AllocProblem& p, const Allocation& a) { return validate_allocation(p, a, &why, SharingRule::Interference); };

    {  // Triangle, 3 registers: three different colors.  2 registers: one variable is spilled, the cheapest.
      AllocProblem t = graph(3, 3, {{0, 1}, {1, 2}, {0, 2}}, {5, 1, 9});
      Allocation a = allocate_graph_coloring(t);
      check(ok(t, a) && a.num_spills == 0, "coloring: a triangle fits in 3 registers: " + describe(a));
      t.R = 2;
      Allocation b = allocate_graph_coloring(t);
      check(ok(t, b) && b.num_spills == 1 && b.reg[1] < 0 && b.reg[0] >= 0 && b.reg[2] >= 0, "coloring: a triangle in 2 registers spills the lowest-weight node, v1: " + describe(b) + why);
    }
    {  // Optimism (Briggs). A 4-cycle has every degree equal to 2 = R, so simplify is stuck at once, yet it
       // is 2-colorable. Chaitin's original would spill for certain; optimistic selection finds the coloring.
      AllocProblem c4 = graph(2, 4, {{0, 1}, {1, 2}, {2, 3}, {3, 0}});
      Allocation a = allocate_graph_coloring(c4);
      check(ok(c4, a) && a.num_spills == 0, "coloring: a 4-cycle with 2 registers needs no spill (optimistic select): " + describe(a) + why);
    }
    {  // A chain needs only 2 colors however long; a pair of registers is enough.
      std::vector<std::pair<int, int>> e;
      for (int i = 0; i + 1 < 20; ++i) e.push_back({i, i + 1});
      AllocProblem line = graph(2, 20, e);
      Allocation a = allocate_graph_coloring(line);
      check(ok(line, a) && a.num_spills == 0, "coloring: a 20-node chain colors with 2 registers");
    }
    {  // Complete graphs: K_n needs n colors, so exactly n - R variables spill.
      int bad = 0, cases = 0;
      for (int n = 1; n <= 8; ++n)
        for (int R = 0; R <= 9; ++R) {
          std::vector<std::pair<int, int>> e;
          for (int i = 0; i < n; ++i)
            for (int j = i + 1; j < n; ++j) e.push_back({i, j});
          const AllocProblem k = graph(R, n, e);
          const Allocation a = allocate_graph_coloring(k);
          ++cases;
          if (!ok(k, a) || a.num_spills != std::max(0, n - R)) ++bad;
        }
      check(bad == 0, "coloring: K_n in R registers spills exactly max(0, n - R) (" + std::to_string(cases) + " cases)");
    }
    {  // No edges at all: everything shares register 0, however few registers.
      AllocProblem none = graph(1, 6, {});
      Allocation a = allocate_graph_coloring(none);
      check(ok(none, a) && a.num_spills == 0 && std::all_of(a.reg.begin(), a.reg.end(), [](int r) { return r == 0; }), "coloring: no interference -> everything in register 0: " + describe(a));
    }
    {  // R = 0 spills everything, one slot each.
      AllocProblem z = graph(0, 4, {{0, 1}, {2, 3}});
      Allocation a = allocate_graph_coloring(z);
      std::set<int> slots(a.slot.begin(), a.slot.end());
      check(ok(z, a) && a.num_spills == 4 && slots.size() == 4 && *slots.begin() == 0, "coloring: R = 0 spills everything into distinct slots: " + describe(a));
    }
    {  // The spill heuristic. A star with centre v0 and three leaves, one register. Any two linked variables
       // conflict, so the best is to spill just the centre. By weight alone the leaves look cheapest (20 < 30) and
       // all three go; by weight / degree the centre (30/3 = 10 < 20) is chosen and only it is spilled.
      AllocProblem star = graph(1, 4, {{0, 1}, {0, 2}, {0, 3}}, {30, 20, 20, 20});
      Allocation lw = allocate_graph_coloring_with(star, SpillChoice::LowestWeight);
      Allocation pd = allocate_graph_coloring_with(star, SpillChoice::LowestWeightPerDegree);
      check(ok(star, lw) && lw.num_spills == 3 && lw.reg[0] == 0, "coloring: lowest-weight on a star spills the three leaves: " + describe(lw));
      check(ok(star, pd) && pd.num_spills == 1 && pd.reg[0] < 0, "coloring: weight-per-degree on the same star spills only the centre: " + describe(pd));
      check(allocate_graph_coloring(star).reg == lw.reg, "coloring: the default choice is lowest weight");
    }
    {  // Ties in the spill key go to the node with more neighbours. A star with equal weights and one register:
       // setting the centre aside frees all three leaves (one spill); setting a leaf aside first leaves the
       // centre, still stuck, and ends with all three leaves spilled.
      // The centre is the HIGHEST variable number so that the fallback tie-break (lower number) would pick a leaf.
      AllocProblem tie = graph(1, 4, {{3, 0}, {3, 1}, {3, 2}}, {7, 7, 7, 7});
      Allocation a = allocate_graph_coloring(tie);
      check(ok(tie, a) && a.num_spills == 1 && a.reg[3] < 0, "coloring: equal weights -> the highest-degree node is set aside first, so only the centre spills: " + describe(a));
    }
    {  // Bad input fails loudly rather than producing something quietly wrong.
      auto throws = [](const AllocProblem& p) {
        try { allocate_graph_coloring(p); } catch (const std::invalid_argument&) { return true; } catch (...) {}
        return false;
      };
      AllocProblem missing = problem(2, {{0, 1}, {1, 2}});  // intervals only, no interference lists
      check(throws(missing), "coloring: a problem without an interference graph is rejected");
      AllocProblem self = graph(2, 2, {});
      self.interference[0].push_back(0);
      check(throws(self), "coloring: a self-edge is rejected");
      AllocProblem range = graph(2, 2, {});
      range.interference[0].push_back(5);
      check(throws(range), "coloring: an edge to a nonexistent variable is rejected");
    }

    // Random graphs: always a proper coloring, and the guarantee simplify rests on: if repeatedly removing
    // a node of degree < R empties the graph (checked independently by min-degree peeling), nothing spills.
    std::mt19937 rng(31337);
    int cases = 0, invalid = 0, guarantee_cases = 0, guarantee_broken = 0, spilled_cases = 0;
    for (int it = 0; it < 4000; ++it) {
      const int n = 1 + static_cast<int>(rng() % 30);
      const int pct = static_cast<int>(rng() % 70);
      std::vector<std::pair<int, int>> e;
      for (int i = 0; i < n; ++i)
        for (int j = i + 1; j < n; ++j)
          if (static_cast<int>(rng() % 100) < pct) e.push_back({i, j});
      std::vector<double> w(n);
      for (double& x : w) x = 1 + static_cast<double>(rng() % 50);
      for (int R = 0; R <= 7; ++R)
        for (SpillChoice sc : {SpillChoice::LowestWeight, SpillChoice::LowestWeightPerDegree}) {
          const AllocProblem g = graph(R, n, e, w);
          const Allocation a = allocate_graph_coloring_with(g, sc);
          ++cases;
          if (a.num_spills > 0) ++spilled_cases;
          if (!ok(g, a)) ++invalid;
          if (degeneracy(g.interference) < R) {
            ++guarantee_cases;
            if (a.num_spills != 0) ++guarantee_broken;
          }
        }
    }
    check(invalid == 0, "coloring: " + std::to_string(cases) + " random graph x R x heuristic results are all proper colorings (" + std::to_string(spilled_cases) + " with spills)");
    check(guarantee_cases > 1000 && guarantee_broken == 0, "coloring: whenever removing nodes of degree < R empties the graph, nothing spills (" + std::to_string(guarantee_cases) + " cases, " + std::to_string(guarantee_broken) + " exceptions)");

    // The same guarantee on the real interference graphs of programs.
    int real_cases = 0, real_broken = 0;
    std::mt19937 prng(8);
    for (int it = 0; it < 300; ++it) {
      const Function f = parse(random_program(prng, it % 2 == 0));
      for (int R = 1; R <= 8; ++R) {
        const AllocProblem p = make_problem(f, R);
        if (degeneracy(p.interference) < R) {
          ++real_cases;
          if (allocate_graph_coloring(p).num_spills != 0) ++real_broken;
        }
      }
    }
    check(real_cases > 100 && real_broken == 0, "coloring: the same guarantee holds on programs' own interference graphs (" + std::to_string(real_cases) + " cases)");
  }

  // ============ every real allocator, through the differential harness ============
  struct NamedAllocator {
    std::string name;
    Allocator fn;
    SharingRule sharing;  // what the validator may call a conflict: linear scan promises more than coloring
  };
  const std::vector<NamedAllocator> allocators = {
      {"usage-count", allocate_usage_count, SharingRule::Intervals},
      {"linear scan", allocate_linear_scan, SharingRule::Intervals},
      {"graph coloring", allocate_graph_coloring, SharingRule::Interference},
      {"graph coloring (weight/degree)",
       [](const AllocProblem& p) { return allocate_graph_coloring_with(p, SpillChoice::LowestWeightPerDegree); },
       SharingRule::Interference},
  };

  const std::vector<Named> programs = fixed_programs(LINSCAN_SOURCE_DIR);
  for (const NamedAllocator& al : allocators) {
    int combos = 0, failures = 0, spilled = 0;
    std::string first;
    for (const Named& pr : programs)
      for (int R : Rs)
        for (Numbering nu : numberings) {
          CheckOptions opt;
          opt.numbering = nu;
          opt.sharing = al.sharing;
          CheckResult r = check_allocation(pr.f, R, al.fn, opt);
          ++combos;
          if (r.num_spills > 0) ++spilled;
          if (!r.ok && failures++ == 0) first = pr.name + " R=" + std::to_string(R) + ": " + r.why;
        }
    check(failures == 0, al.name + ": " + std::to_string(combos) + " fixed combinations pass the harness (" + std::to_string(spilled) + " with spills)" + (failures ? "  FIRST FAILURE: " + first : ""));

    std::mt19937 rng(424242);
    combos = failures = spilled = 0;
    first.clear();
    for (int it = 0; it < 500; ++it) {
      const std::string src = random_program(rng, it % 2 == 0);
      const Function f = parse(src);
      for (int R : Rs)
        for (Numbering nu : numberings) {
          CheckOptions opt;
          opt.numbering = nu;
          opt.sharing = al.sharing;
          CheckResult r = check_allocation(f, R, al.fn, opt);
          ++combos;
          if (r.num_spills > 0) ++spilled;
          if (!r.ok && failures++ == 0) first = "R=" + std::to_string(R) + ": " + r.why + "\n" + src;
        }
    }
    check(failures == 0, al.name + ": " + std::to_string(combos) + " random combinations pass the harness (" + std::to_string(spilled) + " with spills)" + (failures ? "\n" + first : ""));
  }

  // ======================= usage-count: properties and the paper's behaviour =======================
  {
    const Function loop = programs[0].f;  // sum_loop

    // Hand calculation. Weights are v1=41, v0=22, v4=20, v2=11, v3=11, so at R=3 the three
    // registers go to v1, v0, v4 and v2, v3 are spilled. v2 is stored once and read by the
    // `lt` in each of the 11 loop tests; v3 is stored once and read by the `add` in each of the
    // 10 iterations of the body. So: 21 spill loads + 2 spill stores on top of the 59 base.
    CheckResult r = check_allocation(loop, 3, allocate_usage_count);
    check(r.ok && r.num_spills == 2 && r.allocated.spill_loads == 21 && r.allocated.spill_stores == 2 && r.allocated.executed == 59 + 23,
          "sum_loop at R=3: v2 and v3 spilled -> 21 loads + 2 stores, 82 instructions executed (got " +
              std::to_string(r.allocated.spill_loads) + " + " + std::to_string(r.allocated.spill_stores) + ", " + std::to_string(r.allocated.executed) + ")");

    std::cout << "      sum_loop, instructions executed by R (reference: " << r.reference.executed << "):";
    for (int R = 1; R <= 6; ++R) std::cout << "  R=" << R << ": " << check_allocation(loop, R, allocate_usage_count).allocated.executed;
    std::cout << "\n";
  }

  {
    // The cost of a spilled variable does not depend on which other variables are spilled, so
    // giving usage-count one more register can never make a program slower; and with a register for
    // every variable it costs exactly what the reference costs. The numbering cannot matter either.
    std::mt19937 rng(99);
    int programs_checked = 0, not_monotone = 0, not_free = 0, numbering_matters = 0;
    for (int it = 0; it < 300; ++it) {
      const Function f = parse(random_program(rng, it % 2 == 0));
      long prev = -1;
      long last = 0, ref = 0;
      for (int R = 1; R <= f.num_vars + 1; ++R) {
        CheckResult r = check_allocation(f, R, allocate_usage_count);
        if (!r.ok) { std::cout << "FAIL  unexpected harness failure: " << r.why << "\n"; ++g_bad; }
        if (prev >= 0 && r.allocated.executed > prev) ++not_monotone;
        prev = r.allocated.executed;
        last = r.allocated.executed;
        ref = r.reference.executed;

        Allocation df = allocate_usage_count(make_problem(f, R, Numbering::DepthFirst));
        Allocation ly = allocate_usage_count(make_problem(f, R, Numbering::Layout));
        if (df.reg != ly.reg || df.slot != ly.slot) ++numbering_matters;
      }
      if (last != ref) ++not_free;
      ++programs_checked;
    }
    check(not_monotone == 0, "usage-count: more registers never costs more (" + std::to_string(programs_checked) + " random programs, R = 1 .. num_vars+1)");
    check(not_free == 0, "usage-count: with a register for every variable the run costs exactly what the reference costs");
    check(numbering_matters == 0, "usage-count: depth-first and layout numbering give identical allocations");
  }

  // ======================= linear scan against usage-count =======================
  {
    // Usage-count never shares a register, so it spills exactly max(0, n - R) variables, where n counts
    // the variables with an interval. Linear scan reuses registers, so it can never spill more.
    // Dynamic cost is a different matter: linear scan picks victims by end point, not by use count,
    // so it can lose on a given program. We count both and only insist on the spill-count bound.
    std::mt19937 rng(2024);
    int cases = 0, more_spills = 0, ls_cheaper = 0, uc_cheaper = 0, equal = 0;
    long ls_total = 0, uc_total = 0, ls_spill_total = 0, uc_spill_total = 0;
    for (int it = 0; it < 300; ++it) {
      const Function f = parse(random_program(rng, it % 2 == 0));
      for (int R = 1; R <= f.num_vars; ++R) {
        const AllocProblem p = make_problem(f, R);
        const int ls_spills = allocate_linear_scan(p).num_spills;
        const int uc_spills = allocate_usage_count(p).num_spills;
        ls_spill_total += ls_spills;
        uc_spill_total += uc_spills;
        if (ls_spills > uc_spills) ++more_spills;
        CheckResult a = check_allocation(f, R, allocate_linear_scan);
        CheckResult b = check_allocation(f, R, allocate_usage_count);
        if (!a.ok || !b.ok) { std::cout << "FAIL  unexpected harness failure: " << (a.ok ? b.why : a.why) << "\n"; ++g_bad; }
        ++cases;
        ls_total += a.allocated.executed;
        uc_total += b.allocated.executed;
        if (a.allocated.executed < b.allocated.executed) ++ls_cheaper;
        else if (a.allocated.executed > b.allocated.executed) ++uc_cheaper;
        else ++equal;
      }
    }
    check(more_spills == 0, "linear scan never spills more variables than usage-count (" + std::to_string(cases) + " program x R cases)");
    check(ls_spill_total < uc_spill_total, "linear scan spills fewer variables than usage-count in total (" + std::to_string(ls_spill_total) + " vs " + std::to_string(uc_spill_total) + ")");
    // Deliberately NOT asserted: that linear scan executes fewer instructions. It chooses victims by end
    // point and ignores use counts, so on these synthetic programs it can lose (see the printout).
    std::cout << "      total executed instructions: linear scan " << ls_total << ", usage-count " << uc_total << "\n";
    std::cout << "      executed instructions, linear scan vs usage-count over " << cases << " random cases: linear scan cheaper in " << ls_cheaper
              << ", usage-count cheaper in " << uc_cheaper << ", equal in " << equal << "\n";
  }

  // ======================= the three allocators side by side =======================
  {
    CheckOptions by_intervals, by_interference;
    by_interference.sharing = SharingRule::Interference;

    // The only thing we can promise about graph coloring versus usage-count is on spill COUNTS: usage-count
    // spills max(0, n - R) variables, and coloring never spills more (K_n is the worst case, tested above).
    // Everything else is measured and printed, not asserted: the paper's claims are about benchmarks,
    // not about every random program.
    std::mt19937 rng(555);
    int cases = 0, gc_more_than_uc = 0, gc_fewer_than_ls = 0, gc_more_than_ls = 0, gc_pd_differs = 0;
    long ex_uc = 0, ex_ls = 0, ex_gc = 0, sp_uc = 0, sp_ls = 0, sp_gc = 0;
    for (int it = 0; it < 300; ++it) {
      const Function f = parse(random_program(rng, it % 2 == 0));
      for (int R = 1; R <= f.num_vars; ++R) {
        const CheckResult u = check_allocation(f, R, allocate_usage_count, by_intervals);
        const CheckResult l = check_allocation(f, R, allocate_linear_scan, by_intervals);
        const CheckResult g = check_allocation(f, R, allocate_graph_coloring, by_interference);
        if (!u.ok || !l.ok || !g.ok) { std::cout << "FAIL  unexpected harness failure\n"; ++g_bad; }
        ++cases;
        ex_uc += u.allocated.executed; ex_ls += l.allocated.executed; ex_gc += g.allocated.executed;
        sp_uc += u.num_spills; sp_ls += l.num_spills; sp_gc += g.num_spills;
        if (g.num_spills > u.num_spills) ++gc_more_than_uc;
        if (g.num_spills < l.num_spills) ++gc_fewer_than_ls;
        if (g.num_spills > l.num_spills) ++gc_more_than_ls;
        const AllocProblem p = make_problem(f, R);
        if (allocate_graph_coloring(p).reg != allocate_graph_coloring_with(p, SpillChoice::LowestWeightPerDegree).reg) ++gc_pd_differs;
      }
    }
    check(gc_more_than_uc == 0, "graph coloring never spills more variables than usage-count (" + std::to_string(cases) + " program x R cases)");
    std::cout << "      spilled variables in total over " << cases << " random cases:  usage-count " << sp_uc << ",  linear scan " << sp_ls << ",  graph coloring " << sp_gc << "\n";
    std::cout << "      coloring spilled fewer than linear scan in " << gc_fewer_than_ls << " cases, more in " << gc_more_than_ls << "\n";
    std::cout << "      executed instructions in total:  usage-count " << ex_uc << ",  linear scan " << ex_ls << ",  graph coloring " << ex_gc << "\n";
    std::cout << "      weight vs weight/degree spill choice gave different allocations in " << gc_pd_differs << " of those cases\n";

    const Function loop = fixed_programs(LINSCAN_SOURCE_DIR)[0].f;
    std::cout << "      sum_loop, executed instructions by R (reference " << check_allocation(loop, 6, allocate_usage_count).reference.executed << "):\n";
    for (const char* who : {"usage-count", "linear scan", "graph coloring"}) {
      std::cout << "        " << who << std::string(16 - std::string(who).size(), ' ');
      for (int R = 1; R <= 6; ++R) {
        CheckResult r = std::string(who) == "usage-count" ? check_allocation(loop, R, allocate_usage_count, by_intervals)
                      : std::string(who) == "linear scan" ? check_allocation(loop, R, allocate_linear_scan, by_intervals)
                                                          : check_allocation(loop, R, allocate_graph_coloring, by_interference);
        std::cout << " R=" << R << ": " << r.allocated.executed << " ";
      }
      std::cout << "\n";
    }
  }

  std::cout << (g_bad ? "FAILED" : "all passed") << "\n";
  return g_bad ? 1 : 0;
}
