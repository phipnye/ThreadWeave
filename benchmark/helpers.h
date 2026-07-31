#ifndef TW_BM_HELPERS_H
#define TW_BM_HELPERS_H
#include <threadweave/internal/utils.h>
#include <benchmark/benchmark.h>
#include <threadweave/ThreadPool.h>

#include <limits>
#include <random>

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

// Compute median of three numbers
template <typename Iter>
auto medianThree(Iter i1, Iter i2, Iter i3) {
  const auto a{*i1};
  const auto b{*i2};
  const auto c{*i3};
  return (a < b) ? ((b < c) ? b : ((a < c) ? c : a))
                 : ((c < b) ? b : ((c < a) ? c : a));
}

// Pick a pivot based on the median of three of three medians (pseudo median of
// nine)
template <typename Iter>
auto pickPivot(Iter begin, Iter end) {
  const auto d{std::distance(begin, end) / 8};
  const auto m1{medianThree(begin, begin + d, begin + 2 * d)};
  const auto m2{medianThree(begin + 3 * d, begin + 4 * d, begin + 5 * d)};
  const auto m3{medianThree(begin + 6 * d, begin + 7 * d, std::prev(end))};
  return medianThree(&m1, &m2, &m3);
}

template <typename Iter>
void parallelQuickSort(Iter begin, Iter end, ThreadWeave::ThreadPool& pool) {
  const auto d{std::distance(begin, end)};

  // Base case: fewer than cutoff elements, fallback to std::sort
  constexpr Index kCutoff{500};  // mimic tbb

  if (d < kCutoff) {
    std::sort(begin, end);
    return;
  }

  // Partition around a pivot
  const auto piv{pickPivot(begin, end)};
  const Iter mid1{
      std::partition(begin, end, [piv](const auto& ele) { return ele < piv; })};
  const Iter mid2{
      std::partition(mid1, end, [piv](const auto& ele) { return piv >= ele; })};

  // Recursive task submission for left side
  auto futL{pool.submit(
      [begin, mid1, &pool] { parallelQuickSort(begin, mid1, pool); })};

  // Process right side synchronously on current thread
  parallelQuickSort(mid2, end, pool);
  futL.wait();
}

#endif
