/* Benchmark execution time of ThreadWeave against BS::thread_pool across a
 * range of thread counts and task counts, for both balanced and unbalanced
 * workloads, to compare raw performance between thread pool implementations.
 */

#include <benchmark/benchmark.h>
#include <threadweave/ThreadPool.h>

#include <cstddef>
#include <future>
#include <vector>

#include "BS_thread_pool.hpp"
#include "helpers.h"

// --- Global parameters

constexpr Index kBaseIter{10'000'000};
constexpr Index kNumThreadArgs[]{2, 3, 4};
constexpr Index kNumTaskArgs[]{100, 1'000, 10'000};

static void latencyArgs(benchmark::Benchmark* b) {
  for (const Index nTasks : kNumTaskArgs) {
    for (const Index nThreads : kNumThreadArgs) {
      b->Args({nThreads, nTasks});
    }
  }
}

// --- Balanced workload benchmarking

// Benchmark ThreadWeave's pool across a range of number of threads and number
// of balanced (roughly equivalent amount of work) tasks
static void twBalancedComparisonBM(benchmark::State& state) {
  state.SetLabel("category=Comparison;library=ThreadWeave;workload=Balanced");
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

    // First submit a series of balanced tasks
    for (Index i{0}; i < nTasks; ++i) {
      futures.push_back(pool.submit(busyWork, kBaseIter));
    }

    // Then collect results after all tasks have been submitted
    for (auto& f : futures) {
      res += f.get();
      benchmark::DoNotOptimize(res);
    }
  }
}

// Benchmark BS's pool across a range of number of threads and number
// of balanced (roughly equivalent amount of work) tasks
static void bsBalancedComparisonBM(benchmark::State& state) {
  state.SetLabel(
      "category=Comparison;library=BS::thread_pool;workload=Balanced");
  const Index nThreads{state.range(0)};
  const Index nTasks{state.range(1)};
  BS::thread_pool pool{static_cast<std::size_t>(nThreads)};
  std::vector<std::future<Index>> futures{};
  futures.reserve(nTasks);
  Index res{0};

  for (auto _ : state) {
    state.PauseTiming();
    futures.clear();
    state.ResumeTiming();

    // First submit a series of balanced tasks
    for (Index i{0}; i < nTasks; ++i) {
      futures.push_back(pool.submit_task([] { return busyWork(kBaseIter); }));
    }

    // Then collect results after all tasks have been submitted
    for (auto& f : futures) {
      res += f.get();
      benchmark::DoNotOptimize(res);
    }
  }
}

// --- Unbalanced workload benchmarking

// Benchmark ThreadWeave's pool across a range of number of threads and number
// of unbalanced (unequal amount of work) tasks
static void twUnbalancedComparisonBM(benchmark::State& state) {
  state.SetLabel("category=Comparison;library=ThreadWeave;workload=Unbalanced");
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

    // First submit a series of unbalanced tasks
    for (const Index nIter : taskIters) {
      futures.push_back(pool.submit(busyWork, nIter));
    }

    // Then collect results after all tasks have been submitted
    for (auto& f : futures) {
      res += f.get();
      benchmark::DoNotOptimize(res);
    }
  }
}

// Benchmark BS's pool across a range of number of threads and number
// of unbalanced (unequal amount of work) tasks
static void bsUnbalancedComparisonBM(benchmark::State& state) {
  state.SetLabel(
      "category=Comparison;library=BS::thread_pool;workload=Unbalanced");
  const Index nThreads{state.range(0)};
  const Index nTasks{state.range(1)};
  BS::thread_pool pool{static_cast<std::size_t>(nThreads)};
  std::vector<std::future<Index>> futures{};
  futures.reserve(nTasks);
  const std::vector<Index> taskIters{genUnbalancedWorkloads(nTasks, kBaseIter)};
  Index res{0};

  for (auto _ : state) {
    state.PauseTiming();
    futures.clear();
    state.ResumeTiming();

    // First submit a series of unbalanced tasks
    for (const Index nIter : taskIters) {
      futures.push_back(pool.submit_task([nIter] { return busyWork(nIter); }));
    }

    // Then collect results after all tasks have been submitted
    for (auto& f : futures) {
      res += f.get();
      benchmark::DoNotOptimize(res);
    }
  }
}

// --- Register benchmarks

BENCHMARK(twBalancedComparisonBM)
    ->Apply(latencyArgs)
    ->UseRealTime()
    ->Unit(benchmark::kMillisecond);

BENCHMARK(bsBalancedComparisonBM)
    ->Apply(latencyArgs)
    ->UseRealTime()
    ->Unit(benchmark::kMillisecond);

BENCHMARK(twUnbalancedComparisonBM)
    ->Apply(latencyArgs)
    ->UseRealTime()
    ->Unit(benchmark::kMillisecond);

BENCHMARK(bsUnbalancedComparisonBM)
    ->Apply(latencyArgs)
    ->UseRealTime()
    ->Unit(benchmark::kMillisecond);

BENCHMARK_MAIN();
