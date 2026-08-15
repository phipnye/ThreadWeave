#include <gtest/gtest.h>
#include <threadweave/internal/Hazard.h>
#include <threadweave/internal/utils.h>

#include <atomic>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>
#include <type_traits>

using namespace ThreadWeave;
using Internal::HazardGuard;
using Internal::HazardSlot;
using Internal::ThreadHazardManager;

static_assert(std::is_same_v<std::underlying_type_t<HazardSlot>, Index>);
static_assert(static_cast<Index>(HazardSlot::Stack0) == 0);
static_assert(static_cast<Index>(HazardSlot::Queue0) == 0);
static_assert(static_cast<Index>(HazardSlot::Queue1) == 1);
static_assert(static_cast<Index>(HazardSlot::Alloc2) == 2);
static_assert(static_cast<Index>(HazardSlot::COUNT) == 3);

TEST(HazardTests, ManagerCanBeConstructedAndDestructedOnAThread) {
  std::jthread worker([] { EXPECT_NO_THROW({ ThreadHazardManager mgr{}; }); });
}

TEST(HazardTests, FreshHazardPointerStartsNull) {
  std::jthread worker([] {
    EXPECT_EQ(
        Internal::getThreadHazardPointer(static_cast<Index>(HazardSlot::Alloc2))
            .load(MemoryOrder::relaxed),
        nullptr);
  });
}

TEST(HazardTests, GetPointerRoundTripsStoredValue) {
  std::jthread worker([] {
    int dummy{42};
    auto& hp{Internal::getThreadHazardPointer(
        static_cast<Index>(HazardSlot::Alloc2))};
    hp.store(&dummy, MemoryOrder::relaxed);
    EXPECT_EQ(hp.load(MemoryOrder::relaxed), &dummy);
  });
}

TEST(HazardTests, IsPointerInUseReturnsFalseForNullptr) {
  // Safe on the main thread (purely a read of the slots pool)
  EXPECT_FALSE(Internal::anyThreadsUsingPtr(nullptr));
}

TEST(HazardTests, IsPointerInUseReturnsFalseWhenNoThreadHoldsIt) {
  constexpr int dummy{42};
  EXPECT_FALSE(Internal::anyThreadsUsingPtr(&dummy));
}

TEST(HazardTests, IsPointerInUseReturnsTrueWhileCurrentThreadHoldsIt) {
  std::jthread worker([] {
    int dummy{42};
    auto& hp{Internal::getThreadHazardPointer(
        static_cast<Index>(HazardSlot::Alloc2))};
    EXPECT_FALSE(Internal::anyThreadsUsingPtr(&dummy));
    hp.store(&dummy, MemoryOrder::release);
    EXPECT_TRUE(Internal::anyThreadsUsingPtr(&dummy));
    hp.store(nullptr, MemoryOrder::release);
    EXPECT_FALSE(Internal::anyThreadsUsingPtr(&dummy));
  });
}

TEST(HazardTests, CrossThreadVisibilityOfHazardPointer) {
  int x{99};
  std::atomic<int*> src{&x};
  std::atomic<bool> acquired{false};
  std::atomic<bool> canRelease{false};

  EXPECT_FALSE(Internal::anyThreadsUsingPtr(&x));

  std::jthread holder([&] {
    const HazardGuard<HazardSlot::Alloc2> guard{};
    guard.acquirePointerWithHazard(src);
    acquired.store(true, MemoryOrder::release);

    while (!canRelease.load(MemoryOrder::acquire)) {
      std::this_thread::yield();
    }
  });

  while (!acquired.load(MemoryOrder::acquire)) {
    std::this_thread::yield();
  }
  EXPECT_TRUE(Internal::anyThreadsUsingPtr(&x));
  canRelease.store(true, MemoryOrder::release);
  holder.join();  // thread local manager destructs here, releasing the slot
  EXPECT_FALSE(Internal::anyThreadsUsingPtr(&x));
}

TEST(HazardTests, HazardGuardAcquiresMatchesAtomicAndClearsOnDestruction) {
  std::jthread worker([] {
    int dummy{42};
    std::atomic<int*> src{&dummy};
    EXPECT_FALSE(Internal::anyThreadsUsingPtr(&dummy));

    {
      HazardGuard<HazardSlot::Alloc2> guard{};
      int* const acquired{guard.acquirePointerWithHazard(src)};
      EXPECT_EQ(acquired, &dummy);
      EXPECT_TRUE(Internal::anyThreadsUsingPtr(&dummy));
    }

    EXPECT_FALSE(Internal::anyThreadsUsingPtr(&dummy));
  });
}

TEST(HazardTests, MultipleHazardSlotsAreIndependentOnSameThread) {
  std::jthread worker([] {
    int x{1};
    int y{2};
    std::atomic<int*> srcX{&x};
    std::atomic<int*> srcY{&y};
    const HazardGuard<HazardSlot::Queue0> guard0{};
    const HazardGuard<HazardSlot::Queue1> guard1{};
    EXPECT_EQ(guard0.acquirePointerWithHazard(srcX), &x);
    EXPECT_EQ(guard1.acquirePointerWithHazard(srcY), &y);
    EXPECT_TRUE(Internal::anyThreadsUsingPtr(&x));
    EXPECT_TRUE(Internal::anyThreadsUsingPtr(&y));
  });
}

TEST(HazardTests, Stack0AndQueue0ShareTheSameUnderlyingSlot) {
  // Stack0 and Queue0 both map to hazard index 0 by design (a thread is
  // not expected to use both at the same time). Acquiring through the Queue0
  // guard overwrites the same underlying slot the Stack0 guard just wrote to.
  std::jthread worker([] {
    int a{1};
    int b{2};
    std::atomic<int*> srcA{&a};
    std::atomic<int*> srcB{&b};
    const HazardGuard<HazardSlot::Stack0> stackGuard{};
    EXPECT_EQ(stackGuard.acquirePointerWithHazard(srcA), &a);
    EXPECT_TRUE(Internal::anyThreadsUsingPtr(&a));
    const HazardGuard<HazardSlot::Queue0> queueGuard{};
    EXPECT_EQ(queueGuard.acquirePointerWithHazard(srcB), &b);
    EXPECT_FALSE(Internal::anyThreadsUsingPtr(&a));
    EXPECT_TRUE(Internal::anyThreadsUsingPtr(&b));
  });
}

TEST(HazardTests, ExhaustingHazardSlotsThrowsRuntimeError) {
  std::jthread worker([] {
    std::vector<std::unique_ptr<ThreadHazardManager>> managers{};
    managers.reserve(Internal::kMaxThreads);

    for (Index i{0}; i < Internal::kMaxThreads; ++i) {
      EXPECT_NO_THROW(
          managers.push_back(std::make_unique<ThreadHazardManager>()));
    }

    try {
      ThreadHazardManager extra{};
      FAIL() << "Expected construction to throw once the pool is exhausted";
    } catch (const std::runtime_error& e) {
      const std::string errMsg{e.what()};
      EXPECT_NE(errMsg.find("No available hazard pointers"), std::string::npos);
    }
  });
}

TEST(HazardTests, AcquirePointerWithHazardUnderConcurrentMutation) {
  std::jthread worker([] {
    int a{1};
    int b{2};
    std::atomic<int*> src{&a};
    std::atomic<bool> stop{false};

    std::jthread mutator([&] {
      while (!stop.load(MemoryOrder::acquire)) {
        src.store(&a, MemoryOrder::relaxed);
        src.store(&b, MemoryOrder::relaxed);
      }
    });

    constexpr int nIterations{50'000};

    for (int i{0}; i < nIterations; ++i) {
      const HazardGuard<HazardSlot::Alloc2> guard{};
      const int* const acquired{guard.acquirePointerWithHazard(src)};
      EXPECT_TRUE(acquired == &a || acquired == &b);
    }

    stop.store(true, MemoryOrder::release);
  });
}

#ifndef TW_NDEBUG
TEST(HazardTests, GetThreadHazardPointerAssertsOnOutOfBoundsIndex) {
  EXPECT_DEATH({ Internal::getThreadHazardPointer(100); }, "out of bounds");
}

TEST(HazardTests, GetThreadHazardPointerAssertsAtExactCountBoundary) {
  EXPECT_DEATH(
      {
        Internal::getThreadHazardPointer(static_cast<Index>(HazardSlot::COUNT));
      },
      "out of bounds");
}
#endif
