#include <benchmark/benchmark.h>
#include <threadweave/ThreadPool.h>

#include <vector>

using ThreadWeave::Index;

// --- Global Parameters (Configure all sweeps here)

constexpr Index NumThreadArgs[]{1, 2, 4, 8, 12};
constexpr Index NumBatchTaskArgs[]{10, 100, 1'000, 10'000};
constexpr Index GranularityIterArgs[]{10, 50, 100, 500, 1'000, 5'000, 10'000, 50'000, 100'000};

// --- Argument Generators

// Generates thread sweeps: {1}, {2}, {4}, {8}, {12}
static void threadCountArgs(benchmark::Benchmark* b) {
  for (const Index nThreads : NumThreadArgs) {
    b->Arg(nThreads);
  }
}

// Generates matrix of threads x batch sizes
static void batchArgs(benchmark::Benchmark* b) {
  for (const Index nThreads : NumThreadArgs) {
    for (const Index nTasks : NumBatchTaskArgs) {
      b->Args({nThreads, nTasks});
    }
  }
}

// Generates matrix of threads x task durations
static void parallelGranularityArgs(benchmark::Benchmark* b) {
  for (const Index nThreads : NumThreadArgs) {
    for (const Index nIter : GranularityIterArgs) {
      b->Args({nThreads, nIter});
    }
  }
}

// Generates single-thread iteration sweeps for sequential baseline
static void sequentialGranularityArgs(benchmark::Benchmark* b) {
  for (const Index nIter : GranularityIterArgs) {
    b->Arg(nIter);
  }
}

// --- Work Helper Functions

static Index busyWork(const Index nIter) {
  Index sum{0};
  for (Index i{0}; i < nIter; ++i) {
    sum += 1;
    benchmark::DoNotOptimize(sum);
  }
  return sum;
}

// --- 1. Single-Task Latency vs. Thread Count
// Measures how thread pool size impacts single task submission & retrieval overhead.

static void twSingleTaskLatencyBM(benchmark::State& state) {
  state.SetLabel("library=ThreadWeave;type=SingleTaskLatency");
  const Index nThreads{static_cast<Index>(state.range(0))};
  ThreadWeave::ThreadPool pool{nThreads};

  for (auto _ : state) {
    auto f{pool.submit([] { return Index{1}; })};
    Index res{f.get()};
    benchmark::DoNotOptimize(res);
  }
}

// --- 2. Batch Latency vs. Thread Count & Task Count
// Measures enqueue/dequeue contention as both worker count and task count scale.

static void twBatchLatencyBM(benchmark::State& state) {
  state.SetLabel("library=ThreadWeave;type=BatchLatency");
  const Index nThreads{static_cast<Index>(state.range(0))};
  const Index nTasks{static_cast<Index>(state.range(1))};

  ThreadWeave::ThreadPool pool{nThreads};
  std::vector<ThreadWeave::Future<Index>> futures{};
  futures.reserve(nTasks);
  Index res{0};

  for (auto _ : state) {
    state.PauseTiming();
    futures.clear();
    state.ResumeTiming();

    for (Index i{0}; i < nTasks; ++i) {
      futures.push_back(pool.submit([] { return Index{1}; }));
    }

    for (auto& f : futures) {
      res += f.get();
      benchmark::DoNotOptimize(res);
    }
  }
  state.SetItemsProcessed(state.iterations() * nTasks);
}

// --- 3. Sequential Baseline (Single Thread, No Pool)

static void sequentialGranularityBM(benchmark::State& state) {
  state.SetLabel("library=Sequential;type=Granularity");
  const Index nIter{static_cast<Index>(state.range(0))};
  constexpr Index nTasks{1'000};
  Index res{0};

  for (auto _ : state) {
    for (Index i{0}; i < nTasks; ++i) {
      res += busyWork(nIter);
      benchmark::DoNotOptimize(res);
    }
  }
  state.SetItemsProcessed(state.iterations() * nTasks);
}

// --- 4. Parallel Granularity Crossover vs. Thread Count & Workload
// Finds the crossover point where parallel execution beats sequential execution
// across different thread pool sizes.

static void twParallelGranularityBM(benchmark::State& state) {
  state.SetLabel("library=ThreadWeave;type=Granularity");
  const Index nThreads{static_cast<Index>(state.range(0))};
  const Index nIter{static_cast<Index>(state.range(1))};
  constexpr Index nTasks{1'000};

  ThreadWeave::ThreadPool pool{nThreads};
  std::vector<ThreadWeave::Future<Index>> futures{};
  futures.reserve(nTasks);
  Index res{0};

  for (auto _ : state) {
    state.PauseTiming();
    futures.clear();
    state.ResumeTiming();

    for (Index i{0}; i < nTasks; ++i) {
      futures.push_back(pool.submit(busyWork, nIter));
    }

    for (auto& f : futures) {
      res += f.get();
      benchmark::DoNotOptimize(res);
    }
  }
  state.SetItemsProcessed(state.iterations() * nTasks);
}

// --- Registration

BENCHMARK(twSingleTaskLatencyBM)
    ->Apply(threadCountArgs)
    ->UseRealTime()
    ->Unit(benchmark::kNanosecond);

BENCHMARK(twBatchLatencyBM)
    ->Apply(batchArgs)
    ->UseRealTime()
    ->Unit(benchmark::kNanosecond);

BENCHMARK(sequentialGranularityBM)
    ->Apply(sequentialGranularityArgs)
    ->UseRealTime()
    ->Unit(benchmark::kMicrosecond);

BENCHMARK(twParallelGranularityBM)
    ->Apply(parallelGranularityArgs)
    ->UseRealTime()
    ->Unit(benchmark::kMicrosecond);

BENCHMARK_MAIN();