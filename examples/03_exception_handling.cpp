// If a task throws, the exception is captured on the worker thread and
// re-thrown from Future::get() on whichever thread calls it, so normal
// try/catch works across the async boundary.
//
// Build:
//   g++ -std=c++23 -Iinclude -pthread 03_exception_handling.cpp -o
//   exception_handling
// Run:
//   ./exception_handling

#include <threadweave/ThreadPool.h>

#include <iostream>
#include <stdexcept>

namespace {

int divide(const int num, const int den) {
  if (den == 0) {
    throw std::invalid_argument{"division by zero"};
  }

  return num / den;
}

}  // namespace

int main() {
  ThreadWeave::ThreadPool pool{};
  auto goodFuture{pool.submit(divide, 10, 2)};
  auto badFuture{pool.submit(divide, 10, 0)};
  std::cout << "10 / 2 = " << goodFuture.get() << '\n';

  try {
    badFuture.get();
  } catch (const std::invalid_argument& e) {
    std::cout << "Caught expected exception: " << e.what() << '\n';
  }
}
