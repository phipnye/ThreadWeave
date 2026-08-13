// The idea here is to measure the effectiveness of the node allocator at
// minimizing malloc calls. Under instances where the allocator's global cache
// runs out, all threads can technically "instruct" the global cache to call
// malloc at the same time which is slow especially under contention. The
// underlying implementation tries to minimize this contention by recycling
// nodes as much as possible and allocating in chunks. Ideally, this profiling
// could should show malloc taking minimal time while the runtime is dominated
// by other tasks like thread spawning.

#include <threadweave/TreiberStack.h>

#include <barrier>
#include <thread>
#include <vector>

namespace {

constexpr int kRounds{2'000};
constexpr int kThreadsPerRound{4};
constexpr int kOpsPerThread{500};

}  // namespace

int main() {
  ThreadWeave::TreiberStack<int> stk{};
  std::vector<std::jthread> threads{};
  threads.reserve(kThreadsPerRound);

  for (int round{0}; round < kRounds; ++round) {
    std::barrier syncPoint{kThreadsPerRound};

    for (int t{0}; t < kThreadsPerRound; ++t) {
      threads.emplace_back([&, t] {
        // Every thread here is brand new, so thier thread-local node caches
        // start empty (waiting here "lines" them up to hit the global pool's
        // askForNode() at roughly the same moment)
        syncPoint.arrive_and_wait();

        for (int i{0}; i < kOpsPerThread; ++i) {
          stk.push(t * kOpsPerThread + i);
        }

        for (int i{0}; i < kOpsPerThread; ++i) {
          stk.pop();
        }
      });
    }

    threads.clear();
  }
}
