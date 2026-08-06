#include <threadweave/ThreadPool.h>

#include <cstddef>
#include <iomanip>
#include <iostream>
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
  std::cout << "--- Thread Pool Execution Results ---\n";

  for (std::size_t i{0}; i < nTasks; ++i) {
    // f.get() blocks until the specific task finishes
    const int res{futures[i].get()};
    std::cout << "Task " << std::setw(3) << std::setfill('0') << (i + 1)
              << ": Fibonacci(" << std::setw(3) << std::setfill('0')
              << randSample[i] << ") = " << res << '\n';
  }
}
