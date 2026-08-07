// The simplest possible use of ThreadWeave: create a pool, submit a few
// independent tasks, and collect their results.
//
// Build:
//   g++ -std=c++23 -Iinclude -pthread 01_basic_tasks.cpp -o basic_tasks
// Run:
//   ./basic_tasks

#include <threadweave/ThreadPool.h>

#include <iostream>
#include <vector>

namespace {

int square(const int n) {
  return n * n;
}

}  // namespace

int main() {
  if (std::thread::hardware_concurrency() == 0) {
    std::cout << "The number of threads on this hardware is not well defined "
                 "or not computable. Please specify the number of threads to "
                 "use for the pool manually.\n";
    return 0;
  }

  // Defaults to std::thread::hardware_concurrency() workers
  ThreadWeave::ThreadPool pool{};

  // submit() returns a Future<T> immediately (the task runs asynchronously)
  constexpr int nIter{10};
  std::vector<ThreadWeave::Future<int>> futures{};
  futures.reserve(nIter);

  for (int i{0}; i < nIter; ++i) {
    // subit takes for the form: submit(function, args...)
    futures.push_back(pool.submit(square, i));
  }

  // future.get() blocks (since the calling thread here is not a worker) until
  // the result is ready before returning results
  for (int i{0}; i < nIter; ++i) {
    std::cout << "square(" << i << ") = " << futures[i].get() << '\n';
  }
}
