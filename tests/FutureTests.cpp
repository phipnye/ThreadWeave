#include <gtest/gtest.h>
#include <threadweave/Future.h>
#include <threadweave/internal/Task.h>
#include <threadweave/internal/utils.h>

#include <atomic>
#include <exception>
#include <new>
#include <stdexcept>
#include <utility>

using namespace ThreadWeave;

struct HelpWait {
  Internal::TaskBase* lastHelpedTask{nullptr};
  int helpWaitCount{0};

  void operator()(Internal::TaskBase* const task) noexcept {
    lastHelpedTask = task;
    ++helpWaitCount;

    if (task) {
      task->notify();
    }
  }
};

template <typename T>
struct NodeAllocator {
  static inline int deallocateCount{0};
  static inline T* lastDeallocated{nullptr};

  static void deallocate(T* const ptr) noexcept {
    ++deallocateCount;
    lastDeallocated = ptr;
    delete ptr;
  }
};

struct MoveOnlyTracker {
  static inline int dtorCnt{0};
  int val_{0};

  explicit MoveOnlyTracker(const int val = 0) : val_{val} {}

  ~MoveOnlyTracker() {
    ++dtorCnt;
  }

  MoveOnlyTracker(const MoveOnlyTracker&) = delete;
  MoveOnlyTracker& operator=(const MoveOnlyTracker&) = delete;

  MoveOnlyTracker(MoveOnlyTracker&& other) noexcept : val_{other.val_} {
    other.val_ = -1;
  }

  MoveOnlyTracker& operator=(MoveOnlyTracker&& other) noexcept {
    if (this != &other) {
      val_ = other.val_;
      other.val_ = -1;
    }

    return *this;
  }
};

TEST(FutureTest, GetReturnsValueAndRetiresTask) {
  constexpr HelpWait helpWait{};
  auto* const task{new Internal::Task<int>{}};
  constexpr int expectedVal{42};
  new (task->resultStorage_) int{expectedVal};
  task->hasResult_ = true;
  Future<int> fut{task};
  const int actualVal{fut.get()};
  EXPECT_EQ(actualVal, expectedVal);
  EXPECT_FALSE(task->hasResult_);
  EXPECT_EQ(task->refCount_.load(MemoryOrder::relaxed), 1);
  EXPECT_EQ(helpWait.helpWaitCount, 1);
  EXPECT_EQ(helpWait.lastHelpedTask, task);
  delete task;
}

TEST(FutureTest, GetMovesAndDestroysInPlaceForNonCopyableTypes) {
  MoveOnlyTracker::dtorCnt = 0;
  auto* const task{new Internal::Task<MoveOnlyTracker>{}};
  new (task->resultStorage_) MoveOnlyTracker{100};
  task->hasResult_ = true;
  Future<MoveOnlyTracker> fut{task};
  MoveOnlyTracker res{fut.get()};
  EXPECT_EQ(res.val_, 100);
  EXPECT_FALSE(task->hasResult_);
  EXPECT_EQ(MoveOnlyTracker::dtorCnt, 1);
  delete task;
}

TEST(FutureTest, VoidFutureGetSupport) {
  HelpWait helpWait;
  auto* const task{new Internal::Task<void>{}};
  task->hasResult_ = true;
  Future<void> fut{task};
  fut.get();
  EXPECT_EQ(task->refCount_.load(MemoryOrder::relaxed), 1);
  EXPECT_EQ(helpWait.helpWaitCount, 1);
  delete task;
}

TEST(FutureTest, GetRethrowsStoredExceptionAndRetiresTask) {
  auto* const task{new Internal::Task<int>{}};

  try {
    throw std::runtime_error{"Execution failure"};
  } catch (...) {
    task->exception_ = std::current_exception();
  }

  Future<int> fut{task};
  EXPECT_THROW(fut.get(), std::runtime_error);
  EXPECT_EQ(task->refCount_.load(MemoryOrder::relaxed), 1);
  EXPECT_EQ(task->exception_, nullptr);
  delete task;
}

TEST(FutureTest, WaitCallsHelpWaitWithoutConsumingTask) {
  HelpWait helpWait;
  Internal::Task<int> task{};
  Future<int> fut{&task};
  fut.wait();
  EXPECT_EQ(helpWait.helpWaitCount, 1);
  EXPECT_EQ(helpWait.lastHelpedTask, &task);
  EXPECT_EQ(task.refCount_.load(MemoryOrder::relaxed), 2);
}

TEST(FutureTest, MoveConstructorTransfersTaskOwnership) {
  Internal::Task<int> task{};
  new (task.resultStorage_) int{99};
  task.hasResult_ = true;
  Future<int> srcFut{&task};
  Future<int> dstFut{std::move(srcFut)};
  const int val{dstFut.get()};
  EXPECT_EQ(val, 99);
  EXPECT_EQ(task.refCount_.load(MemoryOrder::relaxed), 1);
}

TEST(FutureTest, MoveAssignmentRetiresOldTaskAndStealsNewTask) {
  Internal::Task<int> oldTask{};
  Internal::Task<int> newTask{};
  new (newTask.resultStorage_) int{777};
  newTask.hasResult_ = true;
  Future<int> fut1{&oldTask};
  Future<int> fut2{&newTask};
  fut1 = std::move(fut2);
  EXPECT_EQ(oldTask.refCount_.load(MemoryOrder::relaxed), 1);
  const int val{fut1.get()};
  EXPECT_EQ(val, 777);
  EXPECT_EQ(newTask.refCount_.load(MemoryOrder::relaxed), 1);
}

// TEST(FutureTest, SelfMoveAssignmentIsNoOp) {
//   Internal::Task<int> task{};
//   new (task.resultStorage_) int{123};
//   task.hasResult_ = true;
//   Future<int> fut{&task};
//   fut = std::move(fut);
//   const int val{fut.get()};
//   EXPECT_EQ(val, 123);
// }

TEST(FutureTest, DestructorRetiresTaskWhenUnconsumed) {
  Internal::Task<int> task{};

  {
    Future<int> fut{&task};
    EXPECT_EQ(task.refCount_.load(MemoryOrder::relaxed), 2);
  }

  EXPECT_EQ(task.refCount_.load(MemoryOrder::relaxed), 1);
}

TEST(FutureTest, DeallocatesNodeWhenLastReferenceIsReleased) {
  NodeAllocator<Internal::Task<int>>::deallocateCount = 0;
  NodeAllocator<Internal::Task<int>>::lastDeallocated = nullptr;
  auto* const task{new Internal::Task<int>{}};
  task->refCount_.store(1, MemoryOrder::relaxed);

  { Future<int> fut{task}; }

  EXPECT_EQ(NodeAllocator<Internal::Task<int>>::deallocateCount, 1);
  EXPECT_EQ(NodeAllocator<Internal::Task<int>>::lastDeallocated, task);
}
