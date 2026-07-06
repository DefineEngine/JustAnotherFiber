// Tests for jaf::Counter value semantics (jaf/Counter.hpp).
// Wait/wake behavior is exercised through the scheduler in SchedulerTest.cpp,
// since it requires a running fiber + worker.

#include <jaf/Counter.hpp>

#include <gtest/gtest.h>

TEST(Counter, StartsAtInitialValue) {
    jaf::Counter c{5};
    EXPECT_EQ(c.value(), 5);

    jaf::Counter zero;
    EXPECT_EQ(zero.value(), 0);
}

TEST(Counter, AddRaisesValue) {
    jaf::Counter c;
    jaf::detail::CounterAdd(c, 3);
    EXPECT_EQ(c.value(), 3);
    jaf::detail::CounterAdd(c, 2);
    EXPECT_EQ(c.value(), 5);
}
