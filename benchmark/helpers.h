#ifndef TW_BM_HELPERS_H
#define TW_BM_HELPERS_H
#include <benchmark/benchmark.h>
#include <threadweave/ThreadPool.h>
#include <threadweave/internal/utils.h>

#include <algorithm>
#include <functional>
#include <iterator>
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

// --- The following were taken from TBB's parallel sorting algorithm to try to
// mimic the work it does under the hood

// Compute median of three numbers
template <typename Iter, typename Compare>
auto medianThree(Iter i1, Iter i2, Iter i3, const Compare& comp) {
  const auto a{*i1};
  const auto b{*i2};
  const auto c{*i3};
  return comp(a, b) ? (comp(b, c) ? b : (comp(a, c) ? c : a))
                    : (comp(c, b) ? b : (comp(c, a) ? c : a));
}

// Pick a pivot based on the median of three of three medians (pseudo median of
// nine)
template <typename Iter, typename Compare>
auto pickPivot(Iter begin, Iter end, const Compare& comp) {
  const auto d{std::distance(begin, end) / 8};
  const auto m1{medianThree(begin, begin + d, begin + 2 * d, comp)};
  const auto m2{medianThree(begin + 3 * d, begin + 4 * d, begin + 5 * d, comp)};
  const auto m3{
      medianThree(begin + 6 * d, begin + 7 * d, std::prev(end), comp)};
  return medianThree(&m1, &m2, &m3, comp);
}

template <typename Iter, typename Compare>
Iter partition(Iter begin, Iter end, const auto piv, const Compare& comp) {
  Iter i{begin};
  Iter j{std::prev(end)};

  while (true) {
    while (comp(*i, piv)) {
      ++i;
    }

    while (comp(piv, *j)) {
      --j;
    }

    if (i >= j) {
      return j;
    }

    std::iter_swap(i, j);
    ++i;
    --j;
  }
}

template <typename Iter, typename Compare>
void parallelQuickSort(Iter begin, Iter end, ThreadWeave::ThreadPool& pool,
                       const Compare& comp) {
  const auto d{std::distance(begin, end)};

  // Base case: fewer than cutoff elements, fallback to std::sort
  constexpr Index kCutoff{500};  // mimic tbb

  if (d < kCutoff) {
    std::sort(begin, end, comp);
    return;
  }

  // Partition around a pivot
  const auto piv{pickPivot(begin, end, comp)};
  const Iter mid{partition(begin, end, piv, comp)};
  const Iter right{std::next(mid)};
  const auto leftSize{std::distance(begin, right)};

  if (leftSize < kCutoff) {
    std::sort(begin, right, comp);
    parallelQuickSort(right, end, pool, comp);
  } else {
    // Recursive task submission only in instances where the task is
    // sufficiently large
    auto futL{pool.submit([begin, right, &pool, &comp] {
      parallelQuickSort(begin, right, pool, comp);
    })};
    parallelQuickSort(right, end, pool, comp);
    futL.wait();
  }
}

// Default comparator overload, mirroring TBB's parallel_sort(begin, end)
template <typename Iter>
void parallelQuickSort(Iter begin, Iter end, ThreadWeave::ThreadPool& pool) {
  parallelQuickSort(
      begin, end, pool,
      std::less<typename std::iterator_traits<Iter>::value_type>());
}

#endif
