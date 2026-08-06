#ifndef TW_TASK_H
#define TW_TASK_H

#include <threadweave/internal/Node.h>
#include <threadweave/internal/utils.h>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <type_traits>

namespace ThreadWeave::Internal {

/**
 * Task base class allows thread pool to store generic tasks of varying return
 * types that can be cast back to the underlying derived type
 */
class TaskBase {
 protected:
  enum class TaskStatus : std::int8_t { pending, ready, waiting };

 public:
  // --- Data members

  // Pointer to function to execute
  void (*execute_)(TaskBase*){nullptr};

  // Status of the result
  std::atomic<TaskStatus> state_{TaskStatus::pending};

  // Reference count for resource management (future and thread pool hold refs)
  std::atomic<std::int8_t> refCount_{2};

  // --- Member functions

  /**
   * Determine if task result is ready
   * @return true if the task result is ready, false otherwise
   */
  bool isReady() const noexcept;

  /**
   * Decrements reference count indicating caller no longer needs the node to
   * stay alive at which point the last caller can call deallocate()
   * @return true if the caller is the last instance with a reference, false
   * otherwise
   */
  bool releaseReference() noexcept;

  /**
   * Wait for the task to finish running
   */
  void wait() noexcept;

  /**
   * Notify when the task is done running
   */
  void notify() noexcept;
};

inline bool TaskBase::isReady() const noexcept {
  return state_.load(MemoryOrder::acquire) == TaskStatus::ready;
}

inline bool TaskBase::releaseReference() noexcept {
  const auto oldRefCnt{refCount_.fetch_sub(1, MemoryOrder::acq_rel)};
  TW_ASSERT(oldRefCnt > 0, "Double-release detected in FutureNodeBase");
  return oldRefCnt == 1;  // true if last holding reference
}

inline void TaskBase::wait() noexcept {
  // Early-return if task already complete
  if (isReady()) {
    return;
  }

  // Try transitioning from waiting to running
  auto expected{TaskStatus::pending};
  state_.compare_exchange_strong(expected, TaskStatus::waiting,
                                 MemoryOrder::release, MemoryOrder::relaxed);

  // Wait until the task is ready (no longer waiting)
  while (!isReady()) {
    state_.wait(TaskStatus::waiting, MemoryOrder::relaxed);
  }
}

inline void TaskBase::notify() noexcept {
  // Update to ready and notify waiting entities if it was originally in a
  // waiting state
  if (state_.exchange(TaskStatus::ready, MemoryOrder::release) ==
      TaskStatus::waiting) {
    state_.notify_one();
  }
}

template <typename T>
class Task : public TaskBase {
  using ResultType = std::conditional_t<std::is_void_v<T>, std::byte, T>;

 public:
#ifdef TW_CALLABLE_STORAGE_SIZE
  // User-defined payload size
  static_assert(TW_CALLABLE_STORAGE_SIZE > 0,
                "TW_CALLABLE_STORAGE_SIZE must be strictly poisitive");
  static constexpr Index kCallableStorageSize{TW_CALLABLE_STORAGE_SIZE};
#else
  // Default to 128 bytes if user does not define value
  static constexpr Index kCallableStorageSize{128};
#endif

  // --- Data members

  // Storage for pool task submission to store a bound task
  alignas(std::max_align_t) std::byte callableStorage_[kCallableStorageSize];

  // Storage for the task result
  alignas(ResultType) std::byte resultStorage_[sizeof(ResultType)];

  // Pointer to any thrown exceptions
  std::exception_ptr exception_{nullptr};

  // Internal node allocator information
  AllocatorInfo<Task> _internal{};

  // Flag allwoing us to track whether the destructor needs to and can be called
  bool hasResult_{false};

  // --- Ctors, dtor, and assignment operators

  Task() = default;
  ~Task();

  // Prevent copies and moves
  Task(const Task&) = delete;
  Task(Task&&) = delete;
  Task& operator=(const Task&) = delete;
  Task& operator=(Task&&) = delete;

  // --- Member functions

  /**
   * Clean up after a task and reset it to it's "default" state
   */
  void reset() noexcept;

 private:
  /**
   * Clean up stored results if present
   */
  void destroyResults() noexcept;
};

template <typename T>
Task<T>::~Task() {
  destroyResults();  // guards against leaking results that were never retrieved
}

template <typename T>
void Task<T>::reset() noexcept {
  destroyResults();
  exception_ = nullptr;
  execute_ = nullptr;
  refCount_.store(2, MemoryOrder::relaxed);
  state_.store(TaskStatus::pending, MemoryOrder::relaxed);
}

template <typename T>
void Task<T>::destroyResults() noexcept {
  if constexpr (!std::is_void_v<T> &&
                !std::is_trivially_destructible_v<ResultType>) {
    if (hasResult_) {
      std::launder(reinterpret_cast<ResultType*>(resultStorage_))
          ->~ResultType();
      hasResult_ = false;
    }
  } else {
    hasResult_ = false;
  }
}

}  // namespace ThreadWeave::Internal

#endif
