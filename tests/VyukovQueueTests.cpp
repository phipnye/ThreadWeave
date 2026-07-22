#include <gtest/gtest.h>
#include <threadweave/VyukovQueue.h>
#include <threadweave/utils.h>

#include <atomic>
#include <bitset>
#include <memory>
#include <optional>
#include <random>
#include <thread>
#include <vector>

template <typename T>
using Queue = ThreadWeave::VyukovQueue<T>;

namespace MemoryOrder = ThreadWeave::MemoryOrder;

// Make sure an empty queue returns std::nullopt
TEST(VyukovQueueTests, EmptyPopReturnsNullopt) {
  Queue<int> q{};
  const auto val{q.pop()};
  EXPECT_FALSE(val.has_value());
}

// Trivial single push/pop
TEST(VyukovQueueTests, SinglePushPop) {
  Queue<int> q{};
  q.push(42);
  const auto val{q.pop()};
  ASSERT_TRUE(val.has_value());
  EXPECT_EQ(*val, 42);
  EXPECT_FALSE(q.pop().has_value());
}

// Make sure the queue follows FIFO when executed sequentially
TEST(VyukovQueueTests, QueueIsFifo) {
  constexpr int n{100};
  Queue<int> q{};

  // Push 0-99 into queue
  for (int i{0}; i < n; ++i) {
    q.push(i);
  }

  // Pop 0-99 from the queue to ensure data integrity and FIFO behavior
  for (int i{0}; i < n; ++i) {
    const auto val{q.pop()};
    ASSERT_TRUE(val.has_value());
    EXPECT_EQ(*val, i);
  }

  EXPECT_FALSE(q.pop().has_value());
}

// Test FIFO against a move-only type
TEST(VyukovQueueTests, MoveOnlyType) {
  constexpr int n{100};
  Queue<std::unique_ptr<int>> q{};

  for (int i{0}; i < n; ++i) {
    q.push(std::make_unique<int>(i));
  }

  for (int i{0}; i < n; ++i) {
    const auto val{q.pop()};
    ASSERT_TRUE(val.has_value());
    ASSERT_NE(*val, nullptr);
    EXPECT_EQ(**val, i);
  }

  EXPECT_FALSE(q.pop().has_value());
}

// Test concurrent pushes into a single consumer
TEST(VyukovQueueTests, ConcurrentPushes) {
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
  constexpr int maxVal{nThreads * itemsPerThread};
  std::bitset<maxVal> seen{};

  // Single-threaded pop after all pushes complete
  while (const auto val{q.pop()}) {
    const auto idx{*val};
    EXPECT_GE(idx, 0);
    EXPECT_LT(idx, maxVal);
    EXPECT_FALSE(seen.test(idx));
    seen.set(idx);
  }

  EXPECT_TRUE(seen.all());
  EXPECT_FALSE(q.pop().has_value());
}

// Ensure data integrity with concurrent producers and a single consumer
TEST(VyukovQueueTests, ConcurrentProducersSingleConsumer) {
  Queue<int> q{};
  constexpr int nProducers{8};
  constexpr int opsPerThread{5000};

  std::vector<std::jthread> producers{};
  producers.reserve(nProducers);
  std::atomic<int> activeProducers{nProducers};
  std::vector<int> consumedData{};

  // Spawn multiple producers
  for (int i{0}; i < nProducers; ++i) {
    producers.emplace_back([=, &q, &activeProducers] {
      for (int j{0}; j < opsPerThread; ++j) {
        q.push(i * opsPerThread + j);
      }

      activeProducers.fetch_sub(1, MemoryOrder::release);
    });
  }

  // Single consumer thread
  std::jthread consumer{[&q, &activeProducers, &consumedData] {
    while (true) {
      if (const auto val{q.pop()}) {
        consumedData.push_back(*val);
      } else {
        if (activeProducers.load(MemoryOrder::acquire) == 0) {
          // Final drain pass after all producers sign off
          while (const auto finalVal{q.pop()}) {
            consumedData.push_back(*finalVal);
          }

          break;
        }

        std::this_thread::yield();
      }
    }
  }};

  producers.clear();
  consumer.join();
  constexpr int totalItems{nProducers * opsPerThread};
  std::bitset<totalItems> seen{};

  for (const int item : consumedData) {
    EXPECT_GE(item, 0);
    EXPECT_LT(item, totalItems);
    EXPECT_FALSE(seen.test(item));
    seen.set(item);
  }

  EXPECT_TRUE(seen.all());
  EXPECT_FALSE(q.pop().has_value());
}

// Randomized race condition test with multiple producers pushing at random
// intervals while a single consumer continuously pops
TEST(VyukovQueueTests, RandomizedMultiProducerSingleConsumerTest) {
  constexpr int nProducers{7};
  constexpr int nPushesPerProducer{10'000};
  constexpr int totalPushes{nProducers * nPushesPerProducer};

  Queue<int> q{};
  std::vector<std::jthread> producers{};
  producers.reserve(nProducers);
  std::atomic<bool> doneProducing{false};
  std::vector<int> consumedData{};

  // Single consumer thread
  std::jthread consumer([&q, &doneProducing, &consumedData] {
    while (true) {
      if (const auto val{q.pop()}) {
        consumedData.push_back(*val);
      } else if (doneProducing.load(MemoryOrder::acquire)) {
        // Drain any remaining items after all producers finish
        while (const auto finalVal{q.pop()}) {
          consumedData.push_back(*finalVal);
        }

        break;
      } else {
        std::this_thread::yield();
      }
    }
  });

  // Multiple producer threads with randomized delays
  for (int t{0}; t < nProducers; ++t) {
    producers.emplace_back([=, &q] {
      std::mt19937 rng{std::random_device{}()};
      std::uniform_int_distribution dist{0, 3};

      for (int i{0}; i < nPushesPerProducer; ++i) {
        q.push(t * nPushesPerProducer + i);
        if (dist(rng) == 0) {
          std::this_thread::yield();
        }
      }
    });
  }

  producers.clear();
  doneProducing.store(true, MemoryOrder::release);
  consumer.join();
  EXPECT_EQ(consumedData.size(), totalPushes);
  std::bitset<totalPushes> seen{};

  for (const int item : consumedData) {
    EXPECT_GE(item, 0);
    EXPECT_LT(item, totalPushes);
    EXPECT_FALSE(seen.test(item));
    seen.set(item);
  }

  EXPECT_TRUE(seen.all());
  EXPECT_FALSE(q.pop().has_value());
}
