#include <gtest/gtest.h>
#include <threadweave/internal/Node.h>
#include <threadweave/internal/NodeAllocator.h>
#include <threadweave/internal/Task.h>
#include <threadweave/internal/utils.h>

#include <algorithm>
#include <atomic>
#include <mutex>
#include <thread>
#include <type_traits>
#include <vector>

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
  static inline std::atomic<int> constructCount{0};
  static inline std::atomic<int> destructCount{0};
  std::vector<int> values{};

  NonTrivialPayload() noexcept {
    constructCount.fetch_add(1, MemoryOrder::relaxed);
  }

  ~NonTrivialPayload() {
    destructCount.fetch_add(1, MemoryOrder::relaxed);
  }

  static void resetCounters() {
    constructCount.store(0, MemoryOrder::relaxed);
    destructCount.store(0, MemoryOrder::relaxed);
  }
};

static_assert(!std::is_trivially_destructible_v<NonTrivialPayload>);

// Spawns several threads that each allocate a batch of nodes with no
// deallocation, then verifies every pointer handed out was unique (guards
// against races in the global free-list CAS loops / block allocation path)
template <typename Node, Index NodesPerBlock>
void concurrentAllocateProducesUniquePointers() {
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
        local.push_back(NodeAllocator<Node, NodesPerBlock>::allocate());
      }
    });
  }

  threads.clear();
  std::vector<Node*> allPtrs{};
  allPtrs.reserve(nThreads * nPerThread);

  for (auto& v : threadPtrs) {
    allPtrs.insert(allPtrs.end(), v.begin(), v.end());
  }

  const std::size_t sz{allPtrs.size()};
  EXPECT_EQ(sz, static_cast<std::size_t>(nThreads * nPerThread));
  std::ranges::sort(allPtrs);
  allPtrs.erase(std::unique(allPtrs.begin(), allPtrs.end()), allPtrs.end());
  EXPECT_EQ(allPtrs.size(), sz)
      << "NodeAllocator handed out a duplicate pointer under concurrent load";
}

// NOTE: Each allocator has a unique NodesPerBlock since each instantiation gets
// its own static and thread local data which keeps runs separate across tests
// if run sequentially

TEST(NodeAllocatorStackNodeTests, AllocateReturnsNonNull) {
  using Allocator = NodeAllocator<StackNode<int>, 4>;
  auto* const node{Allocator::allocate()};
  EXPECT_NE(node, nullptr);
  Allocator::deallocate(node);
}

TEST(NodeAllocatorStackNodeTests, AllocateReturnsDistinctPointers) {
  using Allocator = NodeAllocator<StackNode<int>, 5>;
  auto* const a{Allocator::allocate()};
  auto* const b{Allocator::allocate()};
  EXPECT_NE(a, b);
  Allocator::deallocate(a);
  Allocator::deallocate(b);
}

TEST(NodeAllocatorStackNodeTests, AllocatedNodeStartsInResetState) {
  using Allocator = NodeAllocator<StackNode<int>, 6>;
  auto* const node{Allocator::allocate()};
  EXPECT_EQ(node->data, 0);
  EXPECT_EQ(node->next, nullptr);
  Allocator::deallocate(node);
}

TEST(NodeAllocatorStackNodeTests, DeallocateNullptrIsSafe) {
  using Allocator = NodeAllocator<StackNode<int>, 7>;
  Allocator::deallocate(nullptr);
  SUCCEED();
}

TEST(NodeAllocatorStackNodeTests, DeallocateResetsNodeImmediately) {
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

TEST(NodeAllocatorStackNodeTests, SameThreadRecyclesSameNode) {
  using Node = StackNode<int>;
  using Allocator = NodeAllocator<Node, 8>;
  auto* const a{Allocator::allocate()};
  Allocator::deallocate(a);
  auto* const b{Allocator::allocate()};
  EXPECT_EQ(a, b);
  Allocator::deallocate(b);
}

TEST(NodeAllocatorStackNodeTests, SameThreadRecyclesSameNodeAcrossIters) {
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

TEST(NodeAllocatorStackNodeTests, AllocatingMoreThanOneBlockYieldsUniqueNodes) {
  constexpr Index kSmallBlock{2};
  using Node = StackNode<int>;
  using Allocator = NodeAllocator<Node, kSmallBlock>;
  constexpr int nNodes{25};
  std::vector<Node*> nodes{};
  nodes.reserve(nNodes);

  for (int i{0}; i < nNodes; ++i) {
    nodes.push_back(Allocator::allocate());
  }

  std::ranges::sort(nodes);
  EXPECT_EQ(std::ranges::adjacent_find(nodes), nodes.end());

  for (auto* const node : nodes) {
    Allocator::deallocate(node);
  }
}

TEST(NodeAllocatorStackNodeTests, NodesFromEveryBlockAreWritableAndReadable) {
  constexpr Index kSmallBlock{5};
  constexpr int nNodes{50};
  using Node = StackNode<int>;
  using Allocator = NodeAllocator<Node, kSmallBlock>;
  std::vector<Node*> nodes{};
  nodes.reserve(nNodes);

  for (int i{0}; i < nNodes; ++i) {
    auto* node{Allocator::allocate()};
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
