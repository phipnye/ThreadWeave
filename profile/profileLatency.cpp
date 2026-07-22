#include <benchmark/benchmark.h>
#include <threadweave/ThreadPool.h>
#include <threadweave/utils.h>

#include <thread>

#include "../cmake-build-release/_deps/bshoshany_thread_pool-src/include/BS_thread_pool.hpp"

using ThreadWeave::Future;
using ThreadWeave::Index;
using ThreadWeave::ThreadPool;

static void twSubmitTasks(const Index nThreads, const Index nIter = 10'000) {
  ThreadPool pool{nThreads};

  for (Index i{0}; i < nIter; ++i) {
    auto f{pool.submit([] { return 1; })};
    benchmark::DoNotOptimize(f.get());
  }
}

static void bsSubmitTasks(const Index nThreads, const Index nIter = 10'000) {
  BS::thread_pool pool{static_cast<std::size_t>(nThreads)};

  for (Index i{0}; i < nIter; ++i) {
    auto f{pool.submit_task([] { return 1; })};
    benchmark::DoNotOptimize(f.get());
  }
}

int main() {
  twSubmitTasks(std::thread::hardware_concurrency());
}
