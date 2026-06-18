#include <threadweave/scheduler.h>
#include <iostream>
#include <future>
#include <vector>

int main() {
  threadweave::Scheduler s{};
  std::vector<std::future<int>> futures{};

  for (int i{0}; i < 100; ++i) {
    futures.emplace_back(s.enqueue([i] { return i * i; }));
  }

  for (const auto& f : futures) {
    std::cout << f.get() << '\n';
  }
}