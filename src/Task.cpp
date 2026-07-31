#include <threadweave/internal/Task.h>
#include <threadweave/internal/utils.h>

namespace ThreadWeave::Internal {

bool TaskBase::isReady() const noexcept {
  return state_.load(MemoryOrder::acquire) == TaskStatus::ready;
}

bool TaskBase::releaseReference() noexcept {
  const auto oldRefCnt{refCount_.fetch_sub(1, MemoryOrder::acq_rel)};
  TW_ASSERT(oldRefCnt > 0, "Double-release detected in FutureNodeBase");
  return oldRefCnt == 1;
}

void TaskBase::wait() noexcept {
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

void TaskBase::notify() noexcept {
  // Update to ready and notify waiting entities if it was originally in a
  // waiting state
  if (state_.exchange(TaskStatus::ready, MemoryOrder::release) ==
      TaskStatus::waiting) {
    state_.notify_one();
  }
}

}  // namespace ThreadWeave::Internal
