/* Benchmark execution time across a number of threads for a range of number of
 * tasks to identify how execution time scales/speeds up as the number of
 * workers increases.
 */

#include <benchmark/benchmark.h>
#include <threadweave/ThreadPool.h>

#include <cstddef>
#include <future>
#include <random>
#include <vector>

#include "BS_thread_pool.hpp"

using ThreadWeave::Index;

// --- Global parameters

constexpr Index kBaseIter{20'000'000};
constexpr Index kNumThreadArgs[]{2, 3, 4};
constexpr Index kNumTaskArgs[]{100, 1'000, 10'000};

static void nTasksAndThreadsArgs(benchmark::Benchmark* b) {
  for (const Index nTasks : kNumTaskArgs) {
    for (const Index nThreads : kNumThreadArgs) {
      b->Args({nThreads, nTasks});
    }
  }
}

// --- Work helper functions

// Helper to simulate computation
static Index busyWork(const Index nIter) {
  Index sum{0};

  for (Index i{0}; i < nIter; ++i) {
    sum += 1;
    benchmark::DoNotOptimize(sum);
  }

  return sum / nIter;  // prevent overflow
}

// Generate a series of imbalanced workloads
static std::vector<Index> genUnbalancedWorkloads(
    const Index nTasks, const Index baseIter, const Index pctDeviation = 50) {
  std::mt19937 rng{42};  // NOLINT(*-msc51-cpp)
  std::uniform_int_distribution<Index> pctDist{-pctDeviation, pctDeviation};
  std::vector<Index> work{};
  work.reserve(nTasks);

  for (Index i{0}; i < nTasks; ++i) {
    work.push_back(baseIter + ((baseIter * pctDist(rng)) / 100));
  }

  return work;
}

// --- Balanced workload benchmarking

// Benchmark ThreadWeave's pool across a range of number of threads and number
// of balanced (roughly equivalent amount of work) tasks
static void twBalancedWorkloadBM(benchmark::State& state) {
  state.SetLabel("library=ThreadWeave;workload=Balanced");
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
static void bsBalancedWorkloadBM(benchmark::State& state) {
  state.SetLabel("library=BS::thread_pool;workload=Balanced");
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
static void twUnbalancedWorkloadBM(benchmark::State& state) {
  state.SetLabel("library=ThreadWeave;workload=Unbalanced");
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
static void bsUnbalancedWorkloadBM(benchmark::State& state) {
  state.SetLabel("library=BS::thread_pool;workload=Unbalanced");
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

BENCHMARK(twBalancedWorkloadBM)
    ->Apply(nTasksAndThreadsArgs)
    ->UseRealTime()
    ->Unit(benchmark::kMillisecond);

BENCHMARK(bsBalancedWorkloadBM)
    ->Apply(nTasksAndThreadsArgs)
    ->UseRealTime()
    ->Unit(benchmark::kMillisecond);

BENCHMARK(twUnbalancedWorkloadBM)
    ->Apply(nTasksAndThreadsArgs)
    ->UseRealTime()
    ->Unit(benchmark::kMillisecond);

BENCHMARK(bsUnbalancedWorkloadBM)
    ->Apply(nTasksAndThreadsArgs)
    ->UseRealTime()
    ->Unit(benchmark::kMillisecond);

BENCHMARK_MAIN();
