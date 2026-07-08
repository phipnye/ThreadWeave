#ifndef TW_DEQUE_H
#define TW_DEQUE_H

#include <threadweave/Constants.h>

#include <atomic>
#include <cassert>
#include <iostream>
#include <memory>
#include <optional>
#include <print>
#include <source_location>
#include <type_traits>
#include <vector>

namespace ThreadWeave {

namespace Internal {

/**
 * A helper array class for our work-stealing deque.
 * @tparam T A generic type for storing in the array.
 */
template <typename T>
class Array {
  static constexpr Index defaultCapacity{16};
  std::unique_ptr<std::atomic<T>[]> buffer_;
  const Index capacity_;  // capacity is read-only and safe from data races

 public:
  // Ctor (capacity must be a power of 2 for correct bitmask logic)
  explicit Array(const Index capacity = defaultCapacity)
      : buffer_{std::make_unique<std::atomic<T>[]>(capacity)},
        capacity_{capacity} {
    assert(!(capacity & (capacity - 1)) && "capacity must be a power of 2.");
  }

  // Dtor
  ~Array() = default;

  // Copies and moves shouldn't be necessary
  Array(const Array&) = delete;
  Array(Array&&) = delete;
  Array& operator=(const Array&) = delete;
  Array& operator=(Array&&) = delete;

  // Retrieve the capacity
  Index capacity() const noexcept {
    return capacity_;
  }

  // Index an element in the array (with wrap around logic)
  std::atomic<T>& operator[](const Index idx) noexcept {
    return buffer_[idx & (capacity_ - 1)];
  }
};

}  // namespace Internal

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
class ChaseLevDeque {
  // --- Data members
  using Index = Internal::Index;
  using Array = Internal::Array<T>;
  alignas(Internal::CacheLineSize) std::atomic<Array*> data_;
  alignas(Internal::CacheLineSize) std::atomic<Index> front_{0};
  alignas(Internal::CacheLineSize) std::atomic<Index> back_{0};
  std::vector<std::unique_ptr<Array>> garbage_{};

 public:
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
  std::optional<T> pop();

  /**
   * Steal an item from the front of the deque. This function is intended to be
   * invoked by the consumer.
   * @return the element at the back of the deque or std::nullopt if empty or a
   * race was lost
   */
  std::optional<T> steal();

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
  void expand(Index front, Index back);
};

template <typename T>
  requires(std::is_default_constructible_v<T> &&
           std::is_trivially_copyable_v<T>)
ChaseLevDeque<T>::ChaseLevDeque() : data_{new Array{}} {
  // The logic for this class is greatly simplified when std::atomic<T> is
  // itself lock free. Storing std::atomic<T*> would require heap allocations
  // which are prone to std::bad_alloc exceptions.
#ifndef NDEBUG
  if constexpr (!std::atomic<T>::is_always_lock_free) {
    std::println(
        std::cerr,
        "[Warning] in {}\n'std::atomic<T>' is not lock-free on this target "
        "hardware.",
        std::source_location::current().function_name());
  }
#endif
  garbage_.reserve(16);
}

template <typename T>
  requires(std::is_default_constructible_v<T> &&
           std::is_trivially_copyable_v<T>)
ChaseLevDeque<T>::~ChaseLevDeque() {
  delete data_.load(std::memory_order::relaxed);
}

template <typename T>
  requires(std::is_default_constructible_v<T> &&
           std::is_trivially_copyable_v<T>)
void ChaseLevDeque<T>::push(T item) {
  // push() is only ever called by the single owner thread, and back_ is only
  // ever written by the owner, so this load can be relaxed: no other thread
  // writes back_, and we don't need it to synchronize with anything here.
  const Index back{back_.load(std::memory_order::relaxed)};

  // TO DO: Review if this must use acquire semantics
  // https://stackoverflow.com/questions/79976694/is-the-acquire-load-on-top-necessary-in-this-c11-chase-lev-deque-implementation
  const Index front{front_.load(std::memory_order::acquire)};
  Array* data{data_.load(std::memory_order::relaxed)};

  // Deque is full, double the capacity of it
  if (back - front + 1 > data->capacity()) {
    expand(front, back);
    data = data_.load(std::memory_order::relaxed);
  }

  // Insert item at back index
  (*data)[back].store(item, std::memory_order::relaxed);

  // Ensure the newly pushed item is globally visible in memory before we
  // publish the updated back_ index. A release fence pairs with the acquire
  // load in steal(), ensuring that any thief thread that sees the new back_
  // index will also see the item we just wrote to the array.
  std::atomic_thread_fence(std::memory_order::release);
  back_.fetch_add(1, std::memory_order::relaxed);
}

template <typename T>
  requires(std::is_default_constructible_v<T> &&
           std::is_trivially_copyable_v<T>)
std::optional<T> ChaseLevDeque<T>::pop() {
  Array& data{*data_.load(std::memory_order::relaxed)};
  const Index back{back_.fetch_sub(1, std::memory_order::relaxed) - 1};

  // We just wrote to back_ and are about to read front_ (load). This seq_cst
  // fence prevents the from reordering the load of front_ to occur before the
  // store to back_ is globally visible. Without this, the owner could read a
  // stale front_, while a thief simultaneously reads a stale back_, causing
  // both to bypass the safety CAS and pop the exact same final element.
  std::atomic_thread_fence(std::memory_order::seq_cst);
  Index front{front_.load(std::memory_order::relaxed)};

  // Empty deque
  if (front > back) {
    back_.fetch_add(1, std::memory_order::relaxed);
    return std::nullopt;
  }

  // Front and back point to same element and there is a race condition
  // between whether consumer or producer gets it
  if (front == back) {
    // Both the owner (pop) and a thief (steal) are trying to claim the final
    // element. They both attempt the same CAS operation on front_ to settle
    // this with the winner taking the final element. The seq_cst ordering on
    // success ensures total global ordering of this arbitration. If the owner
    // wins the CAS, it gets the item. If it loses, a thief stole it first, and
    // the deque is now empty.
    const bool wonRace{front_.compare_exchange_strong(
        front, front + 1, std::memory_order::seq_cst,
        std::memory_order::relaxed)};
    back_.fetch_add(1, std::memory_order::relaxed);

    // Lost race condition to consumer, return std::nullopt
    if (!wonRace) {
      return std::nullopt;
    }
  }

  return data[back].load(std::memory_order::relaxed);
}

template <typename T>
  requires(std::is_default_constructible_v<T> &&
           std::is_trivially_copyable_v<T>)
std::optional<T> ChaseLevDeque<T>::steal() {
  // Note the use of an atomic fence is the reverse ordering of that used in
  // pop()
  // In pop(): store(back) -> seq_cst fence -> load(front)
  // In steal(): load(front) -> seq_cst fence -> load(back)
  // Without this seq_cst fence, the owner's load of front could be reordered
  // before its store to back is globally visible. Simultaneously, the thief
  // could read back before front. Both threads could erroneously observe > 1
  // items, bypass the CAS safety net, and pop the same item, resulting in
  // duplicate item retrieval.
  Index front{front_.load(std::memory_order::acquire)};
  std::atomic_thread_fence(std::memory_order::seq_cst);

  // Empty queue
  if (const Index back{back_.load(std::memory_order::acquire)}; front >= back) {
    return std::nullopt;
  }

  // Acquire pointer to return element
  Array& data{*data_.load(std::memory_order::acquire)};

  // This CAS is symmetric to the CAS in pop(). It decides who wins the race for
  // the last element in the deque (whether competing against other thieves or
  // the owner thread), whoever successfully increments front_ via CAS claims
  // the element. A failed CAS means another thread got there first.
  return front_.compare_exchange_strong(front, front + 1,
                                        std::memory_order::seq_cst,
                                        std::memory_order::relaxed)
             ? data[front].load(std::memory_order::relaxed)
             : std::nullopt;
}

template <typename T>
  requires(std::is_default_constructible_v<T> &&
           std::is_trivially_copyable_v<T>)
bool ChaseLevDeque<T>::empty() const noexcept {
  const Index front{front_.load(std::memory_order::relaxed)};
  const Index back{back_.load(std::memory_order::relaxed)};
  return back <= front;
}

template <typename T>
  requires(std::is_default_constructible_v<T> &&
           std::is_trivially_copyable_v<T>)
void ChaseLevDeque<T>::expand(const Index front, const Index back) {
  // Note that we must double the capacity to retain a power of two for the
  // capacity for accurate wrap around indexing logic using a bitmask
  Array* const oldArray{data_.load(std::memory_order::relaxed)};
  Array* const newArray{new Array{oldArray->capacity() << 1}};

  // Copy pointers to the original elements
  for (Index i{front}; i < back; ++i) {
    (*newArray)[i].store((*oldArray)[i].load(std::memory_order::relaxed),
                         std::memory_order::relaxed);
  }

  // Update data pointer
  data_.store(newArray, std::memory_order::release);

  // Store oldArray in case other threads are pointing to it
  garbage_.emplace_back(oldArray);
}

}  // namespace ThreadWeave

#endif
