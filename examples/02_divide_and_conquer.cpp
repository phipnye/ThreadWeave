// Demonstrates recursive, divide-and-conquer parallelism. A task submitted to
// the pool submits more tasks to the pool and waits on their results. Workers
// calling .get() help execute other queued work instead of blocking, and thus
// won't deadlock or starve the pool, even with a small number of threads.
//
// Build:
//   g++ -std=c++23 -I../include -pthread 02_divide_and_conquer.cpp -o
//   divide_and_conquer
// Run:
//   ./divide_and_conquer

#include <threadweave/ThreadPool.h>

#include <functional>
#include <iostream>
#include <numeric>
#include <vector>

// 64-bit signed integer
using ThreadWeave::Index;

namespace {

constexpr Index kSequentialThreshold{1'000};

// Recursively sums a range, splitting work across the pool once the range is
// large enough to be worth parallelizing.
Index parallelSum(ThreadWeave::ThreadPool& pool, const std::vector<Index>& data,
                  const Index begin, Index end) {
  if ((end - begin) <= kSequentialThreshold) {
    const auto iter{data.cbegin()};
    return std::accumulate(iter + begin, iter + end, Index{0});
  }

  // Submit the right half to the pool while this thread recurses on the left
  // half
  const Index mid{std::midpoint(begin, end)};
  auto futR{
      pool.submit(parallelSum, std::ref(pool), std::cref(data), mid, end)};
  const Index sumL{parallelSum(pool, data, begin, mid)};

  // If this call is running on a worker thread, get() executes other
  // queued tasks instead of blocking while it waits on futR
  return sumL + futR.get();
}

}  // namespace

int main() {
  ThreadWeave::ThreadPool pool{4};  // using 4 threads
  constexpr Index sz{1'000'000};
  const std::vector<Index> data(sz, 1);
  auto total{pool.submit(parallelSum, std::ref(pool), std::cref(data), 0, sz)};
  std::cout << "Sum = " << total.get() << '\n';
}
