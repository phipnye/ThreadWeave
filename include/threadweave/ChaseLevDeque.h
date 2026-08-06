#ifndef TW_CHASE_LEV_DEQUE_H
#define TW_CHASE_LEV_DEQUE_H

#include <threadweave/internal/utils.h>

#include <algorithm>
#include <atomic>
#include <iostream>
#include <limits>
#include <memory>
#include <optional>
#include <source_location>
#include <thread>
#include <type_traits>
#include <vector>

namespace ThreadWeave {

/**
 * An implementation of the Chase Lev work-stealing deque as described in
 * "Correct and Efﬁcient Work-Stealing for Weak Memory Models." This deque
 * follows a single producer, multiple consumer protocol in which the producer
 * operates on the back and the consumer operates on the front.
 * @tparam T A generic type for storing in the deque
 */
template <typename T>
  requires(std::is_default_constructible_v<T> &&
           std::is_trivially_copyable_v<T>)
class alignas(Internal::kCacheLineSize) ChaseLevDeque {
  /**
   * Helper ring buffer class supporting work-stealing deque
   */
  class RingBuffer {
    static constexpr Index kDefaultCapacity{16};  // must be power of 2
    std::unique_ptr<std::atomic<T>[]> buffer_;
    const Index capacity_;

   public:
    // Ctor (capacity must be a power of 2 for correct bitmask logic)
    explicit RingBuffer(Index capacity = kDefaultCapacity);

    // Dtor
    ~RingBuffer() = default;

    // Prevent copies and moves
    RingBuffer(const RingBuffer&) = delete;
    RingBuffer(RingBuffer&&) = delete;
    RingBuffer& operator=(const RingBuffer&) = delete;
    RingBuffer& operator=(RingBuffer&&) = delete;

    // Retrieve the capacity
    Index capacity() const noexcept;

    // Index an element in the array (with wrap around logic)
    std::atomic<T>& operator[](Index idx) noexcept;
  };

  // --- Data members
  alignas(Internal::kCacheLineSize) std::atomic<RingBuffer*> data_;
  alignas(Internal::kCacheLineSize) std::atomic<Index> front_{0};
  alignas(Internal::kCacheLineSize) std::atomic<Index> back_{0};

  // Only producer ever interacts with garbage and thus false sharing and race
  // conditions are non-issue. Garbage defers old buffer deletion until
  // destruction to prevent use-after-free for concurrent thieves.
  std::vector<std::unique_ptr<RingBuffer>> garbage_{};

 public:
#ifndef TW_NDEBUG
  // Make sure of no invalid pointer use after a resize, we keep track of the
  // number of expansions to test our logic in unit tests
  alignas(Internal::kCacheLineSize) mutable std::atomic<int> debugExpandCnt_{0};

  // Make sure only one thread calls push or pop such that user doesn't violate
  // SPMC semantics
  alignas(Internal::kCacheLineSize) mutable std::atomic<
      std::thread::id> ownerThreadId_{};
#endif

  // --- Ctors, dtor, and assignment operators

  /**
   * Default construct a Chase Lev Deque
   */
  ChaseLevDeque();

  /**
   * Clean up memory associated with Chase Lev Deque
   */
  ~ChaseLevDeque();

  // Prevent copy and move operations
  ChaseLevDeque(const ChaseLevDeque&) = delete;
  ChaseLevDeque(ChaseLevDeque&&) = delete;
  ChaseLevDeque& operator=(const ChaseLevDeque&) = delete;
  ChaseLevDeque& operator=(ChaseLevDeque&&) = delete;

  /*
   * Synchronization Key:
   *
   * (1) push() release store on back_
   * Synchronizes-with (4) acquire load on back_ in steal() to publish item
   * payload.
   *
   * (2) pop() seq_cst fence after decrementing back_
   * Enforces total ordering with (4) in steal() to prevent store-load
   * reordering.
   *
   * (3) pop() load of front_
   * Enforces total ordering with (5) CAS on front_ during single-element
   * race.
   *
   * (4) steal() seq_cst fence + acquire load on back_
   * Enforces total ordering with (2) and synchronizes-with (1) to see item
   * payload.
   *
   * (5) steal() / pop() seq_cst CAS on front_
   * Enforces total ordering with (3) and synchronizes-with (6).
   *
   * (6) push() acquire load on front_
   * Synchronizes-with (5) to ensure owner never overwrites a slot while a
   * thief reads it.
   *
   * (7) expand() release store on data_
   * Synchronizes-with (8) to publish newly allocated RingBuffer pointer.
   *
   * (8) steal() acquire load on data_
   * Synchronizes-with (7) to safely read resized RingBuffer pointer and
   * copied elements.
   */

  /**
   * Push an item to the back of the deque. This function is intended to be
   * invoked by the producer.
   * @param item item to be pushed to the back of the deque
   */
  void push(T item);

  /**
   * Pop an item from the back of the deque. This function is intended to be
   * invoked by the producer.
   * @return the element at the back of the deque or std::nullopt if empty
   */
  std::optional<T> pop() noexcept;

  /**
   * Steal an item from the front of the deque. This function is intended to be
   * invoked by the consumer.
   * @return the element at the back of the deque or std::nullopt if empty or a
   * race was lost
   */
  std::optional<T> steal() noexcept;

  /**
   * Determine the approximate number of elements in the deque. Note that this
   * function uses relaxed semantics and should not be used reliably.
   * @return the approximate number of elements in the deque.
   */
  Index approxSize() const noexcept;

  /**
   * Determine if the deque is empty. Equivalent to approxSize() == 0. Note that
   * this function uses relaxed semantics and should not be used reliably.
   * @return true if the deque is empty and false otherwise.
   */
  bool empty() const noexcept;

 private:
  /**
   * Expand the underyling array to double the capacity.
   */
  RingBuffer* expand(Index front, Index back);

  /**
   * Ensure single producer semantics are satisfied
   */
  void ensureSingleProducer() const {
    TW_DEBUG_ONLY(
        const std::thread::id currentId{std::this_thread::get_id()};
        std::thread::id expectedId{};

        // First call records this thread as owner
        if (ownerThreadId_.compare_exchange_strong(
                expectedId, currentId, MemoryOrder::relaxed,
                MemoryOrder::relaxed)) {} else {
          TW_ASSERT(
              expectedId == currentId,
              "ChaseLevDeque SPMC violation: called from non-owner thread");
        });
  }
};

template <typename T>
  requires(std::is_default_constructible_v<T> &&
           std::is_trivially_copyable_v<T>)
ChaseLevDeque<T>::ChaseLevDeque() : data_{new RingBuffer{}} {
  // The logic for this class is greatly simplified when std::atomic<T> is
  // itself lock free. Storing std::atomic<T*> would require heap allocations
  // which are prone to std::bad_alloc exceptions.

  // clang-format off
  TW_DEBUG_ONLY(
  if constexpr (!std::atomic<T>::is_always_lock_free) {
    std::cerr
        << "[Warning] in " << std::source_location::current().function_name()
        << "\n'std::atomic<T>' is not lock-free on this target hardware.\n";
  });
  // clang-format on

  garbage_.reserve(32);
}

template <typename T>
  requires(std::is_default_constructible_v<T> &&
           std::is_trivially_copyable_v<T>)
ChaseLevDeque<T>::~ChaseLevDeque() {
  delete data_.load(MemoryOrder::relaxed);
}

template <typename T>
  requires(std::is_default_constructible_v<T> &&
           std::is_trivially_copyable_v<T>)
void ChaseLevDeque<T>::push(T item) {
  TW_DEBUG_ONLY(ensureSingleProducer(););

  // push() is only ever called by the single owner thread, and back_ is only
  // ever written by the owner, so this load can be relaxed: no other thread
  // writes back_, and we don't need it to synchronize with anything here.
  const Index back{back_.load(MemoryOrder::relaxed)};

  // Must use acquire semantics. When the ring buffer is full, back and front
  // point to the same slot. This acquire synchronizes with a thief's successful
  // CAS (release) on front_, creating a happens-before edge that guarantees the
  // thief finishes reading the old item before this thread overwrites it
  // https://stackoverflow.com/questions/79976694/is-the-acquire-load-on-top-necessary-in-this-c11-chase-lev-deque-implementation

  // (6) Acquire load on front_ synchronizes-with seq_cst CAS (5) in steal/pop
  // (guarantees owner observes thief's progress and doesn't overwrite a slot
  // being read)
  const Index front{front_.load(MemoryOrder::acquire)};
  RingBuffer* data{data_.load(MemoryOrder::relaxed)};

  // Deque is full, double the capacity of it
  if (back - front + 1 > data->capacity()) [[unlikely]] {
    data = expand(front, back);
    TW_DEBUG_ONLY(debugExpandCnt_.fetch_add(1, MemoryOrder::relaxed););
  }

  // (1) Release store on back_ synchronizes-with acquire load (4) in steal()
  // (publishes the item written to data to any thief thread observing back_)
  (*data)[back].store(item, MemoryOrder::relaxed);
  back_.store(back + 1, MemoryOrder::release);
}

template <typename T>
  requires(std::is_default_constructible_v<T> &&
           std::is_trivially_copyable_v<T>)
std::optional<T> ChaseLevDeque<T>::pop() noexcept {
  TW_DEBUG_ONLY(ensureSingleProducer(););
  RingBuffer& data{*data_.load(MemoryOrder::relaxed)};

  // Reserve the item at index (back - 1)
  const Index back{back_.load(MemoryOrder::relaxed) - 1};
  back_.store(back, MemoryOrder::relaxed);

  // (2) seq_cst fence enforces total order with seq_cst load/fence (4) in
  // steal() (prevents store-load reordering where steal() and pop() read stale
  // values and get same item)
  std::atomic_thread_fence(MemoryOrder::seq_cst);

  // (3) Load front_ after seq_cst barrier. Total order enforced against CAS (5)
  // in steal()
  Index front{front_.load(MemoryOrder::relaxed)};

  // Empty deque
  if (front > back) {
    // Restore previous back value
    back_.store(back + 1, MemoryOrder::relaxed);
    return std::nullopt;
  }

  std::optional<T> res{data[back].load(MemoryOrder::relaxed)};

  // Front and back point to same element and there is a race condition
  // between whether consumer or producer gets it
  if (front == back) {
    // (5) seq_cst CAS on front_ enforces total order against load (3) and CAS
    // (5) in steal(). (also synchronizes-with acquire load (6) in push())
    if (!front_.compare_exchange_strong(front, front + 1, MemoryOrder::seq_cst,
                                        MemoryOrder::relaxed)) {
      res.reset();
    }

    // Restore previous back value
    back_.store(back + 1, MemoryOrder::relaxed);
  }

  return res;
}

template <typename T>
  requires(std::is_default_constructible_v<T> &&
           std::is_trivially_copyable_v<T>)
std::optional<T> ChaseLevDeque<T>::steal() noexcept {
  Index front{front_.load(MemoryOrder::relaxed)};

  // (4) seq_cst fence enforces total order with seq_cst fence (2) in pop()
  // (ensures owner and thief both observe (front == back) on 1 element
  // remaining)
  std::atomic_thread_fence(MemoryOrder::seq_cst);

  // (4) Acquire load on back_ synchronizes-with release store (1) in push()
  // (ensures thief observes the published item)
  if (const Index back{back_.load(MemoryOrder::acquire)}; front >= back) {
    return std::nullopt;
  }

  // (8) Acquire load on data_ synchronizes-with release store (7) in expand()
  // (ensures thief sees the reallocated RingBuffer pointer and copied elements)
  RingBuffer& data{*data_.load(MemoryOrder::acquire)};
  std::optional<T> res{data[front].load(MemoryOrder::relaxed)};

  // (5) seq_cst CAS on front_ enforces total order with (3) in pop()
  // (synchronizes with acquire load (6) in push())
  if (!front_.compare_exchange_strong(front, front + 1, MemoryOrder::seq_cst,
                                      MemoryOrder::relaxed)) {
    res.reset();
  }

  return res;
}

template <typename T>
  requires(std::is_default_constructible_v<T> &&
           std::is_trivially_copyable_v<T>)
Index ChaseLevDeque<T>::approxSize() const noexcept {
  const Index back{back_.load(MemoryOrder::relaxed)};
  const Index front{front_.load(MemoryOrder::relaxed)};
  return std::max(back - front, Index{0});
}

template <typename T>
  requires(std::is_default_constructible_v<T> &&
           std::is_trivially_copyable_v<T>)
bool ChaseLevDeque<T>::empty() const noexcept {
  return approxSize() == 0;
}

template <typename T>
  requires(std::is_default_constructible_v<T> &&
           std::is_trivially_copyable_v<T>)
ChaseLevDeque<T>::RingBuffer* ChaseLevDeque<T>::expand(const Index front,
                                                       const Index back) {
  // Note that we must double the capacity to retain a power of two for the
  // capacity for accurate wrap around indexing logic
  RingBuffer* const oldArray{data_.load(MemoryOrder::relaxed)};
  TW_ASSERT(oldArray != nullptr, "oldArray cannot be null during expansion");
  const Index oldCapacity{oldArray->capacity()};
  TW_ASSERT(oldCapacity > 0 &&
                oldCapacity <= (std::numeric_limits<Index>::max() >> 1),
            "RingBuffer capacity overflow during expansion");
  RingBuffer* const newArray{new RingBuffer{oldCapacity << 1}};

  // Copy over elements
  for (Index i{front}; i < back; ++i) {
    (*newArray)[i].store((*oldArray)[i].load(MemoryOrder::relaxed),
                         MemoryOrder::relaxed);
  }

  // Can't delete oldArray right away because other threads may be using it. We
  // store it in garbage to be deleted later in the destructor.
  garbage_.emplace_back(oldArray);

  // (7) Release store on data_ synchronizes-with acquire load (8) in steal()
  // (publishes the new buffer and its contents to thief threads)
  data_.store(newArray, MemoryOrder::release);
  return newArray;
}

template <typename T>
  requires(std::is_default_constructible_v<T> &&
           std::is_trivially_copyable_v<T>)
ChaseLevDeque<T>::RingBuffer::RingBuffer(const Index capacity)
    : buffer_{std::make_unique<std::atomic<T>[]>(capacity)},
      capacity_{capacity} {
  TW_ASSERT(capacity > 0 && !(capacity & (capacity - 1)),
            "capacity must be a positive power of 2.");
}

template <typename T>
  requires(std::is_default_constructible_v<T> &&
           std::is_trivially_copyable_v<T>)
Index ChaseLevDeque<T>::RingBuffer::capacity() const noexcept {
  return capacity_;
}

template <typename T>
  requires(std::is_default_constructible_v<T> &&
           std::is_trivially_copyable_v<T>)
std::atomic<T>& ChaseLevDeque<T>::RingBuffer::operator[](
    const Index idx) noexcept {
  return buffer_[idx & (capacity_ - 1)];
}

}  // namespace ThreadWeave

#endif
