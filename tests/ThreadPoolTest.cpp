#include <gtest/gtest.h>
#include <threadweave/Future.h>
#include <threadweave/ThreadPool.h>
#include <threadweave/utils.h>

#include <bitset>
#include <chrono>
#include <exception>
#include <future>
#include <stdexcept>
#include <type_traits>
#include <vector>

using ThreadWeave::ThreadPool;
namespace MemoryOrder = ThreadWeave::MemoryOrder;

template <typename T>
using Future = ThreadWeave::Future<T>;

// Try submitting just a single simple task
TEST(ThreadPoolTest, SingleTask) {
  ThreadPool pool{1};
  constexpr int expectedVal{42};
  Future<int> f{pool.submit([] { return expectedVal; })};
  EXPECT_EQ(expectedVal, f.get());
}

// Try submitting multiple simple tasks
TEST(ThreadPoolTest, MultipleTasks) {
  constexpr int nTasks{1'000};
  ThreadPool pool{1};
  std::vector<Future<int>> futures{};
  futures.reserve(nTasks);

  // Submit a bunch of tasks that return just the iteration number
  for (int i{0}; i < nTasks; ++i) {
    futures.push_back(pool.submit([i] { return i; }));
  }

  // Make sure we recover every task
  std::bitset<nTasks> seen{};

  for (auto& f : futures) {
    const int val{f.get()};
    EXPECT_GE(val, 0);
    EXPECT_LT(val, nTasks);
    EXPECT_FALSE(seen.test(val));
    seen.set(val);
  }

  EXPECT_TRUE(seen.all());
}

// Verify pool works with one worker
TEST(ThreadPoolTest, SingleWorkerEdgeCase) {
  ThreadPool pool{1};
  constexpr int nTasks{100};
  std::vector<Future<int>> futures{};
  futures.reserve(nTasks);

  for (int i{0}; i < nTasks; ++i) {
    futures.push_back(pool.submit([i] { return i * 2; }));
  }

  for (int i{0}; i < nTasks; ++i) {
    EXPECT_EQ(futures[i].get(), i * 2);
  }
}

// Make sure pool preserves exceptions
TEST(ThreadPoolTest, PreservesException) {
  ThreadPool pool{2};
  auto f{pool.submit([] {
    throw std::runtime_error{"Error"};
    return 2;  // won't reach but prevents future<void>
  })};
  EXPECT_THROW(f.get(), std::runtime_error);
}

// Verify exceptions do not kill workers
TEST(ThreadPoolTest, WorkersSurviveExceptions) {
  constexpr int nTasks{100};
  ThreadPool pool{4};
  std::vector<Future<void>> failures{};

  for (int i{0}; i < nTasks; ++i) {
    failures.push_back(pool.submit([] { throw std::runtime_error{"Error"}; }));
  }

  for (auto& f : failures) {
    EXPECT_THROW(f.get(), std::runtime_error);
  }

  auto f{pool.submit([] { return 42; })};
  EXPECT_EQ(f.get(), 42);
}

// Make sure tasks finish in destructor and that run longer do not prevent other
// tasks from completing
TEST(ThreadPoolTest, HandlesMixedTaskDurations) {
  constexpr int nTasks{1'000};
  std::atomic<int> completed{0};

  {
    ThreadPool pool{4};

    for (int i{0}; i < nTasks; ++i) {
      pool.submit([&, i] {
        if (i % 10 == 0) {
          std::this_thread::yield();
        }

        completed.fetch_add(1, MemoryOrder::relaxed);
      });
    }
  }

  EXPECT_EQ(completed.load(MemoryOrder::relaxed), nTasks);
}

// Make sure multiple threads can submit work concurrently
TEST(ThreadPoolTest, ConcurrentSubmissions) {
  constexpr int nSubmitters{8};
  constexpr int tasksPerSubmitter{1'000};
  ThreadPool pool{};
  std::vector<std::jthread> submitters{};
  std::vector<std::vector<Future<int>>> futures(nSubmitters);

  for (int t{0}; t < nSubmitters; ++t) {
    submitters.emplace_back([&, t] {
      auto& local{futures[t]};
      local.reserve(tasksPerSubmitter);

      for (int i{0}; i < tasksPerSubmitter; ++i) {
        local.push_back(
            pool.submit([id = t * tasksPerSubmitter + i] { return id; }));
      }
    });
  }

  submitters.clear();
  constexpr int nTasks{nSubmitters * tasksPerSubmitter};
  std::bitset<nTasks> seen{};

  for (auto& local : futures) {
    for (auto& f : local) {
      const int id{f.get()};
      EXPECT_GE(id, 0);
      EXPECT_LT(id, nTasks);
      EXPECT_FALSE(seen.test(id));
      seen.set(id);
    }
  }

  EXPECT_TRUE(seen.all());
}

// Make sure tasks run concurrently on different worker threads
TEST(ThreadPoolTest, RunsTasksConcurrently) {
  constexpr int nTasks{100'000};
  constexpr ThreadWeave::Index nThreads{8};
  std::atomic<int> uniqueThreadCount{0};

  {
    ThreadPool pool{nThreads};

    for (int i{0}; i < nTasks; ++i) {
      pool.submit([&uniqueThreadCount] {
        thread_local bool seen{false};

        if (!seen) {
          seen = true;
          uniqueThreadCount.fetch_add(1, MemoryOrder::relaxed);
        }
      });
    }
  }

  // Not necessarily guaranteed but should be the case with a sufficient number
  // of tasks
  EXPECT_EQ(uniqueThreadCount.load(MemoryOrder::relaxed), nThreads);
}

// Make sure exceptions from one task don't break other tasks
TEST(ThreadPoolTest, ContinuesAfterException) {
  ThreadPool pool{2};

  auto bad{pool.submit([] {
    throw std::runtime_error{"failure"};
    return 0;
  })};

  auto good{pool.submit([] { return 42; })};
  EXPECT_THROW(bad.get(), std::runtime_error);
  EXPECT_EQ(good.get(), 42);
}

// Make sure submitting many tasks doesn't lose any
TEST(ThreadPoolTest, HandlesManyTasks) {
  constexpr int nTasks{100'000};
  std::atomic<int> completed{0};

  {
    ThreadPool pool{};
    for (int i{0}; i < nTasks; ++i) {
      pool.submit(
          [&completed] { completed.fetch_add(1, MemoryOrder::relaxed); });
    }
  }

  EXPECT_EQ(completed.load(MemoryOrder::relaxed), nTasks);
}

// Make sure pool can handle recursive submissions
TEST(ThreadPoolTest, HandlesNestedTaskSubmission) {
  // Must have a sufficient number of threads to perform tasks recursively
  ThreadPool pool{8};

  // Parallel version of naive fibonacci
  auto parallelFib{[&pool](this auto self, int n) {
    if (n <= 1) {
      return pool.submit([] { return 1; });
    }

    // Submit nested work from inside a worker thread
    return pool.submit([self, n] {
      auto lhs{self(n - 1)};
      auto rhs{self(n - 2)};
      return lhs.get() + rhs.get();
    });
  }};

  auto result{parallelFib(5)};
  EXPECT_EQ(result.get(), 8);  // fib(5) = 8
}

// Make sure idle threads steal tasks from working threads
// TEST(ThreadPoolTest, VerificationOfWorkStealing) {
//   ThreadPool pool{2};
//   std::atomic<bool> blockWorker1{true};
//   std::atomic<bool> stealOccurred{false};
//
//   // Submit a long-running task to pin one thread
//   pool.submit([&] {
//     while (blockWorker1.load(MemoryOrder::acquire)) {
//       std::this_thread::yield();
//     }
//   });
//
//   // Make sure first thread is actively running task
//   std::this_thread::sleep_for(std::chrono::milliseconds(10));
//
//   // Submit another task. If Worker A is blocked, Worker B must steal this
//   task
//   // to complete it
//   auto f{pool.submit(
//       [&] { stealOccurred.store(true, MemoryOrder::release); })};
//
//   // Wait for the stolen task to complete
//   const std::future_status status{f.wait_for(std::chrono::seconds{2})};
//
//   // Unblock thread
//   blockWorker1.store(false, MemoryOrder::release);
//   EXPECT_EQ(status, std::future_status::ready);
//   EXPECT_TRUE(stealOccurred.load(MemoryOrder::acquire));
// }

// Ensure proper behavior upon rapid building and destroying of pools
TEST(ThreadPoolTest, HighFrequencyLifecycleChurn) {
  constexpr int nIterations{100};

  for (int i{0}; i < nIterations; ++i) {
    constexpr int nTasks{50};
    ThreadPool pool{8};
    std::vector<Future<int>> futures{};
    futures.reserve(nTasks);

    for (int j{0}; j < nTasks; ++j) {
      futures.push_back(pool.submit([j] { return j; }));
    }

    std::bitset<nTasks> seen{};

    for (auto& f : futures) {
      const int val{f.get()};
      EXPECT_GE(val, 0);
      EXPECT_LT(val, nTasks);
      EXPECT_FALSE(seen.test(val));
      seen.set(val);
    }

    EXPECT_TRUE(seen.all());
  }
}
