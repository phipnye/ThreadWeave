#include <benchmark/benchmark.h>
#include <threadweave/ThreadPool.h>
#include <threadweave/utils.h>

#include <future>
#include <numeric>
#include <thread>
#include <vector>

using ThreadWeave::Future;
using ThreadWeave::Index;
using ThreadWeave::ThreadPool;

/**
 * Benchmarks a ThreadPool along three axes:
 * 1. Pure scheduling overhead (near-zero-cost tasks)
 * 2. Throughput as a function of work per task
 * 3. Throughput as a function of thread count
 */

// --- Parameter sets

constexpr Index NumTasks{10'000};
constexpr Index IterArgs[]{10, 100, 1'000, 10'000, 100'000, 1'000'000};
constexpr Index ThreadArgs[]{1, 2, 4, 8, 16};

static void nIterArgs(benchmark::Benchmark* b) {
  for (const Index nIter : IterArgs) {
    b->Arg(nIter);
  }
}

static void nIterAndThreadsArgs(benchmark::Benchmark* b) {
  for (const Index nIter : IterArgs) {
    for (const Index nThreads : ThreadArgs) {
      b->Args({nIter, nThreads});
    }
  }
}

// --- 1. Pure overhead benchmark:
// We use a nearly free function to isolate the cost of pushing and popping task
// from the thread pool giving us a sense of latency
static void threadPoolOverheadBM(benchmark::State& state) {
  const Index nThreads{state.range(0)};
  ThreadPool pool{nThreads};

  for (auto _ : state) {
    auto f{pool.submit([] { return 1; })};
    benchmark::DoNotOptimize(f.get());
  }

  state.SetItemsProcessed(state.iterations());
}

BENCHMARK(threadPoolOverheadBM)
    ->Apply(nIterArgs)
    ->UseRealTime()
    ->Unit(benchmark::kMicrosecond);

// --- 2 & 3. Throughput as a function of work per task and thread count
// We simulate a task doing busy work to gain an understanding of how the pool
// compares to other approaches as the work and hardware resources evolve

// --- Helper functions

// Helper to simulate some busy work
static double busyWork(const Index nIter) {
  double sum{0.0};

  for (Index i{0}; i < nIter; ++i) {
    sum += 1.0;
    benchmark::DoNotOptimize(sum);
  }

  return sum;
}

// Helper to run tasks sequentially
static double runSequential(const Index nTasks, const Index nIter) {
  double totalSums{0.0};

  for (Index t{0}; t < nTasks; ++t) {
    totalSums += busyWork(nIter);
    benchmark::DoNotOptimize(totalSums);
  }

  return totalSums;
}

// Helper to run tasks with one thread per task
static double runThreadPerTask(const Index nTasks, const Index nIter) {
  std::vector<std::jthread> threads{};
  threads.reserve(nTasks);
  std::vector<double> results(nTasks);

  for (Index t{0}; t < nTasks; ++t) {
    threads.emplace_back(
        [t, nIter, &results] { results[t] = busyWork(nIter); });
  }

  threads.clear();
  double totalSums{0.0};

  for (const double x : results) {
    totalSums += x;
    benchmark::DoNotOptimize(totalSums);
  }

  return totalSums;
}

static double runStdAsync(const Index nTasks, const Index nIter) {
  std::vector<std::future<double>> futures{};
  futures.reserve(nTasks);

  for (Index t{0}; t < nTasks; ++t) {
    futures.push_back(std::async(busyWork, nIter));
  }

  double totalSums{0.0};

  for (auto& f : futures) {
    totalSums += f.get();
    benchmark::DoNotOptimize(totalSums);
  }

  return totalSums;
}

// Helper to run tasks using ThreadWeave's thread pool
static double runPool(const Index nThreads, const Index nTasks,
                      const Index nIter) {
  ThreadPool pool{nThreads};
  std::vector<Future<double>> futures{};
  futures.reserve(nTasks);

  for (Index t{0}; t < nTasks; ++t) {
    futures.push_back(pool.submit(busyWork, nIter));
  }

  double totalSums{0.0};

  for (auto& f : futures) {
    totalSums += f.get();
    benchmark::DoNotOptimize(totalSums);
  }

  return totalSums;
}

// --- Benchmarks

// Sequential sweep
static void sequentialSweepBM(benchmark::State& state) {
  const Index nIter{state.range(0)};

  for (auto _ : state) {
    runSequential(NumTasks, nIter);
  }

  state.SetItemsProcessed(state.iterations() * NumTasks);
  state.counters["nIter"] = static_cast<double>(nIter);
}

BENCHMARK(sequentialSweepBM)
    ->Apply(nIterArgs)
    ->UseRealTime()
    ->Unit(benchmark::kMillisecond);

// Thread per task sweep
static void threadPerTaskSweepBM(benchmark::State& state) {
  const Index nIter{state.range(0)};

  for (auto _ : state) {
    runThreadPerTask(NumTasks, nIter);
  }

  state.SetItemsProcessed(state.iterations() * NumTasks);
  state.counters["nIter"] = static_cast<double>(nIter);
}

BENCHMARK(threadPerTaskSweepBM)
    ->Apply(nIterArgs)
    ->UseRealTime()
    ->Unit(benchmark::kMillisecond);

// Use std::async to submit tasks and let the implementation decipher how to
// allocate
static void stdAsyncBM(benchmark::State& state) {
  const Index nIter{state.range(0)};

  for (auto _ : state) {
    runStdAsync(NumTasks, nIter);
  }

  state.SetItemsProcessed(state.iterations() * NumTasks);
  state.counters["nIter"] = static_cast<double>(nIter);
}

BENCHMARK(stdAsyncBM)
    ->Apply(nIterArgs)
    ->UseRealTime()
    ->Unit(benchmark::kMillisecond);

// Use thread pool to submit tasks
static void poolBM(benchmark::State& state) {
  const Index nIter{state.range(0)};
  const Index nThreads{state.range(1)};

  for (auto _ : state) {
    runPool(nThreads, NumTasks, nIter);
  }

  state.SetItemsProcessed(state.iterations() * NumTasks);
  state.counters["nIter"] = static_cast<double>(nIter);
  state.counters["nThreads"] = static_cast<double>(nThreads);
}

BENCHMARK(poolBM)
    ->Apply(nIterAndThreadsArgs)
    ->UseRealTime()
    ->Unit(benchmark::kMillisecond);

BENCHMARK_MAIN();
