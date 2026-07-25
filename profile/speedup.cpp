#include <benchmark/benchmark.h>
#include <threadweave/ThreadPool.h>

#include <cstddef>
#include <future>
#include <random>
#include <vector>

#include "../cmake-build-release/_deps/bshoshany_thread_pool-src/include/BS_thread_pool.hpp"

using ThreadWeave::Index;

// --- Global parameters

constexpr Index kBaseIter{10'000'000};
constexpr Index kNumThreadArgs[]{2, 4, 8, 12};
constexpr Index kNumTaskArgs[]{100, 1'000, 10'000};

// --- Work helper functions

static Index busyWork(const Index nIter) {
  Index sum{0};

  for (Index i{0}; i < nIter; ++i) {
    sum += 1;
    benchmark::DoNotOptimize(sum);
  }

  return sum / nIter;  // prevent overflow
}

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

// --- Target functions

static void runTwBalanced(const Index nThreads, const Index nTasks) {
  ThreadWeave::ThreadPool pool{nThreads};
  std::vector<ThreadWeave::Future<Index>> futures{};
  futures.reserve(nTasks);
  Index res{0};

  for (Index i{0}; i < nTasks; ++i) {
    futures.push_back(pool.submit(busyWork, kBaseIter));
  }

  for (auto& f : futures) {
    res += f.get();
    benchmark::DoNotOptimize(res);
  }
}

static void runBsBalanced(const Index nThreads, const Index nTasks) {
  BS::thread_pool pool{static_cast<std::size_t>(nThreads)};
  std::vector<std::future<Index>> futures{};
  futures.reserve(nTasks);
  Index res{0};

  for (Index i{0}; i < nTasks; ++i) {
    futures.push_back(pool.submit_task([] { return busyWork(kBaseIter); }));
  }

  for (auto& f : futures) {
    res += f.get();
    benchmark::DoNotOptimize(res);
  }
}

static void runTwUnbalanced(const Index nThreads, const Index nTasks) {
  ThreadWeave::ThreadPool pool{nThreads};
  std::vector<ThreadWeave::Future<Index>> futures{};
  futures.reserve(nTasks);
  const std::vector<Index> taskIters{genUnbalancedWorkloads(nTasks, kBaseIter)};
  Index res{0};

  for (const Index nIter : taskIters) {
    futures.push_back(pool.submit(busyWork, nIter));
  }

  for (auto& f : futures) {
    res += f.get();
    benchmark::DoNotOptimize(res);
  }
}

static void runBsUnbalanced(const Index nThreads, const Index nTasks) {
  BS::thread_pool pool{static_cast<std::size_t>(nThreads)};
  std::vector<std::future<Index>> futures{};
  futures.reserve(nTasks);
  const std::vector<Index> taskIters{genUnbalancedWorkloads(nTasks, kBaseIter)};
  Index res{0};

  for (const Index nIter : taskIters) {
    futures.push_back(pool.submit_task([nIter] { return busyWork(nIter); }));
  }

  for (auto& f : futures) {
    res += f.get();
    benchmark::DoNotOptimize(res);
  }
}

int main() {
  constexpr Index nThreads{8};
  constexpr Index nTasks{1'000};

  runTwBalanced(nThreads, nTasks);
  // runBsBalanced(nThreads, nTasks);
  // runTwUnbalanced(nThreads, nTasks);
  // runBsUnbalanced(nThreads, nTasks);

  return 0;
}
