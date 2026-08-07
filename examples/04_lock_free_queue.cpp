// ThreadWeave's lock-free data structures can be used directly, independent
// of the thread pool. This shows MichaelScottQueue use under MPMC semantics.
//
// Build:
//   g++ -std=c++23 -Iinclude -pthread 04_lock_free_queue.cpp -o lock_free_queue
// Run:
//   ./lock_free_queue

#include <threadweave/MichaelScottQueue.h>

#include <atomic>
#include <iostream>
#include <thread>
#include <vector>

namespace {

constexpr int kNumProducers{4};
constexpr int kNumConsumers{4};
constexpr int kItemsPerProducer{10'000};

}  // namespace

int main() {
  ThreadWeave::MichaelScottQueue<int> queue{};
  std::atomic<int> itemsConsumed{0};
  std::atomic<int> producersRemaining{kNumProducers};

  // Producers push items concurrently
  std::vector<std::jthread> producers{};

  for (int p{0}; p < kNumProducers; ++p) {
    producers.emplace_back([&, p] {
      for (int i{0}; i < kItemsPerProducer; ++i) {
        queue.push(p * kItemsPerProducer + i);
      }

      producersRemaining.fetch_sub(1, std::memory_order::release);
    });
  }

  // Multiple consumers drain the queue concurrently until producers are done
  std::vector<std::jthread> consumers{};

  for (int c{0}; c < kNumConsumers; ++c) {
    consumers.emplace_back([&] {
      while (producersRemaining.load(std::memory_order::acquire) > 0) {
        // Pop returns std::optional where std::nullopt indicates an empty queue
        if (queue.pop()) {
          itemsConsumed.fetch_add(1, std::memory_order::relaxed);
        }
      }
    });
  }

  producers.clear();
  consumers.clear();
  std::cout << "Consumed " << itemsConsumed.load() << " of "
            << kNumProducers * kItemsPerProducer << " items\n";
}
