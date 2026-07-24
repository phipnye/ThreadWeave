#include <benchmark/benchmark.h>
#include <threadweave/ThreadPool.h>
#include <threadweave/utils.h>

#include <thread>

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

int main() {
  twSubmitTasks(std::thread::hardware_concurrency());
}
