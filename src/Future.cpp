#include <threadweave/Future.h>
#include <threadweave/utils.h>

namespace ThreadWeave::Internal {

bool FutureNodeBase::isReady() const noexcept {
  return state.load(MemoryOrder::acquire) == FutureStatus::ready;
}

bool FutureNodeBase::release() noexcept {
  const auto oldRefCnt{refCount.fetch_sub(1, MemoryOrder::acq_rel)};
  TW_ASSERT(oldRefCnt > 0, "Double-release detected in FutureNodeBase");
  return oldRefCnt == 1;
}

void FutureNodeBase::wait() noexcept {
  // Early-return if task already complete
  if (isReady()) {
    return;
  }

  // Try transitioning from waiting to running
  auto expected{FutureStatus::pending};
  state.compare_exchange_strong(expected, FutureStatus::waiting,
                                MemoryOrder::release, MemoryOrder::relaxed);

  // Wait until the task is ready (no longer waiting)
  while (!isReady()) {
    state.wait(FutureStatus::waiting, MemoryOrder::relaxed);
  }
}

void FutureNodeBase::notify() noexcept {
  // Update to ready and notify waiting entities if it was originally in a
  // waiting state
  if (state.exchange(FutureStatus::ready, MemoryOrder::release) ==
      FutureStatus::waiting) {
    state.notify_one();
  }
}

}  // namespace ThreadWeave::Internal
