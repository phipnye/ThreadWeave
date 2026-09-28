#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <bitset>
#include <exception>
#include <format>
#include <future>
#include <numeric>
#include <random>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <vector>

#include "threadweave/ThreadPool.h"
#include "threadweave/internal/Future.h"
#include "threadweave/internal/utils.h"

using namespace ThreadWeave;

// Try submitting just a single simple task
TEST(ThreadPoolTests, SingleTask) {
  ThreadPool pool{1};
  constexpr int expectedVal{42};
  Future<int> f{pool.submit([] { return expectedVal; })};
  EXPECT_EQ(expectedVal, f.get());
}

// Try submitting multiple simple tasks
TEST(ThreadPoolTests, MultipleTasks) {
  constexpr int nTasks{1'000};
  ThreadPool pool{1};
  std::vector<Future<int>> futures{};
  futures.reserve(nTasks);

  // Submit a bunch of tasks that return just the iteration number
  for (int i{0}; i < nTasks; ++i) {
    futures.push_back(pool.submit([i] { return i; }));
  }

  // Make sure we recover every task
  std::bitset<nTasks> seen{};

  for (auto& f : futures) {
    const int val{f.get()};
    EXPECT_GE(val, 0);
    EXPECT_LT(val, nTasks);
    EXPECT_FALSE(seen.test(val));
    seen.set(val);
  }

  EXPECT_TRUE(seen.all());
}

// Verify pool works with one worker
TEST(ThreadPoolTests, SingleWorkerEdgeCase) {
  ThreadPool pool{1};
  constexpr int nTasks{100};
  std::vector<Future<int>> futures{};
  futures.reserve(nTasks);

  for (int i{0}; i < nTasks; ++i) {
    futures.push_back(pool.submit([i] { return i * 2; }));
  }

  for (int i{0}; i < nTasks; ++i) {
    EXPECT_EQ(futures[i].get(), i * 2);
  }
}

// Make sure pool preserves exceptions
TEST(ThreadPoolTests, PreservesException) {
  ThreadPool pool{2};
  auto f{pool.submit([] {
    throw std::runtime_error{"Error"};
    return 2;  // won't reach but prevents future<void>
  })};
  EXPECT_THROW(f.get(), std::runtime_error);
}

// Verify exceptions do not kill workers
TEST(ThreadPoolTests, WorkersSurviveExceptions) {
  constexpr int nTasks{100};
  ThreadPool pool{4};
  std::vector<Future<void>> failures{};

  for (int i{0}; i < nTasks; ++i) {
    failures.push_back(pool.submit([] { throw std::runtime_error{"Error"}; }));
  }

  for (auto& f : failures) {
    EXPECT_THROW(f.get(), std::runtime_error);
  }

  auto f{pool.submit([] { return 42; })};
  EXPECT_EQ(f.get(), 42);
}

// Make sure tasks finish in destructor and that run longer do not prevent other
// tasks from completing
TEST(ThreadPoolTests, HandlesMixedTaskDurations) {
  constexpr int nTasks{1'000};
  std::atomic<int> completed{0};

  {
    ThreadPool pool{4};

    for (int i{0}; i < nTasks; ++i) {
      pool.submit([&, i] {
        if (i % 10 == 0) {
          std::this_thread::yield();
        }

        completed.fetch_add(1, MemoryOrder::relaxed);
      });
    }
  }

  EXPECT_EQ(completed.load(MemoryOrder::relaxed), nTasks);
}

// Make sure multiple threads can submit work concurrently
TEST(ThreadPoolTests, ConcurrentSubmissions) {
  constexpr int nSubmitters{8};
  constexpr int tasksPerSubmitter{1'000};
  ThreadPool pool{};
  std::vector<std::jthread> submitters{};
  std::vector<std::vector<Future<int>>> futures(nSubmitters);

  for (int t{0}; t < nSubmitters; ++t) {
    submitters.emplace_back([&, t] {
      auto& local{futures[t]};
      local.reserve(tasksPerSubmitter);

      for (int i{0}; i < tasksPerSubmitter; ++i) {
        local.push_back(
            pool.submit([id = t * tasksPerSubmitter + i] { return id; }));
      }
    });
  }

  submitters.clear();
  constexpr int nTasks{nSubmitters * tasksPerSubmitter};
  std::bitset<nTasks> seen{};

  for (auto& local : futures) {
    for (auto& f : local) {
      const int id{f.get()};
      EXPECT_GE(id, 0);
      EXPECT_LT(id, nTasks);
      EXPECT_FALSE(seen.test(id));
      seen.set(id);
    }
  }

  EXPECT_TRUE(seen.all());
}

// Make sure tasks run concurrently on different worker threads
TEST(ThreadPoolTests, RunsTasksConcurrently) {
  constexpr int nTasks{100'000};
  constexpr ThreadWeave::Index nThreads{8};
  std::atomic<int> uniqueThreadCount{0};

  {
    ThreadPool pool{nThreads};

    for (int i{0}; i < nTasks; ++i) {
      pool.submit([&uniqueThreadCount] {
        thread_local bool seen{false};

        if (!seen) {
          seen = true;
          uniqueThreadCount.fetch_add(1, MemoryOrder::relaxed);
        }
      });
    }
  }

  // Not necessarily guaranteed but should be the case with a sufficient number
  // of tasks
  EXPECT_EQ(uniqueThreadCount.load(MemoryOrder::relaxed), nThreads);
}

// Make sure exceptions from one task don't break other tasks
TEST(ThreadPoolTests, ContinuesAfterException) {
  ThreadPool pool{2};

  auto bad{pool.submit([] {
    throw std::runtime_error{"failure"};
    return 0;
  })};

  auto good{pool.submit([] { return 42; })};
  EXPECT_THROW(bad.get(), std::runtime_error);
  EXPECT_EQ(good.get(), 42);
}

// Make sure submitting many tasks doesn't lose any
TEST(ThreadPoolTests, HandlesManyTasks) {
  constexpr int nTasks{100'000};
  std::atomic<int> completed{0};

  {
    ThreadPool pool{};
    for (int i{0}; i < nTasks; ++i) {
      pool.submit(
          [&completed] { completed.fetch_add(1, MemoryOrder::relaxed); });
    }
  }

  EXPECT_EQ(completed.load(MemoryOrder::relaxed), nTasks);
}

// Make sure pool can handle recursive submissions
TEST(ThreadPoolTests, HandlesNestedTaskSubmission) {
  ThreadPool pool{4};

  // Parallel version of naive fibonacci
  auto parallelFib{[&pool](this auto self, int n) {
    if (n <= 1) {
      return pool.submit([] { return 1; });
    }

    // Submit nested work from inside a worker thread
    return pool.submit([self, n] {
      auto lhs{self(n - 1)};
      auto rhs{self(n - 2)};
      return lhs.get() + rhs.get();
    });
  }};

  // This test makes sure there are no blocking calls to wait internally. By
  // having more recursive calls than available workers, we make sure workers
  // keep working instead of get() blocking
  auto res{parallelFib(12)};
  EXPECT_EQ(res.get(), 233);  // fib(12) = 233
}

// Ensure proper behavior upon rapid building and destroying of pools
TEST(ThreadPoolTests, HighFrequencyLifecycleChurn) {
  constexpr int nIterations{100};

  for (int i{0}; i < nIterations; ++i) {
    constexpr int nTasks{50};
    ThreadPool pool{8};
    std::vector<Future<int>> futures{};
    futures.reserve(nTasks);

    for (int j{0}; j < nTasks; ++j) {
      futures.push_back(pool.submit([j] { return j; }));
    }

    std::bitset<nTasks> seen{};

    for (auto& f : futures) {
      const int val{f.get()};
      EXPECT_GE(val, 0);
      EXPECT_LT(val, nTasks);
      EXPECT_FALSE(seen.test(val));
      seen.set(val);
    }

    EXPECT_TRUE(seen.all());
  }
}

// Verify medianOfThree picks the true median regardless of input order
TEST(ThreadPoolTests, MedianOfThreeAllOrderings) {
  std::array perm{1, 2, 3};

  do {
    const auto begin{perm.begin()};
    const auto median{
        Internal::medianOfThree(begin, begin + 1, begin + 2, std::less<int>{})};
    EXPECT_EQ(*median, 2);
  } while (std::ranges::next_permutation(perm).found);
}

// Verify pickPivot on ascending data
TEST(ThreadPoolTests, PickPivotAscendingData) {
  constexpr Index n{800};
  std::vector<int> nums(n);
  std::ranges::iota(nums, 0);
  const auto piv{
      Internal::pickPivot(nums.begin(), nums.end(), std::less<int>{})};
  EXPECT_EQ(*piv, 400);
}

// Verify pickPivot on descending data
TEST(ThreadPoolTests, PickPivotDescendingData) {
  constexpr Index n{800};
  std::vector<int> nums(n);
  std::iota(nums.begin(), nums.end(), 0);
  std::ranges::reverse(nums);
  const auto piv{
      Internal::pickPivot(nums.begin(), nums.end(), std::less<int>{})};
  EXPECT_EQ(*piv, 399);
}

// Verify Hoare partitioning maintains its invariant on random data
TEST(ThreadPoolTests, HoarePartitionInvariant) {
  static_assert(Internal::IsCheaplyCopyableV<int>);
  constexpr Index nIterations{50};
  constexpr Index n{200};
  std::mt19937 rng{7};

  for (int iter{0}; iter < nIterations; ++iter) {
    std::vector<int> nums(n);
    std::ranges::iota(nums, 0);
    std::ranges::shuffle(nums, rng);
    std::vector<int> original{nums};
    auto pivIt{nums.begin() + n / 2};
    const int pivotVal{*pivIt};
    auto mid{Internal::hoarePartition(nums.begin(), nums.end(), pivIt,
                                      std::less<int>{})};

    for (auto it{nums.begin()}; it != mid; ++it) {
      EXPECT_LE(*it, pivotVal);
    }

    for (auto it{mid}; it != nums.end(); ++it) {
      EXPECT_GE(*it, pivotVal);
    }

    std::vector<int> after{nums};
    std::ranges::sort(original);
    std::ranges::sort(after);
    EXPECT_EQ(original, after);
  }
}

// Verify Lomuto partitioning maintains its invariant
TEST(ThreadPoolTests, LomutoPartitionInvariant) {
  static_assert(!Internal::IsCheaplyCopyableV<std::string>);
  constexpr Index nIterations{50};
  constexpr Index n{200};
  std::mt19937 rng{11};

  for (int iter{0}; iter < nIterations; ++iter) {
    std::vector<std::string> words(n);

    for (int i{0}; i < n; ++i) {
      words[i] = std::to_string(i);
    }

    std::ranges::shuffle(words, rng);
    std::vector<std::string> original{words};
    auto pivIt{words.begin() + n / 2};
    const std::string pivotVal{*pivIt};
    auto mid{Internal::lomutoPartition(words.begin(), words.end(), pivIt,
                                       std::less<std::string>{})};
    EXPECT_EQ(*mid, pivotVal);

    for (auto it{words.begin()}; it != mid; ++it) {
      EXPECT_LT(*it, pivotVal);
    }

    for (auto it{std::next(mid)}; it != words.end(); ++it) {
      EXPECT_GE(*it, pivotVal);
    }

    std::vector<std::string> after{words};
    std::ranges::sort(original);
    std::ranges::sort(after);
    EXPECT_EQ(original, after);
  }
}

// Ensure divisibilty of blocked ranges for a given grain size
TEST(ThreadPoolTests, BlockedRangeIsDivisible) {
  constexpr Index n{100};
  std::vector<int> nums(n);
  const Internal::BlockedRange divisible{nums.begin(), nums.end(), n / 2};
  EXPECT_TRUE(divisible.isDivisible());
  const Internal::BlockedRange exactlyGrainSize{nums.begin(), nums.end(), n};
  EXPECT_FALSE(exactlyGrainSize.isDivisible());
}

// Verify split divides the range in half, LHS keeping the remainder and RHS
// getting the rounded-up half
TEST(ThreadPoolTests, BlockedRangeSplit) {
  constexpr Index n{101};
  constexpr Index grainSize{10};
  std::vector<int> nums(n);
  std::ranges::iota(nums, 0);
  Internal::BlockedRange rng{nums.begin(), nums.end(), grainSize};
  const auto rhs{rng.split()};
  EXPECT_EQ(std::distance(rng.begin(), rng.end()), n / 2);
  EXPECT_EQ(std::distance(rhs.begin(), rhs.end()), (n + 1) / 2);
  EXPECT_EQ(rng.end(), rhs.begin());
}

// Make sure sorting works on random data that is cheaply copyable
TEST(ThreadPoolTests, RandomSortAccuracyCheap) {
  constexpr Index nIterations{20};
  constexpr Index n{100'000};
  std::vector<int> nums(n);
  static_assert(Internal::IsCheaplyCopyableV<decltype(nums)::value_type>);
  std::ranges::iota(nums, 0);
  std::mt19937 rng{124};
  ThreadPool pool{4};

  for (int i{0}; i < nIterations; ++i) {
    std::ranges::shuffle(nums, rng);
    EXPECT_FALSE(std::ranges::is_sorted(nums));
    pool.sort(nums.begin(), nums.end());
    EXPECT_TRUE(std::ranges::is_sorted(nums));
    std::bitset<n> seen{};

    for (Index num{0}; num < n; ++num) {
      EXPECT_FALSE(seen.test(num));
      seen.set(num);
    }

    EXPECT_TRUE(seen.all());
  }
}

// Make sure sorting works on already sorted data that is cheaply copyable
TEST(ThreadPoolTests, AlreadySortedSortAccuracyCheap) {
  constexpr Index nIterations{20};
  constexpr Index n{100'000};
  std::vector<int> nums(n);
  static_assert(Internal::IsCheaplyCopyableV<decltype(nums)::value_type>);
  std::ranges::iota(nums, 0);
  ThreadPool pool{4};

  for (int i{0}; i < nIterations; ++i) {
    EXPECT_TRUE(std::ranges::is_sorted(nums));
    pool.sort(nums.begin(), nums.end());
    EXPECT_TRUE(std::ranges::is_sorted(nums));
    std::bitset<n> seen{};

    for (Index num{0}; num < n; ++num) {
      EXPECT_FALSE(seen.test(num));
      seen.set(num);
    }

    EXPECT_TRUE(seen.all());
  }
}

// Make sure sorting works on already reverse sorted data that is cheaply
// copyable
TEST(ThreadPoolTests, ReverseSortedSortAccuracy) {
  constexpr Index nIterations{20};
  constexpr Index n{100'000};
  std::vector<int> nums(n);
  static_assert(Internal::IsCheaplyCopyableV<decltype(nums)::value_type>);
  std::ranges::iota(nums, 0);
  std::ranges::reverse(nums);
  ThreadPool pool{4};

  for (int i{0}; i < nIterations; ++i) {
    EXPECT_TRUE(std::ranges::is_sorted(nums, std::greater<int>{}));
    pool.sort(nums.begin(), nums.end());
    EXPECT_TRUE(std::ranges::is_sorted(nums));
    std::bitset<n> seen{};

    for (Index num{0}; num < n; ++num) {
      EXPECT_FALSE(seen.test(num));
      seen.set(num);
    }

    EXPECT_TRUE(seen.all());
    std::ranges::reverse(nums);
  }
}

// Verify sorting works on random data wrapped in std::reference_wrapper (for
// trivially copyable types)
TEST(ThreadPoolTests, RandomSortAccuracyRefWrapperCheap) {
  constexpr Index nIterations{20};
  constexpr Index n{100'000};
  std::vector<int> nums(n);
  std::ranges::iota(nums, 0);
  std::vector<std::reference_wrapper<int>> refNums{nums.begin(), nums.end()};
  static_assert(Internal::IsCheaplyCopyableV<decltype(refNums)::value_type>);
  std::mt19937 rng{124};
  ThreadPool pool{4};

  for (int i{0}; i < nIterations; ++i) {
    std::ranges::shuffle(refNums, rng);
    EXPECT_FALSE(std::ranges::is_sorted(refNums));
    pool.sort(refNums.begin(), refNums.end());
    EXPECT_TRUE(std::ranges::is_sorted(refNums));
    std::bitset<n> seen{};

    for (const auto& ref : refNums) {
      const Index num{ref.get()};
      EXPECT_FALSE(seen.test(num));
      seen.set(num);
    }

    EXPECT_TRUE(seen.all());
  }
}

// Verify sorting works on random data that is not cheaply copyable
TEST(ThreadPoolTests, RandomSortAccuracyNonCheap) {
  constexpr Index nIterations{20};
  constexpr Index n{100'000};
  std::vector<std::string> strs(n);

  for (Index i{0}; i < n; ++i) {
    strs[i] = std::format("{:06d}", i);
  }

  static_assert(!Internal::IsCheaplyCopyableV<decltype(strs)::value_type>);
  std::mt19937 rng{124};
  ThreadPool pool{4};

  for (int i{0}; i < nIterations; ++i) {
    std::ranges::shuffle(strs, rng);
    EXPECT_FALSE(std::ranges::is_sorted(strs));
    pool.sort(strs.begin(), strs.end());
    EXPECT_TRUE(std::ranges::is_sorted(strs));
    std::bitset<n> seen{};

    for (const auto& s : strs) {
      const Index num{std::stoi(s)};
      EXPECT_FALSE(seen.test(num));
      seen.set(num);
    }

    EXPECT_TRUE(seen.all());
  }
}

// Verify sorting works on already sorted data that is not cheaply copyable
TEST(ThreadPoolTests, AlreadySortedSortAccuracyNonCheap) {
  constexpr Index nIterations{20};
  constexpr Index n{100'000};
  std::vector<std::string> strs(n);

  for (Index i{0}; i < n; ++i) {
    strs[i] = std::format("{:06d}", i);
  }

  static_assert(!Internal::IsCheaplyCopyableV<decltype(strs)::value_type>);
  ThreadPool pool{4};

  for (int i{0}; i < nIterations; ++i) {
    EXPECT_TRUE(std::ranges::is_sorted(strs));
    pool.sort(strs.begin(), strs.end());
    EXPECT_TRUE(std::ranges::is_sorted(strs));
    std::bitset<n> seen{};

    for (const auto& s : strs) {
      const Index num{std::stoi(s)};
      EXPECT_FALSE(seen.test(num));
      seen.set(num);
    }

    EXPECT_TRUE(seen.all());
  }
}

// Verify sorting works on already reverse sorted data that is not cheaply
// copyable
TEST(ThreadPoolTests, ReverseSortedSortAccuracyNonCheap) {
  constexpr Index nIterations{20};
  constexpr Index n{100'000};
  std::vector<std::string> strs(n);

  for (Index i{0}; i < n; ++i) {
    strs[i] = std::format("{:06d}", i);
  }

  std::ranges::reverse(strs);
  static_assert(!Internal::IsCheaplyCopyableV<decltype(strs)::value_type>);
  ThreadPool pool{4};

  for (int i{0}; i < nIterations; ++i) {
    EXPECT_TRUE(std::ranges::is_sorted(strs, std::greater<>{}));
    pool.sort(strs.begin(), strs.end());
    EXPECT_TRUE(std::ranges::is_sorted(strs));
    std::bitset<n> seen{};

    for (const auto& s : strs) {
      const Index num{std::stoi(s)};
      EXPECT_FALSE(seen.test(num));
      seen.set(num);
    }

    EXPECT_TRUE(seen.all());
    std::ranges::reverse(strs);
  }
}

// Verify sorting works on random data wrapped in std::reference_wrapper
// (cheaply copyable wrapper around non-cheap type)
TEST(ThreadPoolTests, RandomSortAccuracyRefWrapperNonCheap) {
  constexpr Index nIterations{20};
  constexpr Index n{100'000};
  std::vector<std::string> strs(n);

  for (Index i{0}; i < n; ++i) {
    strs[i] = std::format("{:06d}", i);
  }

  using StrRef = std::reference_wrapper<std::string>;
  std::vector<StrRef> refStrs{strs.begin(), strs.end()};

  // The wrapper itself is cheaply copyable
  static_assert(Internal::IsCheaplyCopyableV<decltype(refStrs)::value_type>);
  std::mt19937 rng{124};
  ThreadPool pool{4};
  const auto comp{
      [](const StrRef a, const StrRef b) { return a.get() < b.get(); }};

  for (int i{0}; i < nIterations; ++i) {
    std::ranges::shuffle(refStrs, rng);
    EXPECT_FALSE(std::ranges::is_sorted(refStrs, comp));
    pool.sort(refStrs.begin(), refStrs.end(), comp);
    EXPECT_TRUE(std::ranges::is_sorted(refStrs, comp));
    std::bitset<n> seen{};

    for (const auto& ref : refStrs) {
      const Index num{std::stoi(ref.get())};
      EXPECT_FALSE(seen.test(num));
      seen.set(num);
    }

    EXPECT_TRUE(seen.all());
  }
}

// Verify forEach visits every element exactly once across various grain sizes
TEST(ThreadPoolTests, ForEachVisitsEveryElementOnce) {
  constexpr Index n{100'000};
  constexpr Index grainSizes[]{1, 7, 500, 10'000, n * 2};
  std::vector<int> nums(n);
  std::ranges::iota(nums, 0);
  ThreadPool pool{4};

  for (const Index grainSize : grainSizes) {
    std::vector<std::atomic<int>> seen(n);
    pool.forEach(
        nums.begin(), nums.end(),
        [&](const int v) { seen[v].fetch_add(1, MemoryOrder::relaxed); },
        grainSize);

    for (Index i{0}; i < n; ++i) {
      EXPECT_EQ(seen[i].load(MemoryOrder::relaxed), 1);
    }
  }
}

// Make sure forEach's function receives correct element references
TEST(ThreadPoolTests, ForEachMutatesElementsInPlace) {
  constexpr Index n{50'000};
  std::vector<int> nums(n);
  std::ranges::iota(nums, 0);
  ThreadPool pool{4};
  pool.forEach(nums.begin(), nums.end(), [](int& v) { v *= 2; }, 500);

  for (Index i{0}; i < n; ++i) {
    EXPECT_EQ(nums[i], static_cast<int>(i) * 2);
  }
}

// Verify forEach works on non-cheaply copyable elements
TEST(ThreadPoolTests, ForEachWorksOnNonCheaplyCopyableElements) {
  static_assert(!Internal::IsCheaplyCopyableV<std::string>);
  constexpr Index n{20'000};
  std::vector<std::string> words(n);

  for (Index i{0}; i < n; ++i) {
    words[i] = std::to_string(i);
  }

  ThreadPool pool{4};
  pool.forEach(
      words.begin(), words.end(), [](std::string& s) { s += "_seen"; }, 500);

  for (Index i{0}; i < n; ++i) {
    EXPECT_EQ(words[i], std::to_string(i) + "_seen");
  }
}

// Ensure reduce's result matches a sequential accumulate across various grain
// sizes
TEST(ThreadPoolTests, ReduceSumMatchesSequentialAccumulate) {
  constexpr Index n{50'000};
  constexpr Index grainSizes[]{1, 7, 500, 10'000, n};
  std::vector<int> nums(n);
  std::ranges::iota(nums, 1);
  ThreadPool pool{4};
  const int expected{std::accumulate(nums.begin(), nums.end(), 0)};

  for (const Index grainSize : grainSizes) {
    const int res{pool.reduce(
        nums.begin(), nums.end(), 0,
        [](const int x, const int y) { return x + y; }, grainSize)};
    EXPECT_EQ(res, expected);
  }
}

// Verify reduce applies a non-identity init exactly once, regardless of how
// many times the range gets split
TEST(ThreadPoolTests, ReduceAppliesInitExactlyOnce) {
  constexpr Index n{50'000};
  constexpr Index grainSizes[]{1, 7, 500, 10'000, n};
  std::vector<int> nums(n);
  std::ranges::iota(nums, 1);
  ThreadPool pool{4};
  constexpr int init{100};
  const int expected{std::accumulate(nums.begin(), nums.end(), init)};

  for (Index grainSize : grainSizes) {
    const int res{pool.reduce(
        nums.begin(), nums.end(), init,
        [](const int x, const int y) { return x + y; }, grainSize)};
    EXPECT_EQ(res, expected) << "grainSize=" << grainSize;
  }
}

// Verify reduce on an empty range returns init untouched
TEST(ThreadPoolTests, ReduceOnEmptyRangeReturnsInit) {
  std::vector<int> nums{};
  ThreadPool pool{4};
  constexpr int init{42};
  const int resAdd{pool.reduce(nums.begin(), nums.end(), init,
                               [](const int x, const int y) { return x + y; })};
  const int resMul{pool.reduce(nums.begin(), nums.end(), init,
                               [](const int x, const int y) { return x * y; })};
  EXPECT_EQ(resAdd, init);
  EXPECT_EQ(resMul, init);
}

// Verify reduce product matches sequential product across various grain sizes
TEST(ThreadPoolTests, ReduceProductMatchesSequential) {
  constexpr Index n{1'000};
  constexpr Index grainSizes[]{1, 7, 50, 200, n};
  std::vector<double> nums(n);

  for (Index i{0}; i < n; ++i) {
    nums[i] = 1.0 + (1.0 / static_cast<double>(i + 1'000));
  }

  ThreadPool pool{4};
  const double expected{
      std::accumulate(nums.begin(), nums.end(), 1.0,
                      [](const double x, const double y) { return x * y; })};

  for (const Index grainSize : grainSizes) {
    constexpr double epsilon{1e-9};
    const double res{pool.reduce(
        nums.begin(), nums.end(), 1.0,
        [](const double x, const double y) { return x * y; }, grainSize)};
    EXPECT_NEAR(res, expected, epsilon) << "grainSize=" << grainSize;
  }
}

// Verify reduce product applies non-identity init value exactly once
TEST(ThreadPoolTests, ReduceProductAppliesInitExactlyOnce) {
  constexpr Index n{500};
  constexpr Index grainSizes[]{1, 13, 100, n};
  std::vector<double> nums(n);

  for (Index i{0}; i < n; ++i) {
    nums[i] = 1.001;
  }

  ThreadPool pool{4};
  constexpr double init{2.5};
  const double expected{
      std::accumulate(nums.begin(), nums.end(), init,
                      [](const double x, const double y) { return x * y; })};

  for (const Index grainSize : grainSizes) {
    constexpr double epsilon{1e-9};
    const double res{pool.reduce(
        nums.begin(), nums.end(), init,
        [](const double x, const double y) { return x * y; }, grainSize)};
    EXPECT_NEAR(res, expected, epsilon) << "grainSize=" << grainSize;
  }
}
