#pragma once
#include <vector>

#include "ir.h"

namespace ls {

// Per-block live-in / live-out sets, indexed [block][variable].
struct Liveness {
  std::vector<std::vector<bool>> live_in;
  std::vector<std::vector<bool>> live_out;
};

// Traditional iterative live variable analysis (the "Live variable analysis" bar in Fig. 3).
Liveness compute_liveness(const Function& f);

}  // namespace ls
