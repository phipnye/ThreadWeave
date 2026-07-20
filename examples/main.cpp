#include <threadweave/ThreadPool.h>

#include <cstddef>
#include <print>
#include <random>
#include <vector>

namespace {

// Adjusted parameters for a simple example run
constexpr std::size_t nTasks{100};
constexpr int minFib{1};
constexpr int maxFib{25};

// Intentionally slow implementation for simulating tasks
int naiveFib(const int n) {
  if (n < 2) {
    return n;
  }

  return naiveFib(n - 1) + naiveFib(n - 2);
}

}  // namespace

int main() {
  // Generate random number for simulating computation times
  std::vector<int> randSample{};
  randSample.reserve(nTasks);
  std::mt19937 rng{std::random_device{}()};
  std::uniform_int_distribution<int> dist{minFib, maxFib};

  for (std::size_t _{0}; _ < nTasks; ++_) {
    randSample.push_back(dist(rng));
  }

  // Setup thread pool and futures
  ThreadWeave::ThreadPool pool{};
  std::vector<ThreadWeave::Future<int>> futures{};
  futures.reserve(nTasks);

  // Submit tasks to the thread pool
  for (std::size_t i{0}; i < nTasks; ++i) {
    futures.emplace_back(pool.submit(naiveFib, randSample[i]));
  }

  // Capture and demonstrate results
  std::println("--- Thread Pool Execution Results ---");

  for (std::size_t i{0}; i < nTasks; ++i) {
    // f.get() blocks until the specific task finishes
    int result = futures[i].get();
    std::println("Task {:03d}: Fibonacci({:03d}) = {}", i + 1, randSample[i],
                 result);
  }
}
