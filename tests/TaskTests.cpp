#include <gtest/gtest.h>
#include <threadweave/internal/Task.h>
#include <threadweave/internal/utils.h>

#include <atomic>
#include <chrono>
#include <cstddef>
#include <exception>
#include <new>
#include <stdexcept>
#include <thread>

template <typename T>
using Task = ThreadWeave::Internal::Task<T>;

struct DestructorCounter {
  static inline int dtorCnt{0};
  ~DestructorCounter() {
    ++dtorCnt;
  }
};

TEST(TaskTests, ResetDestroysObject) {
  DestructorCounter::dtorCnt = 0;
  Task<DestructorCounter> task{};
  new (task.resultStorage_) DestructorCounter{};
  task.hasResult_ = true;
  task.reset();
  EXPECT_EQ(DestructorCounter::dtorCnt, 1);
  EXPECT_FALSE(task.hasResult_);
}

TEST(TaskTests, ResetRestoresInitialState) {
  Task<int> task{};
  task.exception_ = std::make_exception_ptr(std::runtime_error{"error"});
  task.execute_ = [](ThreadWeave::Internal::TaskBase*) {};
  task.releaseReference();
  task.reset();
  EXPECT_EQ(task.exception_, nullptr);
  EXPECT_EQ(task.execute_, nullptr);
  EXPECT_FALSE(task.isReady());
  EXPECT_EQ(task.refCount_.load(ThreadWeave::MemoryOrder::relaxed), 2);
}

TEST(TaskTests, ReleaseReferenceSignalsZero) {
  Task<int> task{};
  EXPECT_FALSE(task.releaseReference());
  EXPECT_EQ(task.refCount_.load(ThreadWeave::MemoryOrder::relaxed), 1);
  EXPECT_TRUE(task.releaseReference());
  EXPECT_EQ(task.refCount_.load(ThreadWeave::MemoryOrder::relaxed), 0);
}

TEST(TaskTests, WaitBlocksUntilNotify) {
  Task<int> task{};
  constexpr int val{42};

  std::thread worker{[&task] {
    std::this_thread::sleep_for(std::chrono::milliseconds{10});
    new (task.resultStorage_) int{val};
    task.hasResult_ = true;
    task.notify();
  }};

  task.wait();
  EXPECT_TRUE(task.isReady());
  EXPECT_TRUE(task.hasResult_);
  EXPECT_EQ(*std::launder(reinterpret_cast<int*>(task.resultStorage_)), val);
  worker.join();
}

TEST(TaskTests, ConcurrentReleaseReferenceReturnsTrueOnce) {
  auto* const task{new Task<int>{}};
  std::atomic<int> trueCount{0};

  auto worker{[&] {
    if (task->releaseReference()) {
      trueCount.fetch_add(1, ThreadWeave::MemoryOrder::relaxed);
    }
  }};

  std::thread t1{worker};
  std::thread t2{worker};
  t1.join();
  t2.join();
  EXPECT_EQ(trueCount.load(ThreadWeave::MemoryOrder::relaxed), 1);
  delete task;
}

TEST(TaskTests, DestructorCleansResultWithoutReset) {
  DestructorCounter::dtorCnt = 0;

  {
    Task<DestructorCounter> task{};
    new (task.resultStorage_) DestructorCounter{};
    task.hasResult_ = true;
  }

  EXPECT_EQ(DestructorCounter::dtorCnt, 1);
}

TEST(TaskTests, VoidTaskSupport) {
  Task<void> task{};
  task.hasResult_ = true;
  task.reset();
  EXPECT_FALSE(task.hasResult_);
}

TEST(TaskTests, WaitReturnsImmediatelyIfReady) {
  // Ensures wait doesn't block or hit state.wait() if already in ready state
  Task<int> task{};
  task.notify();
  task.wait();
  EXPECT_TRUE(task.isReady());
}

TEST(TaskTests, DestructorSkippedWhenHasResultIsFalse) {
  DestructorCounter::dtorCnt = 0;

  {
    Task<DestructorCounter> task{};
    task.hasResult_ = false;
  }

  EXPECT_EQ(DestructorCounter::dtorCnt, 0);
}

TEST(TaskTests, StorageAlignmentMatchesConstraints) {
  Task<double> task{};
  const auto callableAddr{
      reinterpret_cast<std::uintptr_t>(task.callableStorage_)};
  const auto resultAddr{reinterpret_cast<std::uintptr_t>(task.resultStorage_)};
  EXPECT_EQ(callableAddr % alignof(std::max_align_t), 0u);
  EXPECT_EQ(resultAddr % alignof(double), 0u);
}

TEST(TaskTests, ExecuteFunctionPointerInvocation) {
  Task<int> task{};
  constexpr int val{42};

  task.execute_ = [](ThreadWeave::Internal::TaskBase* base) {
    auto* const self{static_cast<Task<int>*>(base)};
    new (self->resultStorage_) int{val};
    self->hasResult_ = true;
    self->notify();
  };

  task.execute_(&task);
  EXPECT_TRUE(task.isReady());
  EXPECT_TRUE(task.hasResult_);
  EXPECT_EQ(*std::launder(reinterpret_cast<int*>(task.resultStorage_)), val);
}

TEST(TaskTests, ExceptionPropagation) {
  Task<int> task{};

  try {
    throw std::runtime_error{"Test error"};
  } catch (...) {
    task.exception_ = std::current_exception();
  }

  task.notify();
  EXPECT_TRUE(task.isReady());
  ASSERT_NE(task.exception_, nullptr);
  EXPECT_THROW(std::rethrow_exception(task.exception_), std::runtime_error);
}
