#include <threadweave/ThreadPool.h>
#include <threadweave/VyukovQueue.h>
#include <threadweave/utils.h>

#include <atomic>
#include <cassert>
#include <iostream>
#include <memory>
#include <vector>

namespace ThreadWeave {

ThreadPool::ThreadPool(const Index nThreads)
    : internalDeques_{
          std::make_unique<ChaseLevDeque<FutureNodeBase*>[]>(nThreads)},
      externalQueues_{
          std::make_unique<VyukovQueue<FutureNodeBase*>[]>(nThreads)},
      workers_{},
      nThreads_{nThreads},
      parkDetails_(nThreads),
      nFinished_{0},
      externalId_{0},
      stop_{false} {
  // Fill pool with worker threads
  workers_.reserve(nThreads);

  try {
    for (Index threadId{0}; threadId < nThreads; ++threadId) {
      workers_.emplace_back(&ThreadPool::workerLoop, this, threadId);
    }
  } catch (...) {
    // In case spawning a thread throws, clean up right away
    stop_.store(true, MemoryOrder::release);

    // Account for workers that were never spawned so created workers can break
    const Index sz{static_cast<Index>(workers_.size())};
    nFinished_.fetch_add(nThreads_ - sz, MemoryOrder::release);

    for (Index threadId{0}; threadId < sz; ++threadId) {
      tryUnparkThread(threadId);
    }

    for (auto& t : workers_) {
      if (t.joinable()) {
        t.join();
      }
    }

    throw;
  }
}

ThreadPool::~ThreadPool() {
  // Indicate to the threads to stop
  stop_.store(true, MemoryOrder::release);

  // Awaken every thread
  for (Index threadId{0}; threadId < nThreads_; ++threadId) {
    tryUnparkThread(threadId);
  }

  // Join all of the workers
  for (auto& t : workers_) {
    t.join();
  }
}

void ThreadPool::workerLoop(const Index threadId) {
  // Store information related to the worker so that it can write to its own
  // deque if submissions happen "recursively" (i.e., a pushed task submits new
  // tasks)
  currentPool = this;
  workerId = threadId;

  // State to track thread's current status to coordinate the thread's
  // waiting, yielding, and finishing control flow
  auto status{ThreadStatus::active};

  while (true) {
    // Store the observed count at the beginning of the worker loop, this allows
    // us to try to park the current thread if the observed count remains the
    // same indicating no other thread incremented the count and is trying to
    // awake this thread
    const Index observedCnt{
        status == ThreadStatus::yielded
            ? parkDetails_[threadId].value.load(MemoryOrder::acquire)
            : 0};

    // Drain tasks from the thread's queue for tasks submitted by non-workers
    // so other threads can steal this work
    for (Index i{1}, nUnawoken{nThreads_ - 1};; ++i) {
      std::optional task{externalQueues_[threadId].pop()};

      // No more new tasks
      if (!task) {
        break;
      }

      internalDeques_[threadId].push(*task);

      // Only try to awake threads we haven't already awoken
      if (nUnawoken) {
        --nUnawoken;
        const Index awakeIdx{(threadId + i) % nThreads_};
        tryUnparkThread(awakeIdx);
      }
    }

    // Try taking a task from our current thread's work queue first
    if (std::optional task{internalDeques_[threadId].pop()}) {
      markActiveThread(status);
      FutureNodeBase* const base{*task};
      base->execute(base);
      continue;
    }

    // Try stealing a task from the other threads
    bool stoleTask{false};

    for (Index i{1}; i < nThreads_; ++i) {
      // Index of thread to try to steal from
      const Index stealId{(threadId + i) % nThreads_};

      // Successful stealing of a task
      if (std::optional task{internalDeques_[stealId].steal()}) {
        markActiveThread(status);
        FutureNodeBase* const base{*task};
        base->execute(base);
        stoleTask = true;
        break;
      }
    }

    if (!stoleTask) {
      // Destructor called and we should break if every thread is done
      if (stop_.load(MemoryOrder::acquire)) [[unlikely]] {
        markFinishedThread(status);

        // No more active threads, all tasks are done, go ahead and break
        if (nFinished_.load(MemoryOrder::acquire) == nThreads_) {
          break;
        }

        std::this_thread::yield();
      } else {
        // We yield first and then perform another pass before parking the
        // thread
        if (status != ThreadStatus::yielded) {
          status = ThreadStatus::yielded;
          std::this_thread::yield();
        } else {
          parkThread(threadId, observedCnt);
          status = ThreadStatus::active;
        }
      }
    }
  }
}

void ThreadPool::tryUnparkThread(const Index threadId) noexcept {
  assert(threadId >= 0 && threadId < nThreads_ && "Out of bounds threadId");
  parkDetails_[threadId].value.fetch_add(1, MemoryOrder::release);

  // Only pay for notify_one()'s syscall if the thread is actually parked (more
  // expensive than the additional store load acquired)
  if (parkDetails_[threadId].parked.exchange(false, MemoryOrder::acquire)) {
    parkDetails_[threadId].value.notify_one();
  }
}

void ThreadPool::parkThread(const Index threadId,
                            const Index observedCnt) noexcept {
  assert(threadId >= 0 && threadId < nThreads_ && "Out of bounds threadId");
  parkDetails_[threadId].parked.store(true, MemoryOrder::release);
  parkDetails_[threadId].value.wait(observedCnt, MemoryOrder::acquire);
  parkDetails_[threadId].parked.store(false, MemoryOrder::release);
}

void ThreadPool::markActiveThread(ThreadStatus& status) noexcept {
  // Current thread previously was finished but found a task, re-increment
  // the number of active threads
  if (status == ThreadStatus::finished) [[unlikely]] {
    nFinished_.fetch_sub(1, MemoryOrder::release);
  }

  status = ThreadStatus::active;
}

void ThreadPool::markFinishedThread(ThreadStatus& status) noexcept {
  if (status != ThreadStatus::finished) {
    nFinished_.fetch_add(1, MemoryOrder::release);
  }

  status = ThreadStatus::finished;
}

}  // namespace ThreadWeave
