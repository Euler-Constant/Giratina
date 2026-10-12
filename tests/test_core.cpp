// Usage: test_core validator | fig2
#include <cstdio>
#include <string>

#include "alloc.h"
#include "common.h"
#include "validate.h"

using namespace ls;

static int g_failures = 0;

#define CHECK(cond)                                                    \
  do {                                                                 \
    if (!(cond)) {                                                     \
      std::printf("  FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);    \
      ++g_failures;                                                    \
    }                                                                  \
  } while (0)

// The live intervals of the paper's Figure 2 (Sec. 4.2), R = 2.
// Variables A..E are v0..v4. The paper gives only the picture, so the endpoints here
// are chosen to reproduce its walkthrough:
//   step 3: C arrives with A and B active; C ends last, so C is spilled
//   step 4: A has expired, D takes A's register
//   step 5: B has expired, E takes B's register
static AllocProblem fig2() {
  AllocProblem p;
  p.num_vars = 5;
  p.R = 2;
  p.intervals = {
      {0, 1, 5},   // A
      {1, 2, 7},   // B
      {2, 3, 12},  // C
      {3, 6, 10},  // D
      {4, 8, 11},  // E
  };
  return p;
}

static void test_validator() {
  AllocProblem p = fig2();

  Allocation ok(5);
  ok.reg = {0, 1, -1, 0, 1};
  ok.slot = {-1, -1, 0, -1, -1};
  ok.num_spills = 1;
  CHECK(validate_allocation(p, ok, nullptr));

  Allocation clash = ok;  // A and B overlap and now share a register
  clash.reg[1] = 0;
  CHECK(!validate_allocation(p, clash, nullptr));

  Allocation lost = ok;  // C has neither a register nor a slot
  lost.slot[2] = -1;
  CHECK(!validate_allocation(p, lost, nullptr));

  Allocation out_of_range = ok;  // register 2 does not exist when R = 2
  out_of_range.reg[0] = 2;
  CHECK(!validate_allocation(p, out_of_range, nullptr));

  Allocation miscounted = ok;
  miscounted.num_spills = 0;
  CHECK(!validate_allocation(p, miscounted, nullptr));
}

static void test_fig2() {
  AllocProblem p = fig2();
  Allocation a = allocate_linear_scan(p);

  std::string why;
  CHECK(validate_allocation(p, a, &why));
  if (!why.empty()) std::printf("  validator: %s\n", why.c_str());

  CHECK(a.num_spills == 1);
  CHECK(a.reg[2] == -1);        // C is the only variable not in a register
  CHECK(a.reg[3] == a.reg[0]);  // D reuses A's register
  CHECK(a.reg[4] == a.reg[1]);  // E reuses B's register
}

int main(int argc, char** argv) {
  const std::string which = argc > 1 ? argv[1] : "";
  try {
    if (which == "validator") test_validator();
    else if (which == "fig2") test_fig2();
    else {
      std::printf("usage: test_core validator | fig2\n");
      return 2;
    }
  } catch (const NotImplemented& e) {
    std::printf("  NOT IMPLEMENTED: %s\n", e.what());
    return 1;
  }
  std::printf("%s: %s\n", which.c_str(), g_failures ? "FAILED" : "ok");
  return g_failures ? 1 : 0;
}
