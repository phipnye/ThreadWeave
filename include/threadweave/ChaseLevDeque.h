#ifndef TW_CHASE_LEV_DEQUE_H
#define TW_CHASE_LEV_DEQUE_H

#include <threadweave/utils.h>

#include <atomic>
#include <cassert>
#include <iostream>
#include <memory>
#include <optional>
#include <source_location>
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
class alignas(Internal::CacheLineSize) ChaseLevDeque {
  // --- A helper array class for our work-stealing deque.
  class RingBuffer {
    static constexpr Index defaultCapacity{16};
    std::unique_ptr<std::atomic<T>[]> buffer_;
    const Index capacity_;  // capacity is read-only and safe from data races

   public:
    // Ctor (capacity must be a power of 2 for correct bitmask logic)
    explicit RingBuffer(Index capacity = defaultCapacity);

    // Dtor
    ~RingBuffer() = default;

    // Copies and moves shouldn't be necessary
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
  alignas(Internal::CacheLineSize) std::atomic<RingBuffer*> data_;
  alignas(Internal::CacheLineSize) std::atomic<Index> front_{0};
  alignas(Internal::CacheLineSize) std::atomic<Index> back_{0};
  std::vector<std::unique_ptr<RingBuffer>> garbage_{};

 public:
  // To make sure of no invalid pointer use after a resize, we keep track of the
  // number of expansions to test our logic in unit tests
#ifndef NDEBUG
  alignas(Internal::CacheLineSize) std::atomic<int> debugExpandCnt{0};
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
   * Determine if the deque is empty. This function is intended to be invoked by
   * the consumer.
   * @return true if the deque is empty and false otherwise.
   */
  bool empty() const noexcept;

 private:
  /**
   * Expand the underyling array to double the capacity.
   */
  RingBuffer* expand(Index front, Index back);
};

template <typename T>
  requires(std::is_default_constructible_v<T> &&
           std::is_trivially_copyable_v<T>)
ChaseLevDeque<T>::RingBuffer::RingBuffer(const Index capacity)
    : buffer_{std::make_unique<std::atomic<T>[]>(capacity)},
      capacity_{capacity} {
  assert(!(capacity & (capacity - 1)) && "capacity must be a power of 2.");
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

template <typename T>
  requires(std::is_default_constructible_v<T> &&
           std::is_trivially_copyable_v<T>)
ChaseLevDeque<T>::ChaseLevDeque() : data_{new RingBuffer{}} {
  // The logic for this class is greatly simplified when std::atomic<T> is
  // itself lock free. Storing std::atomic<T*> would require heap allocations
  // which are prone to std::bad_alloc exceptions.
#ifndef NDEBUG
  if constexpr (!std::atomic<T>::is_always_lock_free) {
    std::cerr
        << "[Warning] in " << std::source_location::current().function_name()
        << "\n'std::atomic<T>' is not lock-free on this target hardware.\n";
  }
#endif
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
  // push() is only ever called by the single owner thread, and back_ is only
  // ever written by the owner, so this load can be relaxed: no other thread
  // writes back_, and we don't need it to synchronize with anything here.
  const Index back{back_.load(MemoryOrder::relaxed)};

  // Must use acquire semantics. When the ring buffer is full, back and front
  // point to the same slot. This acquire synchronizes with a thief's successful
  // CAS (release) on front_, creating a happens-before edge that guarantees the
  // thief finishes reading the old item before this thread overwrites it
  // https://stackoverflow.com/questions/79976694/is-the-acquire-load-on-top-necessary-in-this-c11-chase-lev-deque-implementation
  const Index front{front_.load(MemoryOrder::acquire)};
  RingBuffer* data{data_.load(MemoryOrder::relaxed)};

  // Deque is full, double the capacity of it
  if (back - front + 1 > data->capacity()) [[unlikely]] {
    data = expand(front, back);

#ifndef NDEBUG
    debugExpandCnt.fetch_add(1, MemoryOrder::relaxed);
#endif
  }

  // Insert item at back index
  (*data)[back].store(item, MemoryOrder::relaxed);

  // Ensure the newly pushed item is globally visible in memory before we
  // publish the updated back_ index. A release fence pairs with the acquire
  // load in steal(), ensuring that any thief thread that sees the new back_
  // index will also see the item we just wrote to the array.
  std::atomic_thread_fence(MemoryOrder::release);
  back_.fetch_add(1, MemoryOrder::relaxed);
}

template <typename T>
  requires(std::is_default_constructible_v<T> &&
           std::is_trivially_copyable_v<T>)
std::optional<T> ChaseLevDeque<T>::pop() noexcept {
  RingBuffer& data{*data_.load(MemoryOrder::relaxed)};
  const Index back{back_.fetch_sub(1, MemoryOrder::relaxed) - 1};

  // We just wrote to back_ and are about to read front_ (load). This seq_cst
  // fence prevents reordering the load of front_ to occur before the store to
  // back_ is globally visible. Without this, the owner could read a stale
  // front_, while a thief simultaneously reads a stale back_, causing both to
  // bypass the safety CAS and pop the exact same final element.
  std::atomic_thread_fence(MemoryOrder::seq_cst);
  Index front{front_.load(MemoryOrder::relaxed)};

  // Empty deque
  if (front > back) {
    back_.fetch_add(1, MemoryOrder::relaxed);
    return std::nullopt;
  }

  std::optional<T> res{data[back].load(MemoryOrder::relaxed)};

  // Front and back point to same element and there is a race condition
  // between whether consumer or producer gets it
  if (front == back) {
    // Both the owner (pop) and a thief (steal) are trying to claim the final
    // element. They both attempt the same CAS operation on front_ to settle
    // this with the winner taking the final element. The seq_cst ordering on
    // success ensures total global ordering of this arbitration. If the owner
    // wins the CAS, it gets the item. If it loses, a thief stole it first, and
    // the deque is now empty.
    if (!front_.compare_exchange_strong(front, front + 1, MemoryOrder::seq_cst,
                                        MemoryOrder::relaxed)) {
      res.reset();
    }

    back_.store(back + 1, MemoryOrder::relaxed);
  }

  return res;
}

template <typename T>
  requires(std::is_default_constructible_v<T> &&
           std::is_trivially_copyable_v<T>)
std::optional<T> ChaseLevDeque<T>::steal() noexcept {
  // Note the use of an atomic fence is the reverse ordering of that used in
  // pop()
  // In pop(): store(back) -> seq_cst fence -> load(front)
  // In steal(): load(front) -> seq_cst fence -> load(back)
  // Without this seq_cst fence, the owner's load of front could be reordered
  // before its store to back is globally visible. Simultaneously, the thief
  // could read back before front. Both threads could erroneously observe > 1
  // items, bypass the CAS safety net, and pop the same item, resulting in
  // duplicate item retrieval.
  Index front{front_.load(MemoryOrder::acquire)};
  std::atomic_thread_fence(MemoryOrder::seq_cst);

  // Empty queue (fence cannot serve as an acquisition barrier because it occurs
  // after the fence and thus will not synchronize with the release fence in
  // pop without acquire semantics)
  if (const Index back{back_.load(MemoryOrder::acquire)}; front >= back) {
    return std::nullopt;
  }

  // Acquire pointer to return element
  RingBuffer& data{*data_.load(MemoryOrder::acquire)};
  std::optional<T> res{data[front].load(MemoryOrder::relaxed)};

  // This CAS is symmetric to the CAS in pop(). It decides who wins the race for
  // the last element in the deque (whether competing against other thieves or
  // the owner thread), whoever successfully increments front_ via CAS claims
  // the element. A failed CAS means another thread got there first.
  if (!front_.compare_exchange_strong(front, front + 1, MemoryOrder::seq_cst,
                                      MemoryOrder::relaxed)) {
    res.reset();
  }

  return res;
}

template <typename T>
  requires(std::is_default_constructible_v<T> &&
           std::is_trivially_copyable_v<T>)
bool ChaseLevDeque<T>::empty() const noexcept {
  const Index front{front_.load(MemoryOrder::relaxed)};
  const Index back{back_.load(MemoryOrder::relaxed)};
  return back <= front;
}

template <typename T>
  requires(std::is_default_constructible_v<T> &&
           std::is_trivially_copyable_v<T>)
ChaseLevDeque<T>::RingBuffer* ChaseLevDeque<T>::expand(const Index front,
                                                       const Index back) {
  // Note that we must double the capacity to retain a power of two for the
  // capacity for accurate wrap around indexing logic using a bitmask
  RingBuffer* const oldArray{data_.load(MemoryOrder::relaxed)};
  RingBuffer* const newArray{new RingBuffer{oldArray->capacity() << 1}};

  // Copy over elements
  for (Index i{front}; i < back; ++i) {
    (*newArray)[i].store((*oldArray)[i].load(MemoryOrder::relaxed),
                         MemoryOrder::relaxed);
  }

  // Can't delete oldArray right away because other threads may be using it. We
  // store it in garbage to be deleted later in the destructor.
  garbage_.emplace_back(oldArray);

  // Update data pointer
  data_.store(newArray, MemoryOrder::release);
  return newArray;
}

}  // namespace ThreadWeave

#endif
