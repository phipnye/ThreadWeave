#include <tbb/parallel_sort.h>
#include <tbb/task_arena.h>
#include <threadweave/ThreadPool.h>

#include <algorithm>
#include <execution>
#include <functional>
#include <iterator>

#include "helpers.h"

using ThreadWeave::ThreadPool;

// --- Global parameters

constexpr Index kNumThreads{4};
constexpr Index kVecSizes[]{1'000'000, 10'000'000, 100'000'000};

static void sizeArgs(benchmark::Benchmark* b) {
  for (const Index size : kVecSizes) {
    b->Arg(size);
  }
}

// --- Sequential Baseline

static void stdSequentialSortPerformanceBM(benchmark::State& state) {
  state.SetLabel("category=Performance;library=std::sort_sequential");
  const Index size{state.range(0)};
  const std::vector<int> randVec{genRandVec(size)};
  std::vector<int> nums{};

  for (auto _ : state) {
    state.PauseTiming();
    nums = randVec;
    state.ResumeTiming();
    std::ranges::sort(nums);
  }
}

// --- ThreadWeave

static void twQuickSortPerformanceBM(benchmark::State& state) {
  state.SetLabel("category=Performance;library=ThreadWeave");
  const Index size{state.range(0)};
  ThreadPool pool{kNumThreads};
  const std::vector<int> randVec{genRandVec(size)};
  std::vector<int> nums{};

  for (auto _ : state) {
    state.PauseTiming();
    nums = randVec;
    state.ResumeTiming();
    auto f{pool.submit(
        [&] { parallelQuickSort(nums.begin(), nums.end(), pool); })};
    f.get();
  }
}

// --- Parallel STL

static void stdParallelSortPerformanceBM(benchmark::State& state) {
  state.SetLabel("category=Performance;library=std::execution::par");
  const Index size{state.range(0)};
  const std::vector<int> randVec{genRandVec(size)};
  std::vector<int> nums{};

  for (auto _ : state) {
    state.PauseTiming();
    nums = randVec;
    state.ResumeTiming();
    std::sort(std::execution::par, nums.begin(), nums.end());
  }
}

// --- TBB

static void tbbParallelSortPerformanceBM(benchmark::State& state) {
  state.SetLabel("category=Performance;library=oneTBB");
  const Index size{state.range(0)};
  tbb::task_arena arena{kNumThreads};
  const std::vector<int> randVec{genRandVec(size)};
  std::vector<int> nums{};

  for (auto _ : state) {
    state.PauseTiming();
    nums = randVec;
    state.ResumeTiming();
    arena.execute([&nums] { tbb::parallel_sort(nums.begin(), nums.end()); });
  }
}

// --- Register benchmarks

BENCHMARK(stdSequentialSortPerformanceBM)
    ->Apply(sizeArgs)
    ->UseRealTime()
    ->Unit(benchmark::kMillisecond);

BENCHMARK(twQuickSortPerformanceBM)
    ->Apply(sizeArgs)
    ->UseRealTime()
    ->Unit(benchmark::kMillisecond);

BENCHMARK(tbbParallelSortPerformanceBM)
    ->Apply(sizeArgs)
    ->UseRealTime()
    ->Unit(benchmark::kMillisecond);

BENCHMARK(stdParallelSortPerformanceBM)
    ->Apply(sizeArgs)
    ->UseRealTime()
    ->Unit(benchmark::kMillisecond);

BENCHMARK_MAIN();
