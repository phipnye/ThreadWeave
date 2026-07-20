#include <gtest/gtest.h>
#include <threadweave/Stack.h>
#include <threadweave/utils.h>

#include <atomic>
#include <bitset>
#include <memory>
#include <optional>
#include <random>
#include <thread>
#include <vector>

template <typename T>
using Stack = ThreadWeave::Stack<T>;

namespace MemoryOrder = ThreadWeave::MemoryOrder;

// Make sure an empty stack returns std::nullopt
TEST(StackTest, EmptyPopReturnsNullopt) {
  Stack<int> stk{};
  EXPECT_TRUE(stk.empty());
  const auto val{stk.pop()};
  EXPECT_FALSE(val.has_value());
  EXPECT_TRUE(stk.empty());
}

// Trivial single push/pop
TEST(StackTest, SinglePushPop) {
  Stack<int> stk{};
  EXPECT_TRUE(stk.empty());
  stk.push(42);
  EXPECT_FALSE(stk.empty());
  const auto val{stk.pop()};
  ASSERT_TRUE(val.has_value());
  EXPECT_EQ(*val, 42);
  EXPECT_TRUE(stk.empty());
  EXPECT_FALSE(stk.pop().has_value());
}

// Make sure the stack follows LIFO when executed sequentially
TEST(StackTest, StackIsLIFO) {
  constexpr int n{100};
  Stack<int> stk{};

  // Push 0-99 into stack
  for (int i{0}; i < n; ++i) {
    stk.push(i);
  }

  // Pop 99-0 from the stack to ensure data integrity and LIFO behavior
  for (int i{n - 1}; i > -1; --i) {
    EXPECT_FALSE(stk.empty());
    const auto val{stk.pop()};
    EXPECT_TRUE(val.has_value());
    EXPECT_EQ(*val, i);
  }

  // Make sure the stack is now empty
  EXPECT_TRUE(stk.empty());
}

// Test LIFO against a move only type
TEST(StackTest, MoveOnlyType) {
  // Slight modification to previous test except now with a move-only unique ptr
  constexpr int n{100};
  Stack<std::unique_ptr<int>> stk{};

  // Push unique pointers maintaining values 0-99 into the stack
  for (int i{0}; i < n; ++i) {
    stk.push(std::make_unique<int>(i));
  }

  // Pop 99-0 from the stack to ensure data integrity and LIFO behavior
  for (int i{n - 1}; i > -1; --i) {
    EXPECT_FALSE(stk.empty());
    const auto val{stk.pop()};
    EXPECT_TRUE(val.has_value());
    EXPECT_NE(*val, nullptr);
    EXPECT_EQ(**val, i);
  }

  // Make sure the stack is now empty
  EXPECT_TRUE(stk.empty());
}

// Test concurrent pushes and make sure no data is lost
TEST(StackTest, ConcurrentPushes) {
  Stack<int> stk{};
  constexpr int nThreads{8};
  constexpr int itemsPerThread{1000};
  std::vector<std::jthread> threads{};
  threads.reserve(nThreads);

  // Have multiple threads concurrently push items onto the stack
  for (int t{0}; t < nThreads; ++t) {
    threads.emplace_back([=, &stk] {
      for (int i{0}; i < itemsPerThread; ++i) {
        stk.push(t * itemsPerThread + i);
      }
    });
  }

  threads.clear();
  EXPECT_FALSE(stk.empty());
  constexpr int maxVal{nThreads * itemsPerThread};
  std::bitset<maxVal> seen{};

  // Make sure we retrieve every item that was pushed
  while (const auto val{stk.pop()}) {
    const auto idx{*val};
    EXPECT_GE(idx, 0);
    EXPECT_LT(idx, maxVal);
    EXPECT_FALSE(seen.test(idx));
    seen.set(idx);
  }

  EXPECT_TRUE(seen.all());
  EXPECT_TRUE(stk.empty());
}

// Test concurrent pops and make sure no data is lost
TEST(StackTest, ConcurrentPops) {
  Stack<int> stk{};
  constexpr int totalItems{10'000};
  constexpr int nThreads{8};

  // Push 0-9999 onto stack
  for (int i{0}; i < totalItems; ++i) {
    stk.push(i);
  }

  std::vector<std::jthread> threads{};
  std::vector<std::vector<int>> threadResults(nThreads);
  threads.reserve(nThreads);

  // Have multiple threads try to pop from the stack and push it to their own
  // set of results
  for (int t{0}; t < nThreads; ++t) {
    threads.emplace_back([=, &threadResults, &stk] {
      while (const auto val{stk.pop()}) {
        threadResults[t].push_back(*val);
      }
    });
  }

  threads.clear();
  EXPECT_TRUE(stk.empty());
  std::bitset<totalItems> seen{};

  // Make sure we got back every value
  for (const auto& tRes : threadResults) {
    for (const int item : tRes) {
      EXPECT_GE(item, 0);
      EXPECT_LT(item, totalItems);
      EXPECT_FALSE(seen.test(item));
      seen.set(item);
    }
  }

  EXPECT_TRUE(seen.all());
}

// Ensure data integrity with both concurrent pushes and pops
TEST(StackTest, ConcurrentProducerConsumer) {
  Stack<int> stk{};
  constexpr int nPairs{4};
  constexpr int opsPerThread{5000};

  // Have multiple producers and consumers concurrently push to and pop from the
  // stack
  std::vector<std::jthread> producers{};
  std::vector<std::jthread> consumers{};
  producers.reserve(nPairs);
  consumers.reserve(nPairs);
  std::atomic<int> activeProducers{nPairs};
  std::vector<std::vector<int>> consumedData(nPairs);

  for (int i{0}; i < nPairs; ++i) {
    producers.emplace_back([=, &stk, &activeProducers] {
      for (int j{0}; j < opsPerThread; ++j) {
        stk.push(i * opsPerThread + j);
      }

      activeProducers.fetch_sub(1, MemoryOrder::release);
    });
  }

  for (int i{0}; i < nPairs; ++i) {
    consumers.emplace_back([=, &stk, &activeProducers, &consumedData] {
      while (true) {
        if (const auto val{stk.pop()}) {
          consumedData[i].push_back(*val);
        } else {
          if (activeProducers.load(MemoryOrder::acquire) == 0 && stk.empty()) {
            break;
          }

          std::this_thread::yield();
        }
      }
    });
  }

  producers.clear();
  consumers.clear();
  EXPECT_TRUE(stk.empty());

  // Make sure we pushed and popped every expected item
  constexpr int totalItems{nPairs * opsPerThread};
  std::bitset<totalItems> seen{};

  for (const auto& data : consumedData) {
    for (const int item : data) {
      EXPECT_GE(item, 0);
      EXPECT_LT(item, totalItems);
      EXPECT_FALSE(seen.test(item));
      seen.set(item);
    }
  }

  EXPECT_TRUE(seen.all());
}

TEST(StackTest, RandomizedThreadOperationsTest) {
  constexpr int nPushes{100'000};
  constexpr int nThreads{8};
  Stack<int> stk{};
  std::atomic<int> runCnt{0};
  std::vector<std::jthread> threads{};
  threads.reserve(nThreads);
  std::vector<std::vector<int>> consumedData(nThreads);

  // Try having the threads randomly push and pop data from the stack
  for (int i{0}; i < nThreads; ++i) {
    threads.emplace_back([=, &stk, &runCnt, &consumedData] {
      // Random selection between a push and pop operation
      std::mt19937 rng{std::random_device{}()};
      std::uniform_int_distribution dist{0, 1};

      while (true) {
        // Push if 0 - Pop if 1
        if (!dist(rng)) {
          // Try pushing
          const int val{runCnt.fetch_add(1, MemoryOrder::relaxed)};

          if (val >= nPushes) {
            break;
          }

          stk.push(val);
        } else {
          // Try popping
          if (const auto val{stk.pop()}) {
            consumedData[i].push_back(*val);
          }
        }
      }

      // Continue popping until stack is empty
      while (const auto val{stk.pop()}) {
        consumedData[i].push_back(*val);
      }
    });
  }

  threads.clear();
  EXPECT_TRUE(stk.empty());
  std::bitset<nPushes> seen{};

  for (const auto& data : consumedData) {
    for (const int item : data) {
      EXPECT_GE(item, 0);
      EXPECT_LT(item, nPushes);
      EXPECT_FALSE(seen.test(item));
      seen.set(item);
    }
  }

  EXPECT_TRUE(seen.all());
}
