/* Benchmark two distinct metrics:
 * 1. Single-task latency (submission, scheduling, and retrieval).
 * 2. Amortized batch task time (average throughput across N tasks).
 */

#include <benchmark/benchmark.h>
#include <threadweave/ThreadPool.h>

#include <vector>

#include "helpers.h"

// --- Global parameters

constexpr Index kNumThreadArgs[]{1, 2, 3, 4};
constexpr Index kNumTaskArgs[]{100, 1'000, 10'000};

static void singleLatencyArgs(benchmark::Benchmark* b) {
  for (const Index nThreads : kNumThreadArgs) {
    b->Args({nThreads});
  }
}

static void batchArgs(benchmark::Benchmark* b) {
  for (const Index nTasks : kNumTaskArgs) {
    for (const Index nThreads : kNumThreadArgs) {
      b->Args({nThreads, nTasks});
    }
  }
}

// Isolates the overhead of submitting ONE task and immediately waiting for it.
// This eliminates batching, pipeline overlap, and queue saturation effects.
static void twLatencySingleTaskOverheadBM(benchmark::State& state) {
  state.SetLabel("category=Latency;library=ThreadWeave");
  const Index nThreads{state.range(0)};
  ThreadWeave::ThreadPool pool{nThreads};
  Index res{0};

  for (auto _ : state) {
    auto f{pool.submit(trivialTask)};
    res += f.get();
    benchmark::DoNotOptimize(res);
  }
}

// Measures bulk scheduling and concurrent execution under heavy queue load.
static void twLatencyBatchThroughputBM(benchmark::State& state) {
  state.SetLabel("category=Latency;library=ThreadWeave");
  const Index nThreads{state.range(0)};
  const Index nTasks{state.range(1)};
  ThreadWeave::ThreadPool pool{nThreads};
  Index res{0};
  std::vector<ThreadWeave::Future<Index>> futures{};
  futures.reserve(nTasks);

  for (auto _ : state) {
    state.PauseTiming();
    futures.clear();
    state.ResumeTiming();

    for (Index i{0}; i < nTasks; ++i) {
      futures.push_back(pool.submit(trivialTask));
    }

    for (auto& f : futures) {
      res += f.get();
      benchmark::DoNotOptimize(res);
    }
  }

  const auto totalIter{nTasks * state.iterations()};
  state.counters["amortized_service_time"] = benchmark::Counter(
      totalIter, benchmark::Counter::kIsRate | benchmark::Counter::kInvert);
}

// --- Register benchmarks

BENCHMARK(twLatencySingleTaskOverheadBM)
    ->Apply(singleLatencyArgs)
    ->UseRealTime()
    ->Unit(benchmark::kMicrosecond);

BENCHMARK(twLatencyBatchThroughputBM)
    ->Apply(batchArgs)
    ->UseRealTime()
    ->Unit(benchmark::kMicrosecond);

BENCHMARK_MAIN();