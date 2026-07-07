#ifndef TW_DEQUE_H
#define TW_DEQUE_H

#include <threadweave/Constants.h>

#include <atomic>
#include <cassert>
#include <memory>
#include <optional>
#include <type_traits>
#include <utility>
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
  std::unique_ptr<std::atomic<T*>[]> buffer_;
  const Index capacity_;  // capacity is read-only and safe from data races

 public:
  // Ctor (note that the capacity must be a power of 2 for correct bitmask
  // logic)
  explicit Array(const Index capacity = defaultCapacity)
      : buffer_{std::make_unique<std::atomic<T*>[]>(capacity)},
        capacity_{capacity} {
    assert(!(capacity & (capacity - 1)) && "capacity must be a power of 2.");
  }

  // Dtor (note that internal elements should not be deleted because of copy
  // operations in expand())
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
  std::atomic<T*>& operator[](const Index idx) noexcept {
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
  requires(std::is_nothrow_move_constructible_v<T>)
class Deque {
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
  Deque() : data_{new Array{}} {
    garbage_.reserve(16);
  }

  /**
   * Clean up memory associated with Chase Lev Deque
   */
  ~Deque() {
    const Index front{front_.load(std::memory_order::relaxed)};
    const Index back{back_.load(std::memory_order::relaxed)};
    Array* const data{data_.load(std::memory_order::relaxed)};

    for (Index i{front}; i < back; ++i) {
      delete (*data)[i].load(std::memory_order::relaxed);
    }

    delete data;
  }

  // Prevent copy and move operations
  Deque(const Deque&) = delete;
  Deque(Deque&&) = delete;
  Deque& operator=(const Deque&) = delete;
  Deque& operator=(Deque&&) = delete;

  /**
   * Push an item to the back of the deque. This function is intended to be
   * invoked by the producer.
   * @param item item to be pushed to the back of the deque
   */
  void push(T item) {
    const Index back{back_.load(std::memory_order::relaxed)};
    const Index front{front_.load(std::memory_order::acquire)};
    Array* data{data_.load(std::memory_order::relaxed)};

    // Deque is full, double the capacity of it
    if (back - front + 1 > data->capacity()) {
      expand(front, back);
      data = data_.load(std::memory_order::relaxed);
    }

    // Insert item at back index
    (*data)[back].store(new T{std::move(item)}, std::memory_order::relaxed);

    // Ensures that an object isn't stolen before we update back
    std::atomic_thread_fence(std::memory_order::release);
    back_.fetch_add(1, std::memory_order::relaxed);
  }

  /**
   * Pop an item from the back of the deque. This function is intended to be
   * invoked by the producer.
   * @return the element at the back of the deque or std::nullopt if empty
   */
  std::optional<T> pop() {
    Array* data{data_.load(std::memory_order::relaxed)};
    const Index back{back_.fetch_sub(1, std::memory_order::relaxed) - 1};
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
      const bool wonRace{front_.compare_exchange_strong(
          front, front + 1, std::memory_order::seq_cst,
          std::memory_order::relaxed)};
      back_.fetch_add(1, std::memory_order::relaxed);

      // Lost race condition to consumer, return std::nullopt
      if (!wonRace) {
        return std::nullopt;
      }
    }

    // Acquire pointer to return data
    std::atomic<T*>& slot{(*data)[back]};
    T* item{slot.load(std::memory_order::relaxed)};

    // Move the resource and clean up
    std::optional<T> res{std::make_optional(std::move(*item))};
    delete item;
    slot.store(nullptr, std::memory_order::relaxed);
    return res;
  }

  /**
   * Steal an item from the front of the deque. This function is intended to be
   * invoked by the consumer.
   * @return the element at the back of the deque or std::nullopt if empty or a
   * race was lost
   */
  std::optional<T> steal() {
    Index front{front_.load(std::memory_order::acquire)};
    std::atomic_thread_fence(std::memory_order::seq_cst);

    // Empty queue
    if (const Index back{back_.load(std::memory_order::acquire)};
        front >= back) {
      return std::nullopt;
    }

    // Acquire pointer to return element
    Array* data{data_.load(std::memory_order::acquire)};
    std::atomic<T*>& slot{(*data)[front]};
    T* item{slot.load(std::memory_order::relaxed)};

    // Lost race condition (return nullopt)
    if (!front_.compare_exchange_strong(front, front + 1,
                                        std::memory_order::seq_cst,
                                        std::memory_order::relaxed)) {
      return std::nullopt;
    }

    // Move the resource and clean up
    std::optional<T> res{std::make_optional(std::move(*item))};
    delete item;
    slot.store(nullptr, std::memory_order::relaxed);
    return res;
  }

 private:
  /**
   * Expand the underyling array to double the capacity.
   */
  void expand(const Index front, const Index back) {
    Array* const oldArray{data_.load(std::memory_order::relaxed)};
    const Index oldCap{oldArray->capacity()};

    // Note that we must double the capacity to retain a power of two for the
    // capacity for accurate wrap around indexing logic using a bitmask
    const Index newCap{oldCap << 1};
    Array* const newArray{new Array{newCap}};

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
};

}  // namespace ThreadWeave

#endif
