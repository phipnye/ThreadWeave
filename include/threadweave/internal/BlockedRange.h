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
      : first_{first}, last_{last}, grainSize_{grainSize} {
    TW_ASSERT(grainSize > 0, "Grain size should be strictly positive");
  }

  // --- Member functions
  BlockedRange split() noexcept(noexcept(dist())) {
    TW_ASSERT(isDivisible(), "Trying to split an indivisible BlockedRange");
    Iter mid{first_ + (dist() >> 1)};
    BlockedRange res{mid, last_, grainSize_};
    last_ = mid;
    return res;
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
