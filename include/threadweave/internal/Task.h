#ifndef TW_TASK_H
#define TW_TASK_H

#include <threadweave/internal/Node.h>
#include <threadweave/internal/utils.h>

#include <cstdint>
#include <type_traits>

namespace ThreadWeave::Internal {
// Future node base class to hold function pointer and maintain node reference
// count
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

  // Determine if future result is ready
  bool isReady() const noexcept;

  // Decrements reference count indicating caller no longer needs the node to
  // stay alive at which point the last caller can call deallocate()
  bool releaseReference() noexcept;

  // Wait for the task to finish running
  void wait() noexcept;

  // Notify when the task is done running
  void notify() noexcept;
};

template <typename T>
class Task : public TaskBase {
  using ResultT = std::conditional_t<std::is_void_v<T>, std::byte, T>;

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
  alignas(std::max_align_t) std::byte callableStorage_[kCallableStorageSize];
  alignas(ResultT) std::byte resultStorage_[sizeof(ResultT)];
  std::exception_ptr exception_{nullptr};
  AllocatorInfo<Task> _internal{};
  bool hasResult_{false};

  // Dtor
  ~Task();

  // --- Member functions

  // Reset the members of our future node instance
  void reset() noexcept;

 private:
  // Clean up stored results if present
  void destroyResults() noexcept;
};

template <typename T>
Task<T>::~Task() {
  // Guards against leaking a completed result that was never retrieved
  destroyResults();
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
  if constexpr (!std::is_void_v<T>) {
    if (hasResult_) {
      std::launder(reinterpret_cast<ResultT*>(resultStorage_))->~ResultT();
      hasResult_ = false;
    }
  } else {
    hasResult_ = false;
  }
}

}  // namespace ThreadWeave::Internal

#endif
