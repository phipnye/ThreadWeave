#include <gtest/gtest.h>
#include <threadweave/internal/Node.h>
#include <threadweave/internal/NodeAllocator.h>
#include <threadweave/internal/Task.h>
#include <threadweave/internal/utils.h>

#include <algorithm>
#include <atomic>
#include <mutex>
#include <queue>
#include <thread>
#include <type_traits>
#include <vector>

// TODO: Some tests won't pass right now since recycling does not happen
// immediately anymore

using namespace ThreadWeave;
using Internal::NodeAllocator;
using Internal::QueueNode;
using Internal::StackNode;
using Internal::Task;

static_assert(Internal::AllocatorEligibleNode<StackNode<int>>);
static_assert(Internal::AllocatorEligibleNode<QueueNode<int>>);
static_assert(Internal::AllocatorEligibleNode<Task<int>>);
static_assert(Internal::AllocatorEligibleNode<Task<void>>);

struct NonTrivialPayload {
  static inline std::atomic<int> constructCnt{0};
  static inline std::atomic<int> destructCnt{0};
  std::vector<int> values{};

  NonTrivialPayload() noexcept {
    constructCnt.fetch_add(1, MemoryOrder::relaxed);
  }

  ~NonTrivialPayload() {
    destructCnt.fetch_add(1, MemoryOrder::relaxed);
  }

  static void resetCounters() {
    constructCnt.store(0, MemoryOrder::relaxed);
    destructCnt.store(0, MemoryOrder::relaxed);
  }
};

static_assert(!std::is_trivially_destructible_v<NonTrivialPayload>);

// Spawns several threads that each allocate a batch of nodes with no
// deallocation, then verifies every pointer handed out was unique (guards
// against races in the global free-list CAS loops / block allocation path)
template <typename Node, Index NodesPerBlock>
void concurrentAllocateProducesUniquePointers() {
  using Allocator = NodeAllocator<Node, NodesPerBlock>;
  constexpr int nThreads{8};
  constexpr int nPerThread{200};
  std::vector<std::vector<Node*>> threadPtrs(nThreads);
  std::vector<std::jthread> threads{};
  threads.reserve(nThreads);

  for (int t{0}; t < nThreads; ++t) {
    threads.emplace_back([&, t] {
      auto& local{threadPtrs[t]};
      local.reserve(nPerThread);

      for (int i{0}; i < nPerThread; ++i) {
        local.push_back(Allocator::allocate());
      }
    });
  }

  threads.clear();
  std::vector<Node*> allPtrs{};
  allPtrs.reserve(nThreads * nPerThread);

  for (auto& v : threadPtrs) {
    allPtrs.insert(allPtrs.end(), v.begin(), v.end());
  }

  const std::size_t nOrig{allPtrs.size()};
  EXPECT_EQ(nOrig, static_cast<std::size_t>(nThreads * nPerThread));
  std::ranges::sort(allPtrs);
  allPtrs.erase(std::ranges::unique(allPtrs).begin(), allPtrs.end());
  EXPECT_EQ(allPtrs.size(), nOrig)
      << "NodeAllocator handed out a duplicate pointer under concurrent load";

  for (auto* const ptr : allPtrs) {
    Allocator::deallocate(ptr);
  }
}

// Producer threads allocate nodes and hand them off through a shared queue,
// and consumer threads pop and deallocate them
template <typename Node, Index NodesPerBlock>
void producerConsumerCrossThreadDeallocation() {
  using Allocator = NodeAllocator<Node, NodesPerBlock>;
  constexpr int nProducers{4};
  constexpr int nConsumers{4};
  constexpr int nPerProducer{2'000};
  constexpr int nTotal{nProducers * nPerProducer};
  std::mutex qMutex{};
  std::queue<Node*> q{};
  std::atomic<int> producedCnt{0};
  std::atomic<int> consumedCnt{0};
  std::atomic<bool> producersDone{false};

  std::vector<std::jthread> consumers{};
  consumers.reserve(nConsumers);

  for (int c{0}; c < nConsumers; ++c) {
    consumers.emplace_back([&] {
      while (!producersDone.load(MemoryOrder::acquire) ||
             consumedCnt.load(MemoryOrder::relaxed) <
                 producedCnt.load(MemoryOrder::acquire)) {
        Node* node{nullptr};

        {
          std::lock_guard lock{qMutex};
          if (!q.empty()) {
            node = q.front();
            q.pop();
          }
        }

        if (node) {
          Allocator::deallocate(node);
          consumedCnt.fetch_add(1, MemoryOrder::relaxed);
        } else {
          std::this_thread::yield();
        }
      }
    });
  }

  std::vector<std::jthread> producers{};
  producers.reserve(nProducers);

  for (int p{0}; p < nProducers; ++p) {
    producers.emplace_back([&] {
      for (int i{0}; i < nPerProducer; ++i) {
        {
          std::lock_guard lock{qMutex};
          q.push(Allocator::allocate());
        }

        producedCnt.fetch_add(1, MemoryOrder::release);
      }
    });
  }

  producers.clear();
  producersDone.store(true, MemoryOrder::release);
  consumers.clear();
  EXPECT_EQ(producedCnt.load(MemoryOrder::relaxed), nTotal);
  EXPECT_EQ(consumedCnt.load(MemoryOrder::relaxed), nTotal);
}

// NOTE: Each allocator has a unique NodesPerBlock since each instantiation gets
// its own static and thread local data which keeps runs separate across tests
// if run sequentially

// --- Stack nodes

TEST(NodeAllocatorTests, AllocateStackNodeReturnsNonNull) {
  using Allocator = NodeAllocator<StackNode<int>, 4>;
  auto* const node{Allocator::allocate()};
  EXPECT_NE(node, nullptr);
  Allocator::deallocate(node);
}

TEST(NodeAllocatorTests, AllocateReturnsDistinctPointers) {
  using Allocator = NodeAllocator<StackNode<int>, 5>;
  auto* const a{Allocator::allocate()};
  auto* const b{Allocator::allocate()};
  EXPECT_NE(a, b);
  Allocator::deallocate(a);
  Allocator::deallocate(b);
}

TEST(NodeAllocatorTests, AllocatedStackNodeStartsInResetState) {
  using Allocator = NodeAllocator<StackNode<int>, 6>;
  auto* const node{Allocator::allocate()};
  EXPECT_EQ(node->data, 0);
  EXPECT_EQ(node->next, nullptr);
  Allocator::deallocate(node);
}

TEST(NodeAllocatorTests, DeallocateNullptrIsSafe) {
  using Allocator = NodeAllocator<StackNode<int>, 7>;
  Allocator::deallocate(nullptr);
  SUCCEED();
}

TEST(NodeAllocatorTests, DeallocateResetsNodeImmediately) {
  using Node = StackNode<int>;
  using Allocator = NodeAllocator<Node, 7>;
  auto* const node{Allocator::allocate()};
  node->data = 99;
  Allocator::deallocate(node);

  // Checking this is safe here because it's run in isolation (no other threads
  // to worry about) and the data is not actually freed until the end of the
  // program
  EXPECT_EQ(node->data, 0);
  EXPECT_EQ(node->next, nullptr);
}

TEST(NodeAllocatorTests, SameThreadRecyclesSameStackNode) {
  using Node = StackNode<int>;
  using Allocator = NodeAllocator<Node, 8>;
  auto* const a{Allocator::allocate()};
  Allocator::deallocate(a);
  auto* const b{Allocator::allocate()};
  EXPECT_EQ(a, b);
  Allocator::deallocate(b);
}

TEST(NodeAllocatorTests, SameThreadRecyclesSameStackNodeAcrossIters) {
  using Node = StackNode<int>;
  using Allocator = NodeAllocator<Node, 9>;
  constexpr int nIterations{5'000};
  Node* prev{nullptr};

  for (int i{0}; i < nIterations; ++i) {
    auto* const curr{Allocator::allocate()};
    EXPECT_NE(curr, nullptr);
    curr->data = i;
    Allocator::deallocate(curr);

    // Within a single thread, node should be recycled over and over
    if (prev) {
      EXPECT_EQ(curr, prev);
    }

    prev = curr;
  }
}

TEST(NodeAllocatorTests, AllocatingMoreThanOneBlockYieldsUniqueStackNodes) {
  constexpr Index kSmallBlock{2};
  using Node = StackNode<int>;
  using Allocator = NodeAllocator<Node, kSmallBlock>;
  constexpr int nNodes{25};
  std::vector<Node*> nodes{};
  nodes.reserve(nNodes);

  for (int i{0}; i < nNodes; ++i) {
    nodes.push_back(Allocator::allocate());
  }

  const auto nOrig{nodes.size()};
  std::ranges::sort(nodes);
  nodes.erase(std::ranges::unique(nodes).begin(), nodes.end());
  EXPECT_EQ(nOrig, nodes.size());

  for (auto* const node : nodes) {
    Allocator::deallocate(node);
  }
}

TEST(NodeAllocatorTests, StackNodesFromEveryBlockAreWritableAndReadable) {
  constexpr Index kSmallBlock{3};
  constexpr int nNodes{50};
  using Node = StackNode<int>;
  using Allocator = NodeAllocator<Node, kSmallBlock>;
  std::vector<Node*> nodes{};
  nodes.reserve(nNodes);

  for (int i{0}; i < nNodes; ++i) {
    auto* const node{Allocator::allocate()};
    node->data = i;
    nodes.push_back(node);
  }

  for (int i{0}; i < nNodes; ++i) {
    EXPECT_EQ(nodes[i]->data, i);
  }

  for (auto* const node : nodes) {
    Allocator::deallocate(node);
  }
}

// --- Queue Nodes

TEST(NodeAllocatorTests, AllocateQueueNodeReturnsNonNull) {
  using Allocator = NodeAllocator<QueueNode<int>, 10>;
  auto* const node{Allocator::allocate()};
  EXPECT_NE(node, nullptr);
  Allocator::deallocate(node);
}

TEST(NodeAllocatorTests, AllocatedQueueNodeStartsInResetState) {
  using Allocator = NodeAllocator<QueueNode<int>, 11>;
  auto* const node{Allocator::allocate()};
  EXPECT_EQ(node->data, 0);
  EXPECT_EQ(node->next.load(MemoryOrder::relaxed), nullptr);
  Allocator::deallocate(node);
}

TEST(NodeAllocatorTests, DeallocateResetsAtomicNextImmediately) {
  using Node = QueueNode<int>;
  using Allocator = NodeAllocator<Node, 12>;
  auto* const node{Allocator::allocate()};
  node->data = 7;
  node->next.store(reinterpret_cast<Node*>(0x1), MemoryOrder::relaxed);
  Allocator::deallocate(node);

  // Checking this is safe here because it's run in isolation (no other threads
  // to worry about) and the data is not actually freed until the end of the
  // program
  EXPECT_EQ(node->data, 0);
  EXPECT_EQ(node->next.load(MemoryOrder::relaxed), nullptr);
}

TEST(NodeAllocatorTests, SameThreadRecyclesSameQueueNode) {
  using Node = QueueNode<int>;
  using Allocator = NodeAllocator<Node, 13>;
  auto* const a{Allocator::allocate()};
  Allocator::deallocate(a);
  auto* const b{Allocator::allocate()};
  EXPECT_EQ(a, b);
  Allocator::deallocate(b);
}

TEST(NodeAllocatorTests, SameThreadRecyclesSameQueueNodeAcrossIters) {
  using Node = QueueNode<int>;
  using Allocator = NodeAllocator<Node, 14>;
  constexpr int nIterations{5'000};
  Node* prev{nullptr};

  for (int i{0}; i < nIterations; ++i) {
    auto* const curr{Allocator::allocate()};
    EXPECT_NE(curr, nullptr);
    curr->data = i;
    Allocator::deallocate(curr);

    if (prev) {
      EXPECT_EQ(curr, prev);
    }

    prev = curr;
  }
}

TEST(NodeAllocatorTests, AllocatingMoreThanOneBlockYieldsUniqueQueueNodes) {
  constexpr Index kSmallBlock{2};
  using Node = QueueNode<int>;
  using Allocator = NodeAllocator<Node, kSmallBlock>;
  constexpr int nNodes{25};
  std::vector<Node*> nodes{};
  nodes.reserve(nNodes);

  for (int i{0}; i < nNodes; ++i) {
    nodes.push_back(Allocator::allocate());
  }

  const auto nOrig{nodes.size()};
  std::ranges::sort(nodes);
  nodes.erase(std::ranges::unique(nodes).begin(), nodes.end());
  EXPECT_EQ(nOrig, nodes.size());

  for (auto* const node : nodes) {
    Allocator::deallocate(node);
  }
}

TEST(NodeAllocatorTests, QueueNodesFromEveryBlockAreWritableAndReadable) {
  constexpr Index kSmallBlock{3};
  constexpr int nNodes{50};
  using Node = QueueNode<int>;
  using Allocator = NodeAllocator<Node, kSmallBlock>;
  std::vector<Node*> nodes{};
  nodes.reserve(nNodes);

  for (int i{0}; i < nNodes; ++i) {
    auto* const node{Allocator::allocate()};
    node->data = i;
    nodes.push_back(node);
  }

  for (int i{0}; i < nNodes; ++i) {
    EXPECT_EQ(nodes[i]->data, i);
  }

  for (auto* const node : nodes) {
    Allocator::deallocate(node);
  }
}

// --- Task nodes

TEST(NodeAllocatorTests, AllocateTaskNodeReturnsNonNullAndReset) {
  using Allocator = NodeAllocator<Task<int>, 15>;
  auto* const task{Allocator::allocate()};
  EXPECT_NE(task, nullptr);
  EXPECT_FALSE(task->isReady());
  EXPECT_FALSE(task->hasResult_);
  EXPECT_EQ(task->refCount_.load(MemoryOrder::relaxed), 2);
  Allocator::deallocate(task);
}

TEST(NodeAllocatorTests, SameThreadRecyclesSameTaskNode) {
  using Node = Task<int>;
  using Allocator = NodeAllocator<Node, 16>;
  auto* const a{Allocator::allocate()};
  Allocator::deallocate(a);
  auto* const b{Allocator::allocate()};
  EXPECT_EQ(a, b);
  Allocator::deallocate(b);
}

TEST(NodeAllocatorTests, AllocatingMoreThanOneBlockYieldsUniqueTaskNodes) {
  constexpr Index kSmallBlock{2};
  using Node = Task<int>;
  using Allocator = NodeAllocator<Node, kSmallBlock>;
  constexpr int nNodes{25};
  std::vector<Node*> nodes{};
  nodes.reserve(nNodes);

  for (int i{0}; i < nNodes; ++i) {
    nodes.push_back(Allocator::allocate());
  }

  const auto nOrig{nodes.size()};
  std::ranges::sort(nodes);
  nodes.erase(std::ranges::unique(nodes).begin(), nodes.end());
  EXPECT_EQ(nOrig, nodes.size());

  for (auto* const node : nodes) {
    Allocator::deallocate(node);
  }
}

// --- Non-trivial payload

TEST(NodeAllocatorTests, DeallocateDestroysAndReconstructsNonTrivialPayload) {
  using Node = StackNode<NonTrivialPayload>;
  using Allocator = NodeAllocator<Node, 17>;
  NonTrivialPayload::resetCounters();
  auto* const node{Allocator::allocate()};
  node->data.values = {1, 2, 3};
  const int constructedBefore{
      NonTrivialPayload::constructCnt.load(MemoryOrder::relaxed)};
  Allocator::deallocate(node);

  // resetValue() should have destroyed the populated payload and performed a
  // placement new, fresh default-constructed one in its place
  EXPECT_TRUE(node->data.values.empty());
  EXPECT_GE(NonTrivialPayload::destructCnt.load(MemoryOrder::relaxed), 1);
  EXPECT_GT(NonTrivialPayload::constructCnt.load(MemoryOrder::relaxed),
            constructedBefore);
}

// --- Concurrency

TEST(NodeAllocatorTests, ConcurrentAllocateProducesUniquePointersStackNode) {
  concurrentAllocateProducesUniquePointers<StackNode<int>, 18>();
}

TEST(NodeAllocatorTests, ConcurrentAllocateProducesUniquePointersQueueNode) {
  concurrentAllocateProducesUniquePointers<QueueNode<int>, 19>();
}

TEST(NodeAllocatorTests, ConcurrentAllocateProducesUniquePointersTaskNode) {
  concurrentAllocateProducesUniquePointers<Task<int>, 20>();
}

TEST(NodeAllocatorTests, CrossThreadProducerConsumerStackNode) {
  producerConsumerCrossThreadDeallocation<StackNode<int>, 21>();
}

TEST(NodeAllocatorTests, CrossThreadProducerConsumerQueueNode) {
  producerConsumerCrossThreadDeallocation<QueueNode<int>, 22>();
}

TEST(NodeAllocatorTests, CrossThreadProducerConsumerTaskNode) {
  producerConsumerCrossThreadDeallocation<Task<int>, 23>();
}

TEST(NodeAllocatorTests, SingleNodeCrossThreadDeallocationIsSafe) {
  using Node = StackNode<int>;
  using Allocator = NodeAllocator<Node, 24>;
  constexpr int nIterations{2'000};

  for (int i{0}; i < nIterations; ++i) {
    auto* const node{Allocator::allocate()};
    node->data = i;

    std::jthread deallocator([&] {
      EXPECT_EQ(node->data, i);
      Allocator::deallocate(node);
    });
  }

  SUCCEED();
}
