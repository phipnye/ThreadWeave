#include <gtest/gtest.h>
#include <threadweave/Stack.h>

#include <memory>

template <typename T>
using Stack = ThreadWeave::Stack<T>;

// Make sure the stack follows LIFO when executed sequentially
TEST(StackTest, StackIsLIFO) {
  constexpr int n{100};
  Stack<int> stk{};

  for (int i{0}; i < n; ++i) {
    stk.push(i);
  }

  for (int i{n - 1}; i > -1; --i) {
    EXPECT_FALSE(stk.empty());
    const auto val{stk.pop()};
    EXPECT_TRUE(val.has_value());
    EXPECT_EQ(*val, i);
  }

  EXPECT_TRUE(stk.empty());
}

// Test LIFO against a move only type
TEST(StackTest, MoveOnlyType) {
  constexpr int n{100};
  Stack<std::unique_ptr<int>> stk{};

  for (int i{0}; i < n; ++i) {
    stk.push(std::make_unique<int>(i));
  }

  for (int i{n - 1}; i > -1; --i) {
    EXPECT_FALSE(stk.empty());
    const auto val{stk.pop()};
    EXPECT_TRUE(val.has_value());
    EXPECT_EQ(**val, i);
  }
}

// Test concurrent pushes and make sure no data is lost
TEST(StackTest, ConcurrentPushes) {
  Stack<int> stk{};
  constexpr int nThreads{8};
  constexpr int itemsPerThread{1000};
  std::vector<std::jthread> threads{};
  threads.reserve(nThreads);

  for (int t{0}; t < nThreads; ++t) {
    threads.emplace_back([=, &stk] {
      for (int i{0}; i < itemsPerThread; ++i) {
        stk.push(t * itemsPerThread + i);
      }
    });
  }

  threads.clear();
  constexpr int maxVal{nThreads * itemsPerThread};
  int cnts[maxVal]{};

  while (const auto val{stk.pop()}) {
    EXPECT_GE(*val, 0);
    EXPECT_LT(*val, maxVal);
    ++cnts[*val];
  }

  EXPECT_TRUE(stk.empty());

  for (int i{0}; i < maxVal; ++i) {
    EXPECT_EQ(cnts[i], 1);
  }
}
