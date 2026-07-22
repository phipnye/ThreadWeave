#include <gtest/gtest.h>
#include <threadweave/MichaelScottQueue.h>
#include <threadweave/utils.h>

#include <atomic>
#include <bitset>
#include <memory>
#include <optional>
#include <random>
#include <thread>
#include <vector>

template <typename T>
using Queue = ThreadWeave::MichaelScottQueue<T>;

namespace MemoryOrder = ThreadWeave::MemoryOrder;

// Make sure an empty queue returns std::nullopt
TEST(MichaelScottQueueTests, EmptyPopReturnsNullopt) {
  Queue<int> q{};
  EXPECT_TRUE(q.empty());
  const auto val{q.pop()};
  EXPECT_FALSE(val.has_value());
  EXPECT_TRUE(q.empty());
}

// Trivial single push/pop
TEST(MichaelScottQueueTests, SinglePushPop) {
  Queue<int> q{};
  EXPECT_TRUE(q.empty());
  q.push(42);
  EXPECT_FALSE(q.empty());
  const auto val{q.pop()};
  ASSERT_TRUE(val.has_value());
  EXPECT_EQ(*val, 42);
  EXPECT_TRUE(q.empty());
  EXPECT_FALSE(q.pop().has_value());
}

// Make sure the queue follows FIFO when executed sequentially
TEST(MichaelScottQueueTests, QueueIsFifo) {
  constexpr int n{100};
  Queue<int> q{};

  // Push 0-99 into queue
  for (int i{0}; i < n; ++i) {
    q.push(i);
  }

  // Pop 0-99 from the queue to ensure data integrity and FIFO behavior
  for (int i{0}; i < n; ++i) {
    EXPECT_FALSE(q.empty());
    const auto val{q.pop()};
    ASSERT_TRUE(val.has_value());
    EXPECT_EQ(*val, i);
  }

  EXPECT_TRUE(q.empty());
}

// Test FIFO against a move only type
TEST(MichaelScottQueueTests, MoveOnlyType) {
  constexpr int n{100};
  Queue<std::unique_ptr<int>> q{};

  for (int i{0}; i < n; ++i) {
    q.push(std::make_unique<int>(i));
  }

  for (int i{0}; i < n; ++i) {
    EXPECT_FALSE(q.empty());
    const auto val{q.pop()};
    ASSERT_TRUE(val.has_value());
    ASSERT_NE(*val, nullptr);
    EXPECT_EQ(**val, i);
  }

  EXPECT_TRUE(q.empty());
}

// Test concurrent pushes and make sure no data is lost
TEST(MichaelScottQueueTests, ConcurrentPushes) {
  Queue<int> q{};
  constexpr int nThreads{8};
  constexpr int itemsPerThread{1000};
  std::vector<std::jthread> threads{};
  threads.reserve(nThreads);

  for (int t{0}; t < nThreads; ++t) {
    threads.emplace_back([=, &q] {
      for (int i{0}; i < itemsPerThread; ++i) {
        q.push(t * itemsPerThread + i);
      }
    });
  }

  threads.clear();
  EXPECT_FALSE(q.empty());
  constexpr int maxVal{nThreads * itemsPerThread};
  std::bitset<maxVal> seen{};

  while (const auto val{q.pop()}) {
    const auto idx{*val};
    EXPECT_GE(idx, 0);
    EXPECT_LT(idx, maxVal);
    EXPECT_FALSE(seen.test(idx));
    seen.set(idx);
  }

  EXPECT_TRUE(seen.all());
  EXPECT_TRUE(q.empty());
}

// Test concurrent pops and make sure no data is lost
TEST(MichaelScottQueueTests, ConcurrentPops) {
  Queue<int> q{};
  constexpr int totalItems{10'000};
  constexpr int nThreads{8};

  for (int i{0}; i < totalItems; ++i) {
    q.push(i);
  }

  std::vector<std::jthread> threads{};
  std::vector<std::vector<int>> threadResults(nThreads);
  threads.reserve(nThreads);

  for (int t{0}; t < nThreads; ++t) {
    threads.emplace_back([=, &threadResults, &q] {
      while (const auto val{q.pop()}) {
        threadResults[t].push_back(*val);
      }
    });
  }

  threads.clear();
  EXPECT_TRUE(q.empty());
  std::bitset<totalItems> seen{};

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
TEST(MichaelScottQueueTests, ConcurrentProducerConsumer) {
  Queue<int> q{};
  constexpr int nPairs{4};
  constexpr int opsPerThread{5000};

  std::vector<std::jthread> producers{};
  std::vector<std::jthread> consumers{};
  producers.reserve(nPairs);
  consumers.reserve(nPairs);
  std::atomic<int> activeProducers{nPairs};
  std::vector<std::vector<int>> consumedData(nPairs);

  for (int i{0}; i < nPairs; ++i) {
    producers.emplace_back([=, &q, &activeProducers] {
      for (int j{0}; j < opsPerThread; ++j) {
        q.push(i * opsPerThread + j);
      }

      activeProducers.fetch_sub(1, MemoryOrder::release);
    });
  }

  for (int i{0}; i < nPairs; ++i) {
    consumers.emplace_back([=, &q, &activeProducers, &consumedData] {
      while (true) {
        if (const auto val{q.pop()}) {
          consumedData[i].push_back(*val);
        } else {
          if (activeProducers.load(MemoryOrder::acquire) == 0 && q.empty()) {
            break;
          }

          std::this_thread::yield();
        }
      }
    });
  }

  producers.clear();
  consumers.clear();
  EXPECT_TRUE(q.empty());

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

// Randomized race condition test checking for lost or duplicate items
TEST(MichaelScottQueueTests, RandomizedThreadOperationsTest) {
  constexpr int nThreads{8};
  constexpr int nPushes{100'000};
  Queue<int> q{};
  std::atomic<int> runCnt{0};
  std::vector<std::jthread> threads{};
  threads.reserve(nThreads);
  std::vector<std::vector<int>> consumedData(nThreads);

  for (int i{0}; i < nThreads; ++i) {
    threads.emplace_back([=, &q, &runCnt, &consumedData] {
      std::mt19937 rng{std::random_device{}()};
      std::uniform_int_distribution dist{0, 1};

      while (true) {
        // Push if 0 - Pop if 1
        if (!dist(rng)) {
          const int val{runCnt.fetch_add(1, MemoryOrder::relaxed)};

          if (val >= nPushes) {
            break;
          }

          q.push(val);
        } else {
          if (const auto val{q.pop()}) {
            consumedData[i].push_back(*val);
          }
        }
      }

      while (const auto val{q.pop()}) {
        consumedData[i].push_back(*val);
      }
    });
  }

  threads.clear();
  EXPECT_TRUE(q.empty());
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
