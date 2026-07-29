/* Benchmark ThreadWeave's execution time across a range of worker threads
 * (1 to 4) for balanced and unbalanced task workloads, to calculate parallel
 * speedup (T_1 / T_p) and efficiency (speedup / p).
 */

#include <benchmark/benchmark.h>
#include <threadweave/ThreadPool.h>

#include <vector>

#include "helpers.h"

// --- Global parameters

constexpr Index kBaseIter{20'000'000};
constexpr Index kNumThreadArgs[]{1, 2, 3, 4};
constexpr Index kNumTaskArgs[]{100, 1'000, 10'000};

static void latencyArgs(benchmark::Benchmark* b) {
  for (const Index nTasks : kNumTaskArgs) {
    for (const Index nThreads : kNumThreadArgs) {
      b->Args({nThreads, nTasks});
    }
  }
}

// --- Speedup benchmarking

// Benchmark ThreadWeave's pool across a range of number of threads (including
// the nThreads=1 baseline) and number of balanced tasks, to compute T_1 / T_p
static void twBalancedSpeedupBM(benchmark::State& state) {
  state.SetLabel("category=Speedup;library=ThreadWeave;workload=Balanced");
  const Index nThreads{state.range(0)};
  const Index nTasks{state.range(1)};
  ThreadWeave::ThreadPool pool{nThreads};
  std::vector<ThreadWeave::Future<Index>> futures{};
  futures.reserve(nTasks);
  Index res{0};

  for (auto _ : state) {
    state.PauseTiming();
    futures.clear();
    state.ResumeTiming();

    for (Index i{0}; i < nTasks; ++i) {
      futures.push_back(pool.submit(busyWork, kBaseIter));
    }

    for (auto& f : futures) {
      res += f.get();
      benchmark::DoNotOptimize(res);
    }
  }
}

// Benchmark ThreadWeave's pool across a range of number of threads (including
// the nThreads=1 baseline) and number of unbalanced tasks, to compute T_1 / T_p
static void twUnbalancedSpeedupBM(benchmark::State& state) {
  state.SetLabel("category=Speedup;library=ThreadWeave;workload=Unbalanced");
  const Index nThreads{state.range(0)};
  const Index nTasks{state.range(1)};
  ThreadWeave::ThreadPool pool{nThreads};
  std::vector<ThreadWeave::Future<Index>> futures{};
  futures.reserve(nTasks);
  const std::vector<Index> taskIters{genUnbalancedWorkloads(nTasks, kBaseIter)};
  Index res{0};

  for (auto _ : state) {
    state.PauseTiming();
    futures.clear();
    state.ResumeTiming();

    for (const Index nIter : taskIters) {
      futures.push_back(pool.submit(busyWork, nIter));
    }

    for (auto& f : futures) {
      res += f.get();
      benchmark::DoNotOptimize(res);
    }
  }
}

// --- Register benchmarks

BENCHMARK(twBalancedSpeedupBM)
    ->Apply(latencyArgs)
    ->UseRealTime()
    ->Unit(benchmark::kMillisecond);

BENCHMARK(twUnbalancedSpeedupBM)
    ->Apply(latencyArgs)
    ->UseRealTime()
    ->Unit(benchmark::kMillisecond);

BENCHMARK_MAIN();
