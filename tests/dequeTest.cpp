#include <gtest/gtest.h>
#include <threadweave/ChaseLevDeque.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <limits>
#include <print>
#include <random>
#include <thread>

#include "PaddedAtomicInt.h"

// Randomized race condition test checking for lost or duplicate items
TEST(DequeTest, RandomizedOperationsTest) {
  ThreadWeave::ChaseLevDeque<int> dq{};

  for (constexpr int testSet[]{10, 124, 3525, 43861};
       const int nTasks : testSet) {
    constexpr int nThieves{10};
    std::vector<PaddedAtomicInt> actualCounts(nTasks + 1);
    std::vector<std::jthread> thieves{};
    thieves.reserve(nThieves);
    std::atomic<bool> ownerDone{false};
    std::atomic<int> activeWorkers{nThieves + 1};  // +1 for owner

    for (int i{0}; i < nThieves; ++i) {
      thieves.emplace_back([&] {
        std::mt19937 rng{std::random_device{}()};
        std::uniform_int_distribution sleepDist{1, 10};

        // Continually try stealing tasks
        while (!ownerDone.load(std::memory_order::relaxed)) {
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
      ownerDone.store(true, std::memory_order::relaxed);
      activeWorkers.fetch_sub(1, std::memory_order::relaxed);
    }

    // Wait for the thieves to finish running
    thieves.clear();

    // No more workers expected at this point
    const int remWorkers{activeWorkers.load(std::memory_order::relaxed)};
    EXPECT_EQ(remWorkers, 0);

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

// Basic sanity check for LIFO behavior of pops
TEST(DequeTest, OwnerPopIsLifo) {
  ThreadWeave::ChaseLevDeque<int> dq{};
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
  ThreadWeave::ChaseLevDeque<int> dq{};
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
  ThreadWeave::ChaseLevDeque<int> dq{};
  constexpr int n{100'000};

  for (int i{0}; i < n; ++i) {
    dq.push(i);
    dq.push(i + 1);
    const auto popVal{dq.pop()};
    const auto stealVal{dq.steal()};
    EXPECT_TRUE(popVal.has_value());
    EXPECT_TRUE(stealVal.has_value());
    EXPECT_EQ(popVal, i + 1);
    EXPECT_EQ(stealVal, i);
  }

  EXPECT_TRUE(dq.empty());
}

// Force expansion of the internal ring buffer while thieves may be looking at
// old buffer to make sure there's no invalid pointer use
TEST(DequeTest, StealDuringExpandStress) {
  ThreadWeave::ChaseLevDeque<int> dq{};
  constexpr int nThieves{2};
  constexpr int nItems{500'000};
  std::atomic<bool> stop{false};
  std::vector<std::jthread> thieves{};

  for (int i{0}; i < nThieves; ++i) {
    thieves.emplace_back([&] {
      while (!stop.load(std::memory_order_relaxed)) {
        dq.steal();
      }
    });
  }

  for (int i{0}; i < nItems; ++i) {
    dq.push(i);
  }

  while (dq.pop()) {}
  stop.store(true, std::memory_order_relaxed);
  thieves.clear();

#ifndef NDEBUG
  const auto nExpands{dq.debugExpandCnt.load(std::memory_order::relaxed)};
  std::println("# of expansions = {}", nExpands);
  ASSERT_GT(nExpands, 0) << "No expansion occurred — thieves may be outpacing "
                            "push; reduce nThieves";
#else
  SUCCEED();
#endif
}

// Push only a few items (equivalent of the initial capacity) and make sure no
// expansions occur
TEST(DequeTest, NoUnnecessaryExpansions) {
  ThreadWeave::ChaseLevDeque<int> dq{};
  constexpr int nThieves{2};
  constexpr int nItems{16};  // initial capacity
  std::atomic<bool> stop{false};
  std::vector<std::jthread> thieves;

  for (int i{0}; i < nThieves; ++i) {
    thieves.emplace_back([&] {
      while (!stop.load(std::memory_order_relaxed)) {
        dq.steal();
      }
    });
  }

  for (int i{0}; i < nItems; ++i) {
    dq.push(i);
  }

  while (dq.pop()) {}
  stop.store(true, std::memory_order_relaxed);
  thieves.clear();

#ifndef NDEBUG
  const auto nExpands{dq.debugExpandCnt.load(std::memory_order::relaxed)};
  std::println("# of expansions = {}", nExpands);
  EXPECT_EQ(nExpands, 0)
      << "An expansion occurred when pushing an insufficient number of items";
#else
  SUCCEED();
#endif
}
