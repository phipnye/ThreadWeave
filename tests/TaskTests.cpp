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

using namespace ThreadWeave;

struct DestructionCounter {
  static inline std::atomic<int> liveCount{0};
  static inline std::atomic<int> destructCount{0};
  int value{0};

  explicit DestructionCounter(const int val = 0) : value{val} {
    liveCount.fetch_add(1, MemoryOrder::relaxed);
  }

  DestructionCounter(const DestructionCounter& other) : value{other.value} {
    liveCount.fetch_add(1, MemoryOrder::relaxed);
  }

  ~DestructionCounter() {
    liveCount.fetch_sub(1, MemoryOrder::relaxed);
    destructCount.fetch_add(1, MemoryOrder::relaxed);
  }

  static void reset() {
    liveCount.store(0, MemoryOrder::relaxed);
    destructCount.store(0, MemoryOrder::relaxed);
  }
};

Internal::TaskBase* g_lastExecutedWith{nullptr};
void recordExecution(Internal::TaskBase* self) {
  g_lastExecutedWith = self;
}

TEST(TaskTests, DefaultStateIsPending) {
  const Internal::Task<int> t{};
  EXPECT_FALSE(t.isReady());
  EXPECT_EQ(t.refCount_.load(MemoryOrder::relaxed), 2);
  EXPECT_EQ(t.execute_, nullptr);
}

TEST(TaskTests, NotifyMarksTaskReady) {
  Internal::Task<int> t{};
  EXPECT_FALSE(t.isReady());
  t.notify();
  EXPECT_TRUE(t.isReady());
}

TEST(TaskTests, DoubleNotifyIsSafe) {
  Internal::Task<int> t{};
  t.notify();
  EXPECT_TRUE(t.isReady());
  t.notify();  // second call should be safe
  EXPECT_TRUE(t.isReady());
}

TEST(TaskTests, ReadyWaitReturnsImmediately) {
  Internal::Task<int> t{};
  t.notify();
  t.wait();  // must not block
  EXPECT_TRUE(t.isReady());
}

TEST(TaskTests, WaitBlocksUntilNotify) {
  Internal::Task<int> t{};
  std::atomic<bool> waitReturned{false};

  std::jthread waiter([&] {
    t.wait();
    waitReturned.store(true, MemoryOrder::release);
  });

  std::this_thread::sleep_for(std::chrono::milliseconds{20});
  EXPECT_FALSE(waitReturned.load(MemoryOrder::acquire));
  t.notify();
  waiter.join();
  EXPECT_TRUE(waitReturned.load(MemoryOrder::acquire));
}

TEST(TaskTests, MultipleWaitersAllUnblockOnNotify) {
  constexpr int nWaiters{8};
  Internal::Task<int> t{};
  std::atomic<int> returnedCount{0};
  std::vector<std::jthread> waiters{};
  waiters.reserve(nWaiters);

  for (int i{0}; i < nWaiters; ++i) {
    waiters.emplace_back([&] {
      t.wait();
      returnedCount.fetch_add(1, MemoryOrder::relaxed);
    });
  }

  std::this_thread::sleep_for(std::chrono::milliseconds{20});
  t.notify();
  waiters.clear();
  EXPECT_EQ(returnedCount.load(MemoryOrder::relaxed), nWaiters);
}

TEST(TaskTests, ReleaseReferenceWorks) {
  Internal::Task<int> t{};
  EXPECT_EQ(t.refCount_.load(MemoryOrder::relaxed), 2);
  EXPECT_FALSE(t.releaseReference());  // 2 -> 1, not last holder
  EXPECT_EQ(t.refCount_.load(MemoryOrder::relaxed), 1);
  EXPECT_TRUE(t.releaseReference());  // 1 -> 0, last holder
  EXPECT_EQ(t.refCount_.load(MemoryOrder::relaxed), 0);
}

TEST(TaskTests, ReleaseReferenceHasExactlyOneLastHolder) {
  constexpr int nExtraHolders{6};
  Internal::Task<int> t{};
  t.refCount_.store(nExtraHolders + 1, MemoryOrder::relaxed);
  std::atomic<int> lastHolderCount{0};
  std::vector<std::jthread> holders{};
  holders.reserve(nExtraHolders + 1);

  for (int i{0}; i < nExtraHolders + 1; ++i) {
    holders.emplace_back([&] {
      if (t.releaseReference()) {
        lastHolderCount.fetch_add(1, MemoryOrder::relaxed);
      }
    });
  }

  holders.clear();
  EXPECT_EQ(lastHolderCount.load(MemoryOrder::relaxed), 1);
  EXPECT_EQ(t.refCount_.load(MemoryOrder::relaxed), 0);
}

#ifndef TW_NDEBUG
TEST(TaskTests, DoubleReleaseTriggersAssertion) {
  Internal::Task<int> t{};
  EXPECT_FALSE(t.releaseReference());
  EXPECT_TRUE(t.releaseReference());
  EXPECT_DEATH({ t.releaseReference(); }, "Double-release detected");
}
#endif

TEST(TaskTests, DefaultConstructedTaskHasExpectedState) {
  const Internal::Task<int> t{};
  EXPECT_FALSE(t.hasResult_);
  EXPECT_EQ(t.execute_, nullptr);
  EXPECT_EQ(t.exception_, nullptr);
  EXPECT_FALSE(t.isReady());
  EXPECT_EQ(t.refCount_.load(MemoryOrder::relaxed), 2);
}

TEST(TaskTests, ExecuteIsInvokedWithTaskPointer) {
  Internal::Task<int> t{};
  g_lastExecutedWith = nullptr;
  t.execute_ = &recordExecution;
  ASSERT_NE(t.execute_, nullptr);
  t.execute_(&t);
  EXPECT_EQ(g_lastExecutedWith, static_cast<Internal::TaskBase*>(&t));
}

TEST(TaskTests, DtorDestroysStoredNonTrivialResult) {
  DestructionCounter::reset();

  {
    static_assert(!std::is_trivially_destructible_v<DestructionCounter>,
                  "DestructionCounter cannot be trivially destructible for "
                  "this test to work properly.");
    Internal::Task<DestructionCounter> t{};
    ::new (static_cast<void*>(t.resultStorage_)) DestructionCounter{};
    t.hasResult_ = true;
    EXPECT_EQ(DestructionCounter::liveCount.load(MemoryOrder::relaxed), 1);
  }  // dtor should call destroyResults() for non - trivial type

  EXPECT_EQ(DestructionCounter::liveCount.load(MemoryOrder::relaxed), 0);
  EXPECT_EQ(DestructionCounter::destructCount.load(MemoryOrder::relaxed), 1);
}

TEST(TaskTests, DtorSkipsDestructionWhenNoResult) {
  DestructionCounter::reset();

  {
    const Internal::Task<DestructionCounter> t{};
    EXPECT_FALSE(t.hasResult_);
  }

  EXPECT_EQ(DestructionCounter::destructCount.load(MemoryOrder::relaxed), 0);
}

TEST(TaskTests, ResetClearsDataProperly) {
  Internal::Task<int> t{};
  t.execute_ = &recordExecution;
  t.exception_ = std::make_exception_ptr(std::runtime_error{"Error"});
  t.releaseReference();  // refCount_: 2 -> 1
  t.notify();            // state_: pending -> ready
  ASSERT_NE(t.execute_, nullptr);
  ASSERT_NE(t.exception_, nullptr);
  ASSERT_EQ(t.refCount_.load(MemoryOrder::relaxed), 1);
  ASSERT_TRUE(t.isReady());
  t.reset();
  EXPECT_EQ(t.execute_, nullptr);
  EXPECT_EQ(t.exception_, nullptr);
  EXPECT_EQ(t.refCount_.load(MemoryOrder::relaxed), 2);
  EXPECT_FALSE(t.isReady());
  EXPECT_FALSE(t.hasResult_);
}

TEST(TaskTests, DuplicateResetsAreSafe) {
  Internal::Task<int> t{};
  t.reset();
  t.reset();
  EXPECT_FALSE(t.hasResult_);
  EXPECT_FALSE(t.isReady());
}

TEST(TaskTests, TriviallyDestructibleDoesNotCallDtor) {
  DestructionCounter::reset();
  Internal::Task<int> t{};
  ::new (static_cast<void*>(t.resultStorage_)) int{};
  t.hasResult_ = true;
  t.reset();
  EXPECT_FALSE(t.hasResult_);
  EXPECT_EQ(DestructionCounter::destructCount.load(MemoryOrder::relaxed), 0);
}

TEST(TaskTests, VoidTask) {
  Internal::Task<void> t{};
  EXPECT_FALSE(t.hasResult_);
  t.hasResult_ = true;
  t.reset();
  EXPECT_FALSE(t.hasResult_);
  EXPECT_FALSE(t.isReady());
}

TEST(TaskTests, MultipleTasksAreIndependent) {
  Internal::Task<int> a{};
  Internal::Task<int> b{};
  a.notify();
  EXPECT_TRUE(a.isReady());
  EXPECT_FALSE(b.isReady());
  a.releaseReference();
  EXPECT_EQ(a.refCount_.load(MemoryOrder::relaxed), 1);
  EXPECT_EQ(b.refCount_.load(MemoryOrder::relaxed), 2);
}
