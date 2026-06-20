#include <threadweave/pool.h>

#include <future>
#include <iostream>
#include <vector>

int main() {
  ThreadWeave::Pool pool{};
  std::vector<std::future<int>> futures{};

  for (int i{0}; i < 100; ++i) {
    futures.emplace_back(pool.emplace([i] { return i * i; }));
  }

  for (auto& f : futures) {
    std::cout << f.get() << '\n';
  }
}