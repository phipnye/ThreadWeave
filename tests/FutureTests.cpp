#include <gtest/gtest.h>
#include <threadweave/Future.h>

#include <stdexcept>
#include <string>
#include <thread>

using namespace ThreadWeave;

TEST(FutureTest, BasicValue) {
  using Node = Internal::Task<int>;
  auto* const node{Internal::NodeAllocator<Node>::allocate()};
  Future<int> future{node};
  new (node->resultStorage_) int{42};
  node->hasResult_ = true;
  node->notify();
  EXPECT_EQ(future.get(), 42);

  if (node->release()) {
    Internal::NodeAllocator<Node>::deallocate(node);
  }
}

TEST(FutureTest, VoidResult) {
  using Node = Internal::Task<void>;
  auto* const node{Internal::NodeAllocator<Node>::allocate()};
  Future<void> future{node};
  node->notify();
  EXPECT_NO_THROW(future.get());

  if (node->release()) {
    Internal::NodeAllocator<Node>::deallocate(node);
  }
}

TEST(FutureTest, RethrowsException) {
  using Node = Internal::Task<int>;
  auto* const node{Internal::NodeAllocator<Node>::allocate()};
  Future<int> future{node};
  node->exception_ = std::make_exception_ptr(std::runtime_error("Task failed"));
  node->notify();
  EXPECT_THROW(future.get(), std::runtime_error);

  if (node->release()) {
    Internal::NodeAllocator<Node>::deallocate(node);
  }
}

TEST(FutureTest, MoveFuture) {
  using Node = Internal::Task<std::string>;
  auto* const node{Internal::NodeAllocator<Node>::allocate()};
  Future<std::string> f1{node};
  Future<std::string> f2{std::move(f1)};
  new (node->resultStorage_) std::string{"ThreadWeave"};
  node->hasResult_ = true;
  node->notify();
  EXPECT_EQ(f2.get(), "ThreadWeave");

  if (node->release()) {
    Internal::NodeAllocator<Node>::deallocate(node);
  }
}

TEST(FutureTest, ThreadedGet) {
  using Node = Internal::Task<int>;
  auto* const node{Internal::NodeAllocator<Node>::allocate()};
  Future<int> future{node};

  std::jthread producer{[node] {
    new (node->resultStorage_) int(777);
    node->hasResult_ = true;
    node->notify();

    if (node->release()) {
      Internal::NodeAllocator<Node>::deallocate(node);
    }
  }};

  EXPECT_EQ(future.get(), 777);
}

TEST(FutureTest, MoveAssignmentRetiresOldNode) {
  using Node = Internal::Task<int>;
  auto* const oldNode{Internal::NodeAllocator<Node>::allocate()};
  auto* const newNode{Internal::NodeAllocator<Node>::allocate()};
  Future<int> f1{oldNode};
  Future<int> f2{newNode};
  f2 = std::move(f1);

  // Verify oldNode's refCount was decremented (should now be 1, held by
  // producer)
  EXPECT_EQ(oldNode->refCount_.load(MemoryOrder::relaxed), 1);

  // Fulfill newNode and verify f2 receives it
  new (newNode->resultStorage_) int{99};
  newNode->hasResult_ = true;
  newNode->notify();
  EXPECT_EQ(f2.get(), 99);

  // Cleanup producer references
  if (oldNode->release()) {
    Internal::NodeAllocator<Node>::deallocate(oldNode);
  }

  if (newNode->release()) {
    Internal::NodeAllocator<Node>::deallocate(newNode);
  }
}
