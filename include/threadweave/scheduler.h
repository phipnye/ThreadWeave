#ifndef TW_SCHEDULER_H
#define TW_SCHEDULER_H

#include <algorithm>
#include <thread>
#include <vector>

namespace ThreadWeave {

class Scheduler {
  // Data members
  std::vector<std::thread> workers_;

 public:
  // Default ctor uses all available hardware resources
  Scheduler() : workers_(std::max(1u, std::thread::hardware_concurrency())) {}

  // Ctor with user-defined number of threads
  explicit Scheduler(const unsigned int nThreads)
      : workers_(std::max(1u, nThreads)) {}
};

}  // namespace ThreadWeave

#endif
