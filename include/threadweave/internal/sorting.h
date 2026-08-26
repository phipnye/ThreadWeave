#ifndef TW_SORTING_H
#define TW_SORTING_H

#include <iterator>
#include <type_traits>

#include "threadweave/internal/utils.h"

namespace ThreadWeave::Internal {

/**
 * Helper to select the median of three objects
 * @tparam Iter a random access iterator
 * @tparam Compare a functor for comparison
 * @param a the first object to take the median from
 * @param b the second object to take the median from
 * @param c the third object to take the median from
 * @param comp a comparison function object
 * @return the median of a, b, & c
 */
template <typename Iter, typename Compare>
  requires(std::random_access_iterator<Iter>)
Iter medianOfThree(Iter a, Iter b, Iter c,
                   Compare comp) noexcept(noexcept(comp(*a, *b))) {
  return comp(*a, *b) ? (comp(*b, *c) ? b : (comp(*a, *c) ? c : a))
                      : (comp(*c, *b) ? b : (comp(*c, *a) ? c : a));
}

/**
 * Helper to select a quicksort pivot using a "median of nine" approach
 * @tparam Iter a random access iterator
 * @tparam Compare a functor for comparison
 * @param begin an interator to the beginning of a data segment
 * @param end an iterator the end of a data segment
 * @param comp a comparison function object
 * @return a proxy for a median of nine values
 */
template <typename Iter, typename Compare>
  requires(std::random_access_iterator<Iter>)
Iter pickPivot(Iter begin, Iter end,
               Compare comp) noexcept(noexcept(medianOfThree(begin, begin,
                                                             begin, comp))) {
  constexpr Index n{8};  // one less because we just take std::prev of end
  const auto d{std::distance(begin, end) / n};
  TW_ASSERT(d > n,
            "Pivot selection called with insufficent number of elements");
  std::remove_const_t<decltype(d)> dists[n]{0};

  // Precompute data segment distances
  for (Index i{1}; i < n; ++i) {
    dists[i] = dists[i - 1] + d;
  }

  // Compute the medians of three segments
  Iter m1{medianOfThree(begin + dists[0], begin + dists[1], begin + dists[2],
                        comp)};
  Iter m2{medianOfThree(begin + dists[3], begin + dists[4], begin + dists[5],
                        comp)};
  Iter m3{
      medianOfThree(begin + dists[6], begin + dists[7], std::prev(end), comp)};

  // Perform a "median of nine" by taking median of the three medians
  return medianOfThree(m1, m2, m3, comp);
}

/**
 * Reorder the elements of an iterable object into two partitions for quicksort
 * using Hoare partitioning
 * @tparam Iter a random access iterator
 * @tparam Compare a functor for comparison
 * @param begin an interator to the beginning of a data segment
 * @param end an iterator the end of a data segment
 * @param piv a pivot value to pivot the partitions around
 * @param comp a comparison function object
 * @return an iterator instance to the end (non-inclusive) of the first
 * partition
 */
template <typename Iter, typename Compare>
  requires(std::random_access_iterator<Iter>)
Iter hoarePartition(Iter begin, Iter end, Iter piv, Compare comp) {
  static_assert(Internal::IsCheaplyCopyableV<
                    typename std::iterator_traits<Iter>::value_type>,
                "Calling Hoare partitioning on a non-cheaply copyable type");
  Iter i{begin};
  Iter j{std::prev(end)};
  const auto pivVal{*piv};

  while (true) {
    while (comp(*i, pivVal)) {
      ++i;
    }

    while (comp(pivVal, *j)) {
      --j;
    }

    if (i >= j) {
      return std::next(j);
    }

    std::iter_swap(i, j);
    ++i;
    --j;
  }
}

/**
 * Reorder the elements of an iterable object into two partitions for quicksort
 * using Lomuto partitioning
 * @tparam Iter a random access iterator
 * @tparam Compare a functor for comparison
 * @param begin an interator to the beginning of a data segment
 * @param end an iterator the end of a data segment
 * @param piv a pivot value to pivot the partitions around
 * @param comp a comparison function object
 * @return an iterator instance to the end (non-inclusive) of the first
 * partition
 */
template <typename Iter, typename Compare>
  requires(std::random_access_iterator<Iter>)
Iter lomutoPartition(Iter begin, Iter end, Iter piv, Compare comp) {
  static_assert(!Internal::IsCheaplyCopyableV<
                    typename std::iterator_traits<Iter>::value_type>,
                "Calling Lomuto partitioning on a heaply copyable type");
  Iter last{std::prev(end)};
  std::iter_swap(piv, last);  // pivot stored at last to prevent invalidation
  Iter lPtr{begin};

  for (Iter rPtr{begin}; rPtr != last; ++rPtr) {
    if (comp(*rPtr, *last)) {
      std::iter_swap(lPtr++, rPtr);
    }
  }

  // Move pivot back to it's final location
  std::iter_swap(lPtr, last);
  return lPtr;
}

/**
 * Reorder the elements of an iterable object into two partitions for quicksort
 * @tparam Iter a random access iterator
 * @tparam Compare a functor for comparison
 * @param begin an interator to the beginning of a data segment
 * @param end an iterator the end of a data segment
 * @param piv a pivot value to pivot the partitions around
 * @param comp a comparison function object
 * @return an iterator instance to the end (non-inclusive) of the first
 * partition
 */
template <typename Iter, typename Compare>
  requires(std::random_access_iterator<Iter>)
Iter partition(Iter begin, Iter end, Iter piv, Compare comp) {
  // If it's trivially copyable, use Hoare because we can just copy pivot value
  // and potentially use fewer swaps, otherwise use lomuto
  if constexpr (Internal::IsCheaplyCopyableV<
                    typename std::iterator_traits<Iter>::value_type>) {
    return hoarePartition(begin, end, piv, comp);
  } else {
    return lomutoPartition(begin, end, piv, comp);
  }
}

}  // namespace ThreadWeave::Internal

#endif
