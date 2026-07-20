#ifndef TW_NODE_H
#define TW_NODE_H

#include <threadweave/enums.h>
#include <threadweave/utils.h>

#include <atomic>
#include <concepts>
#include <cstddef>
#include <new>
#include <type_traits>

#ifndef NDEBUG
#include <cstring>
#endif

namespace ThreadWeave::Internal {

/**
 * A helper aggreagate to manage "internal" data that our allocator can use to
 * resolve memory managment tasks without manipulating the node's "actual" next
 * member causing potential data races
 * @tparam Node a node type to link
 */
template <typename Node>
struct AllocatorInfo {
  Node* next{nullptr};
  bool isBlockStart{false};
};

/**
 * Shared reset logic for a plain payload member (StackNode/QueueNode's data)
 * @tparam T a generic type of data contained in a node instance
 * @param value a value type to be reset/overwritten with value initialized data
 */
template <typename T>
void resetValue(T& value) noexcept {
  static_assert(std::is_nothrow_default_constructible_v<T>,
                "Node payload type must be nothrow-default-constructible to "
                "be safely recycled inside a noexcept reset()");
  if constexpr (std::is_trivially_destructible_v<T> &&
                std::is_trivially_default_constructible_v<T> &&
                std::is_trivially_copy_assignable_v<T>) {
    value = T{};
  } else {
    value.~T();
    ::new (&value) T{};
  }
}

/**
 * Simple aggregate for nodes of a singly linked list to be used as the
 * underlying implementation of a stack. The additional retire next pointer
 * allows storage in a retirement list without introducing data races.
 * @tparam T Type of data to store in the node
 */
template <typename T>
  requires(std::is_nothrow_default_constructible_v<T>)
struct StackNode {
  T data{};
  StackNode* next{nullptr};
  AllocatorInfo<StackNode> _internal{};

  void reset() noexcept {
    resetValue(data);
    next = nullptr;
  }
};

/**
 * Simple aggregate for nodes of a singly linked list to be used as the
 * underlying implementation of a queue. The additional retire next pointer
 * allows storage in a retirement list without introducing data races.
 * @tparam T Type of data to store in the node
 */
template <typename T>
  requires(std::is_nothrow_default_constructible_v<T>)
struct QueueNode {
  T data{};
  std::atomic<QueueNode*> next{nullptr};
  AllocatorInfo<QueueNode> _internal{};

  void reset() noexcept {
    resetValue(data);
    next.store(nullptr, MemoryOrder::relaxed);
  }
};

// Future node base class to hold function pointer and maintain node reference
// count
struct FutureNodeBase {
  void (*execute)(FutureNodeBase*){nullptr};
  std::atomic<std::int8_t> refCount{2};  // future and thread pool hold refs

  // Decrements reference count indicating caller no longer needs the node to
  // stay alive at which point the last caller can call deallocate()
  bool release() noexcept {
    return refCount.fetch_sub(1, MemoryOrder::acq_rel) == 1;
  }
};

template <typename T>
struct FutureNode : FutureNodeBase {  // NOLINT(*-pro-type-member-init)
 private:
  using ResultT = std::conditional_t<std::is_void_v<T>, std::byte, T>;

 public:
#ifdef TW_PAYLOAD_SIZE
  // User-defined payload size
  static_assert(TW_PAYLOAD_SIZE > 0,
                "TW_PAYLOAD_SIZE must be strictly poisitive");
  static constexpr Index payloadSize{TW_PAYLOAD_SIZE};
#else
  // Default to 128 bytes if user does not define value
  static constexpr Index payloadSize{128};
#endif

  // --- Data members
  alignas(std::max_align_t) std::byte payload[payloadSize];  // function payload
  std::exception_ptr exception{nullptr};
  alignas(ResultT) std::byte resultBuffer[sizeof(ResultT)];
  AllocatorInfo<FutureNode> _internal{};
  std::atomic<Status> state{Status::pending};
  bool hasResult{false};

  // Dtor
  ~FutureNode() {
    // Guards against leaking a completed result that was never retrieved
    destroyResults();
  }

  // --- Member functions

  // Wait for the task to finish running
  void wait() noexcept {
    // Early-return if task already complete
    if (state.load(MemoryOrder::acquire) == Status::ready) {
      return;
    }

    // Try transitioning from waiting to running
    auto expected{Status::pending};
    state.compare_exchange_strong(expected, Status::waiting,
                                  MemoryOrder::release, MemoryOrder::relaxed);

    // Wait until the task is ready (no longer waiting)
    while (state.load(MemoryOrder::acquire) != Status::ready) {
      state.wait(Status::waiting, MemoryOrder::relaxed);
    }
  }

  // Notify when the task is done running
  void notify() noexcept {
    // Update to ready and notify waiting entities if it was originally in a
    // waiting state
    if (const Status oldState{
            state.exchange(Status::ready, MemoryOrder::release)};
        oldState == Status::waiting) {
      state.notify_one();
    }
  }

  // Reset the members of our future node instance (note this is only safe once
  // no other thread can observe this node, the allocator enforces this before
  // calling reset)
  void reset() noexcept {
    destroyResults();
    exception = nullptr;
    execute = nullptr;
    refCount.store(2, MemoryOrder::relaxed);
    state.store(Status::pending, MemoryOrder::relaxed);

#ifndef NDEBUG
    std::memset(payload, 0, sizeof(payload));
    std::memset(resultBuffer, 0, sizeof(resultBuffer));
#endif
  }

 private:
  // Clean up stored results if present
  void destroyResults() noexcept {
    if constexpr (!std::is_void_v<T>) {
      if (hasResult) {
        std::launder(reinterpret_cast<ResultT*>(resultBuffer))->~ResultT();
        hasResult = false;
      }
    } else {
      hasResult = false;
    }
  }
};

// Concept to check if the type has a raw internal next pointer for internal
// node mechanics like pushing to a retirement list
template <typename Node>
concept HasAllocatorInfo = requires(Node node) {
  { node._internal } -> std::same_as<AllocatorInfo<Node>&>;
};

// Concept to check if a node has a reset method
template <typename Node>
concept HasReset = requires(Node node) {
  { node.reset() } noexcept;
};

// Concept to determine if a node type has sufficient information for the
// allocator
template <typename Node>
concept AllocatorEligibleNode = HasAllocatorInfo<Node> && HasReset<Node>;

}  // namespace ThreadWeave::Internal

#endif
