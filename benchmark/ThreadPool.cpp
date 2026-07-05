#include <benchmark/benchmark.h>
#include <threadweave/ThreadPool.h>

#include <cstddef>
#include <future>
#include <random>
#include <vector>

namespace {

constexpr int minFib{1};
constexpr int maxFib{25};
constexpr std::size_t nTasks{100};

int naiveFib(const int n) {
  return (n < 2) ? n : (naiveFib(n - 1) + naiveFib(n - 2));
}

// Generate a fixed random sample once so all benchmark runs are comparable
std::vector<int> makeRandSample() {
  std::vector<int> sample{};
  sample.reserve(nTasks);
  std::mt19937 rng{std::random_device{}()};
  std::uniform_int_distribution<int> dist{minFib, maxFib};

  for (std::size_t i{0}; i < nTasks; ++i) {
    sample.push_back(dist(rng));
  }

  return sample;
}

const std::vector<int> randSample{makeRandSample()};

}  // namespace

static void ThreadWeavePoolBM(benchmark::State& state) {
  ThreadWeave::ThreadPool pool{};
  std::vector<std::future<int>> futures{};
  futures.reserve(nTasks);
  std::vector<int> results{};
  results.reserve(nTasks);

  for (auto _ : state) {
    state.PauseTiming();

    // Remove any pre-existing data
    futures.clear();
    results.clear();

    state.ResumeTiming();

    // Sumbit tasks
    for (std::size_t i{0}; i < nTasks; ++i) {
      futures.emplace_back(pool.emplace(naiveFib, randSample[i]));
    }

    // Gather results
    for (auto& f : futures) {
      results.push_back(f.get());
    }

    // Prevent elision of results
    benchmark::DoNotOptimize(results);
  }
}

BENCHMARK(ThreadWeavePoolBM);
BENCHMARK_MAIN();
