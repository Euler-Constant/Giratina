#!/usr/bin/env bash
# Regenerates every CSV in results/ from the optimized build.
#   tools/run_experiments.sh [build_dir]      (default: build)
# Use a quiet machine; the timings are wall-clock. Correctness is checked as it goes: the driver exits
# non-zero if any allocation fails the validator or changes what a program prints.
set -euo pipefail
cd "$(dirname "$0")/.."
BUILD="${1:-build}"
BENCH="$BUILD/linscan_bench"
[ -x "$BENCH" ] || { echo "build first: cmake -S . -B $BUILD && cmake --build $BUILD" >&2; exit 1; }
mkdir -p results bench/generated

python3 tools/gen_bench.py > /dev/null

# C1, C4-C7: the kernels, every allocator, R = 4, 8, 16, 32, both numberings.
"$BENCH" --experiment kernels --R 4,8,16,32 --alloc usage,linear,linear-weight,color,color-ratio \
         --numbering depth,layout --trials 10 --min-ms 10 --out results/kernels.csv bench/*.ir

# C2: type (a), n simultaneously live variables. The paper goes to 512; 1024 and 2048 extend it.
A_FILES=()
for n in 1 2 4 8 16 32 64 128 256 512 1024 2048; do
  python3 tools/gen_pathological.py a --n "$n" > "bench/generated/path_a_$n.ir"
  A_FILES+=("bench/generated/path_a_$n.ir")
done
"$BENCH" --experiment path_a --R 16 --alloc usage,linear,color --numbering depth \
         --trials 10 --min-ms 10 --out results/path_a.csv "${A_FILES[@]}"

# C3: type (b), k staggered sets, at most m live at once. `b` is pure intervals (our reading of the
# figure); `b-acc` adds a long-lived accumulator (see tools/gen_pathological.py for why both exist).
for kind in b b-acc; do
  tag="${kind//-/}"
  B_FILES=()
  for m in 8 16 24 32; do
    for k in 1 2 5 10 20 40 80; do
      python3 tools/gen_pathological.py "$kind" --k "$k" --m "$m" > "bench/generated/path_${tag}_${k}_${m}.ir"
      B_FILES+=("bench/generated/path_${tag}_${k}_${m}.ir")
    done
  done
  "$BENCH" --experiment "path_$tag" --R 16 --alloc linear,linear-weight,color,color-ratio --numbering depth \
           --trials 10 --min-ms 10 --out "results/path_$tag.csv" "${B_FILES[@]}"
done
