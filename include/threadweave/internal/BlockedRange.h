#ifndef TW_BLOCKED_RANGE_H
#define TW_BLOCKED_RANGE_H

#include <iterator>
#include <type_traits>

namespace ThreadWeave::Internal {

template <typename Iter>
class BlockedRange {
  using DistType = std::iterator_traits<Iter>::difference_type;
  static_assert(std::is_integral_v<DistType>,
                "Expected integral distance type");

  // --- Data members
  Iter first_;
  Iter last_;
  DistType grainSize_;

 public:
  // --- Ctor(s)
  explicit BlockedRange(
      Iter first, Iter last,
      const DistType
          grainSize) noexcept(std::is_nothrow_copy_constructible_v<Iter>)
      : first_{first}, last_{last}, grainSize_{grainSize} {}

  // --- Member functions
  BlockedRange split() noexcept(noexcept(dist())) {
    Iter mid{first_ + (dist() >> 1)};

    if constexpr (std::is_nothrow_constructible_v<BlockedRange, Iter, Iter,
                                                  DistType>) {
      last_ = mid;
      return BlockedRange{mid, last_, grainSize_};
    } else {
      BlockedRange other{mid, last_, grainSize_};
      last_ = mid;
      return other;
    }
  }

  bool isDivisible() const noexcept(noexcept(dist())) {
    return dist() > grainSize_;
  }

  Iter begin() const noexcept {
    return first_;
  }

  Iter end() const noexcept {
    return last_;
  }

 private:
  constexpr DistType dist() const
      noexcept(noexcept(std::distance(first_, last_))) {
    return std::distance(first_, last_);
  }
};

}  // namespace ThreadWeave::Internal

#endif
