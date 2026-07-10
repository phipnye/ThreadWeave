#include <gtest/gtest.h>
#include <threadweave/ChaseLevDeque.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <optional>
#include <print>
#include <random>
#include <thread>
#include <vector>

#include "PaddedAtomicInt.h"

template <typename T>
using Deque = ThreadWeave::ChaseLevDeque<T>;

// Make sure an empty deque returns std::nullopt on pop and steal
TEST(DequeTest, EmptyDequeReturnsNullopt) {
  Deque<int> dq{};
  EXPECT_TRUE(dq.empty());
  const auto popVal{dq.pop()};
  EXPECT_FALSE(popVal.has_value());
  EXPECT_TRUE(dq.empty());
  const auto stealVal{dq.steal()};
  EXPECT_FALSE(stealVal.has_value());
  EXPECT_TRUE(dq.empty());
}

// Trivial test for a single push and pop along with a single push and steal
TEST(DequeTest, SinglePushPopSteal) {
  Deque<int> dq{};
  EXPECT_TRUE(dq.empty());
  constexpr int val1{42};
  dq.push(val1);
  EXPECT_FALSE(dq.empty());
  const auto popVal{dq.pop()};
  EXPECT_TRUE(popVal.has_value());
  EXPECT_EQ(val1, *popVal);
  EXPECT_TRUE(dq.empty());
  constexpr int val2{26};
  dq.push(val2);
  EXPECT_FALSE(dq.empty());
  const auto stealVal{dq.steal()};
  EXPECT_TRUE(stealVal.has_value());
  EXPECT_EQ(val2, *stealVal);
  EXPECT_TRUE(dq.empty());
}

// Basic sanity check for LIFO behavior of pops
TEST(DequeTest, OwnerPopIsLifo) {
  Deque<int> dq{};
  constexpr int n{100};

  for (int i{0}; i < n; ++i) {
    dq.push(i);
  }

  for (int i{n - 1}; i > -1; --i) {
    const auto val{dq.pop()};
    EXPECT_TRUE(val.has_value());
    EXPECT_EQ(*val, i);
  }

  EXPECT_FALSE(dq.pop().has_value());
}

// Basic sanity check for FIFO behavior of steals
TEST(DequeTest, ThiefStealIsFifo) {
  Deque<int> dq{};
  constexpr int n{100};

  for (int i{0}; i < n; ++i) {
    dq.push(i);
  }

  for (int i{0}; i < n; ++i) {
    const auto val{dq.steal()};
    EXPECT_TRUE(val.has_value());
    EXPECT_EQ(*val, i);
  }
}

// Test to make sure the internal wrap around behavior of the ring buffer (uses
// modulo/power 2 bitmasking) behaves as expected
TEST(DequeTest, WrapAroundBoundary) {
  Deque<int> dq{};
  constexpr int n{100'000};

  for (int i{0}; i < n; ++i) {
    dq.push(i);
    dq.push(i + 1);
    const auto popVal{dq.pop()};
    const auto stealVal{dq.steal()};
    EXPECT_TRUE(popVal.has_value());
    EXPECT_TRUE(stealVal.has_value());
    EXPECT_EQ(i + 1, popVal);
    EXPECT_EQ(i, stealVal);
  }

  EXPECT_TRUE(dq.empty());
}

// Force expansion of the internal ring buffer while thieves may be looking at
// old buffer to make sure there's no invalid pointer use
TEST(DequeTest, StealDuringExpandStress) {
  Deque<int> dq{};
  constexpr int nThieves{2};
  constexpr int nItems{500'000};
  std::atomic<bool> stop{false};
  std::vector<std::jthread> thieves{};

  for (int i{0}; i < nThieves; ++i) {
    thieves.emplace_back([&] {
      while (!stop.load(std::memory_order::acquire)) {
        dq.steal();
      }
    });
  }

  for (int i{0}; i < nItems; ++i) {
    dq.push(i);
  }

  while (dq.pop()) {}
  stop.store(true, std::memory_order::release);
  thieves.clear();

#ifndef NDEBUG
  const auto nExpands{dq.debugExpandCnt.load(std::memory_order::relaxed)};
  std::println("# of expansions = {}", nExpands);
  ASSERT_GT(nExpands, 0) << "No expansion occurred — thieves may be outpacing "
                            "push; reduce nThieves";
#else
  SUCCEED() << "Test cannot verify expansions occurred in release mode";
#endif
}

// Push only a few items (equivalent of the initial capacity) and make sure no
// expansions occur
TEST(DequeTest, NoUnnecessaryExpansions) {
  Deque<int> dq{};
  constexpr int nThieves{2};
  constexpr int nItems{16};  // initial capacity
  std::atomic<bool> stop{false};
  std::vector<std::jthread> thieves;

  for (int i{0}; i < nThieves; ++i) {
    thieves.emplace_back([&] {
      while (!stop.load(std::memory_order::acquire)) {
        dq.steal();
      }
    });
  }

  for (int i{0}; i < nItems; ++i) {
    dq.push(i);
  }

  while (dq.pop()) {}
  stop.store(true, std::memory_order::release);
  thieves.clear();

#ifndef NDEBUG
  const auto nExpands{dq.debugExpandCnt.load(std::memory_order::relaxed)};
  std::println("# of expansions = {}", nExpands);
  EXPECT_EQ(nExpands, 0)
      << "An expansion occurred when pushing an insufficient number of items";
#else
  SUCCEED() << "Test cannot verify expansions did not occur in release mode";
#endif
}

// Randomized race condition test checking for lost or duplicate items
TEST(DequeTest, RandomizedOperationsTest) {
  for (constexpr int testSet[]{10, 124, 3525, 43861};
       const int nTasks : testSet) {
    Deque<int> dq{};
    constexpr int nThieves{10};
    std::vector<PaddedAtomicInt> actualCounts(nTasks);
    std::vector<std::jthread> thieves{};
    thieves.reserve(nThieves);
    std::atomic<bool> ownerDone{false};
    std::atomic<int> activeWorkers{nThieves + 1};  // +1 for owner

    for (int i{0}; i < nThieves; ++i) {
      thieves.emplace_back([&] {
        std::mt19937 rng{std::random_device{}()};
        std::uniform_int_distribution sleepDist{1, 10};

        // Continually try stealing tasks
        while (!ownerDone.load(std::memory_order::acquire)) {
          if (const auto val{dq.steal()}) {
            actualCounts[*val].fetch_add(1, std::memory_order::relaxed);
          } else {
            std::this_thread::sleep_for(
                std::chrono::milliseconds{sleepDist(rng)});
          }
        }

        // Empty the rest of the deque
        while (!dq.empty()) {
          if (const auto val{dq.steal()}) {
            actualCounts[*val].fetch_add(1, std::memory_order::relaxed);
          }
        }

        // Signal this worker is done
        activeWorkers.fetch_sub(1, std::memory_order::relaxed);
      });
    }

    // Owner thread
    {
      // 0 = popping and 1,2 = pushing (bias toward pushing)
      std::mt19937 rng{std::random_device{}()};
      std::uniform_int_distribution actionDist{0, 2};

      // Keep pushing and popping items until we've satisfied the desired number
      // of tasks
      for (int task{0}; task < nTasks;) {
        if (actionDist(rng) > 0) {
          dq.push(task++);
          continue;
        }

        if (const auto val{dq.pop()}) {
          actualCounts[*val].fetch_add(1, std::memory_order::relaxed);
        }
      }

      // Empty the rest of the deque
      while (!dq.empty()) {
        if (const auto val{dq.pop()}) {
          actualCounts[*val].fetch_add(1, std::memory_order::relaxed);
        }
      }

      // Signal the owner is done
      ownerDone.store(true, std::memory_order::release);
      activeWorkers.fetch_sub(1, std::memory_order::relaxed);
    }

    // Wait for the thieves to finish running
    thieves.clear();

    // No more workers expected at this point
    EXPECT_EQ(activeWorkers.load(std::memory_order::relaxed), 0);

    // Make sure counts are as expected
    int total{0};

    for (int i{0}; i < nTasks; ++i) {
      const int cnt{actualCounts[i].load(std::memory_order::relaxed)};
      total += cnt;
      EXPECT_EQ(cnt, 1) << "Task ID " << i << " was processed " << cnt
                        << " times";
    }

    EXPECT_EQ(total, nTasks);
  }
}

TEST(DequeTest, SingleItemRace) {
  // Note that this test is slower due to the overhead of frequently creating
  // and destroying threads
  constexpr int nIterations{10'000};
  constexpr int nThieves{4};

  // Repeatedly reuse the same resources
  Deque<int> dq{};
  std::vector<std::jthread> thieves{};
  thieves.reserve(nThieves);

  for (int i{0}; i < nIterations; ++i) {
    EXPECT_TRUE(dq.empty());
    dq.push(i);
    std::atomic<int> winners{0};

    for (int t{0}; t < nThieves; ++t) {
      thieves.emplace_back([&] {
        if (dq.steal()) {
          winners.fetch_add(1, std::memory_order::relaxed);
        }
      });
    }

    if (dq.pop()) {
      winners.fetch_add(1, std::memory_order::relaxed);
    }

    thieves.clear();
    EXPECT_EQ(winners.load(std::memory_order::relaxed), 1);
    EXPECT_TRUE(dq.empty());
  }
}
