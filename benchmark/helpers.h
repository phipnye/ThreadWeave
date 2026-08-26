#ifndef TW_BM_HELPERS_H
#define TW_BM_HELPERS_H
#include <benchmark/benchmark.h>

#include <algorithm>
#include <functional>
#include <iterator>
#include <limits>
#include <random>

#include "threadweave/ThreadPool.h"
#include "threadweave/internal/utils.h"

using ThreadWeave::Index;

// Deliberately trvial task for measuring latency
inline Index trivialTask() {
  Index res{1};
  benchmark::DoNotOptimize(res);
  return res;
}

// Helper to simulate computation
inline Index busyWork(const Index nIter) {
  Index sum{0};

  for (Index i{0}; i < nIter; ++i) {
    sum += 1;
    benchmark::DoNotOptimize(sum);
  }

  return sum / nIter;  // prevent overflow
}

// Generate a series of imbalanced workloads
inline std::vector<Index> genUnbalancedWorkloads(
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

// Generate a vector of random numbers
inline std::vector<int> genRandVec(const Index size) {
  std::mt19937 rng{68};  // NOLINT(*-msc51-cpp)
  std::uniform_int_distribution dist{std::numeric_limits<int>::min(),
                                     std::numeric_limits<int>::max()};
  std::vector<int> randVec{};
  randVec.reserve(size);

  for (Index i{0}; i < size; ++i) {
    randVec.push_back(dist(rng));
  }

  return randVec;
}

#endif
