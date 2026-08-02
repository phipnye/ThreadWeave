#include <gtest/gtest.h>
#include <threadweave/Future.h>
#include <threadweave/ThreadPool.h>
#include <threadweave/internal/NodeAllocator.h>
#include <threadweave/internal/Task.h>
#include <threadweave/internal/utils.h>

#include <atomic>
#include <chrono>
#include <exception>
#include <new>
#include <stdexcept>
#include <thread>
#include <utility>
#include <vector>

using namespace ThreadWeave;

template <typename T>
Internal::Task<T>* allocateTask() {
  return Internal::NodeAllocator<Internal::Task<T>>::allocate();
}

// Marks a task complete with a stored value and releases the simulated thread
// pool's hold on the reference count, mirroring what a worker does once it
// finishes running a task (store result, notify waiters, and then give up its
// reference)
template <typename T>
void completeWithValue(Internal::Task<T>* const task, T value) {
  ::new (static_cast<void*>(task->resultStorage_)) T{std::move(value)};
  task->hasResult_ = true;
  task->notify();
  task->releaseReference();
}

void completeVoidTask(Internal::Task<void>* const task) {
  task->notify();
  task->releaseReference();
}

template <typename T>
void completeWithException(Internal::Task<T>* const task,
                           std::exception_ptr ex) {
  task->exception_ = std::move(ex);
  task->notify();
  task->releaseReference();
}

#ifndef TW_NDEBUG
TEST(FutureTests, ConstructionAssertsOnNullTask) {
  EXPECT_DEATH({ Future<int> f{nullptr}; }, "null node");
}
#endif

TEST(FutureTests, GetReturnsStoredValueForReadyTask) {
  constexpr int val{42};
  auto* const task{allocateTask<int>()};
  completeWithValue<int>(task, val);
  Future<int> f{task};
  EXPECT_EQ(f.get(), val);
}

TEST(FutureTests, GetOnVoidTask) {
  auto* const task{allocateTask<void>()};
  completeVoidTask(task);
  Future<void> f{task};
  f.get();
  SUCCEED();
}

TEST(FutureTests, GetRethrowsStoredException) {
  auto* const task{allocateTask<int>()};
  completeWithException<int>(
      task, std::make_exception_ptr(std::runtime_error{"Error"}));
  Future<int> f{task};
  EXPECT_THROW({ f.get(); }, std::runtime_error);
}

TEST(FutureTests, GetRethrowsStoredExceptionForVoidTask) {
  auto* const task{allocateTask<void>()};
  completeWithException<void>(
      task, std::make_exception_ptr(std::runtime_error{"Error"}));
  Future<void> f{task};
  EXPECT_THROW({ f.get(); }, std::runtime_error);
}

TEST(FutureTests, WaitReturnsImmediatelyIfTaskAlreadyReady) {
  constexpr int val{42};
  auto* const task{allocateTask<int>()};
  completeWithValue<int>(task, val);
  Future<int> f{task};
  f.wait();
  EXPECT_EQ(f.get(), val);
}

TEST(FutureTests, WaitBlocksUntilTaskCompletes) {
  constexpr int val{42};
  auto* const task{allocateTask<int>()};
  Future<int> f{task};
  std::jthread completer([&] {
    std::this_thread::sleep_for(std::chrono::milliseconds{20});
    completeWithValue<int>(task, val);
  });
  f.wait();
  EXPECT_EQ(f.get(), val);
}

TEST(FutureTests, GetBlocksUntilTaskCompletes) {
  constexpr int val{42};
  auto* const task{allocateTask<int>()};
  Future<int> f{task};
  std::jthread completer([&] {
    std::this_thread::sleep_for(std::chrono::milliseconds{20});
    completeWithValue<int>(task, val);
  });
  EXPECT_EQ(f.get(), val);
}

TEST(FutureTests, MoveConstructTransfersOwnership) {
  constexpr int val{42};
  auto* const task{allocateTask<int>()};
  completeWithValue<int>(task, val);
  Future<int> f1{task};
  Future<int> f2{std::move(f1)};
  EXPECT_EQ(f2.get(), val);
}

#ifndef TW_NDEBUG
TEST(FutureTests, UsingAMovedFromFutureAsserts) {
  auto* const task{allocateTask<int>()};
  completeWithValue<int>(task, 1);
  Future<int> f1{task};
  Future<int> f2{std::move(f1)};
  EXPECT_DEATH({ f1.wait(); }, "uninitialized");
  f2.get();
}
#endif

TEST(FutureTests, MoveAssignReleasesPreviousTaskAndAdoptsNewOne) {
  constexpr int val1{1};
  constexpr int val2{2};
  auto* const task1{allocateTask<int>()};
  completeWithValue<int>(task1, val1);
  auto* const task2{allocateTask<int>()};
  completeWithValue<int>(task2, val2);
  Future<int> f1{task1};
  Future<int> f2{task2};
  f1 = std::move(f2);
  EXPECT_EQ(f1.get(), val2);
}

#ifndef TW_NDEBUG
TEST(FutureTests, MoveAssignLeavesSourceEmpty) {
  auto* const task1{allocateTask<int>()};
  completeWithValue<int>(task1, 1);
  auto* const task2{allocateTask<int>()};
  completeWithValue<int>(task2, 2);
  Future<int> f1{task1};
  Future<int> f2{task2};
  f1 = std::move(f2);
  EXPECT_DEATH({ f2.wait(); }, "uninitialized");
  f1.get();
}
#endif

TEST(FutureTests, SelfMovingIsSafe) {
  constexpr int val{42};
  auto* const task{allocateTask<int>()};
  completeWithValue<int>(task, val);
  Future<int> f{task};
  Future<int>& fRef{f};
  f = std::move(fRef);
  EXPECT_EQ(f.get(), val);
}

TEST(FutureTests, DtorRetiresTaskWithoutCallingGet) {
  constexpr int val{42};
  auto* const task{allocateTask<int>()};
  completeWithValue<int>(task, val);
  { Future<int> f{task}; }
  SUCCEED();
}

TEST(FutureTests, DroppingFutureBeforeTaskCompletesDoesNotDeallocate) {
  auto* const task{allocateTask<int>()};

  { Future<int> f{task}; }

  // refCount_: 2 -> 1 after the future retires; the pool's hold remains, so the
  // node should not have been deallocated yet
  EXPECT_EQ(task->refCount_.load(MemoryOrder::relaxed), 1);
  EXPECT_TRUE(task->releaseReference());
  Internal::NodeAllocator<Internal::Task<int>>::deallocate(task);
}

TEST(FutureTests, FuturesUnderReuse) {
  // Exercises allocate/complete/get/deallocate repeatedly to help surface any
  // node-recycling or reference-counting bugs via the NodeAllocator free list
  constexpr int nIterations{2'000};

  for (int i{0}; i < nIterations; ++i) {
    auto* const task{allocateTask<int>()};
    completeWithValue<int>(task, i);
    Future<int> f{task};
    EXPECT_EQ(f.get(), i);
  }
}

TEST(FutureTests, ConcurretFuturesSeeTheirOwnValue) {
  constexpr int nThreads{8};
  constexpr int nPerThread{500};
  std::vector<std::jthread> threads{};
  threads.reserve(nThreads);

  for (int t{0}; t < nThreads; ++t) {
    threads.emplace_back([&, t] {
      for (int i{0}; i < nPerThread; ++i) {
        const int value{t * nPerThread + i};
        auto* const task{allocateTask<int>()};
        completeWithValue<int>(task, value);
        Future<int> f{task};
        EXPECT_EQ(f.get(), value);
      }
    });
  }
}
