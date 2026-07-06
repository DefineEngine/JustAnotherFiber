#include <jaf/Jaf.hpp>

#include <gtest/gtest.h>

#include <stdexcept>

using jaf::Fiber;

TEST(Fiber, RunsToCompletionWithoutYield) {
    bool ran = false;
    Fiber f([&] { ran = true; });

    EXPECT_EQ(f.state(), Fiber::State::Ready);
    EXPECT_FALSE(f.done());

    f.resume();

    EXPECT_TRUE(ran);
    EXPECT_TRUE(f.done());
    EXPECT_EQ(f.state(), Fiber::State::Done);
}

TEST(Fiber, SwitchesAndReturns) {
    int counter = 0;
    Fiber f([&] {
        ++counter;                  // 0 -> 1, then hand control back
        jaf::this_fiber::yield();
        ++counter;                  // 1 -> 2 after we are resumed
    });

    EXPECT_EQ(counter, 0);

    f.resume();                     // runs up to the yield
    EXPECT_EQ(counter, 1);
    EXPECT_EQ(f.state(), Fiber::State::Suspended);
    EXPECT_FALSE(f.done());

    f.resume();                     // continues past the yield to the end
    EXPECT_EQ(counter, 2);
    EXPECT_TRUE(f.done());
}

TEST(Fiber, MultipleYields) {
    constexpr int kRounds = 5;
    int steps = 0;

    Fiber f([&] {
        for (int i = 0; i < kRounds; ++i) {
            ++steps;
            jaf::this_fiber::yield();
        }
    });

    int resumes = 0;
    while (!f.done()) {
        f.resume();
        ++resumes;
    }

    EXPECT_EQ(steps, kRounds);
    // kRounds resumes each reach a yield; one extra resume runs off the end.
    EXPECT_EQ(resumes, kRounds + 1);
}

TEST(Fiber, CapturesAndMutatesAcrossSwitches) {
    int value = 10;
    Fiber f([&] {
        value *= 2;                 // 20
        jaf::this_fiber::yield();
        value += 5;                 // 25
    });

    f.resume();
    EXPECT_EQ(value, 20);
    f.resume();
    EXPECT_EQ(value, 25);
}

TEST(Fiber, ResumeAfterDoneIsNoop) {
    int n = 0;
    Fiber f([&] { ++n; });

    f.resume();
    EXPECT_EQ(n, 1);

    f.resume();   // already Done -> no-op
    f.resume();
    EXPECT_EQ(n, 1);
    EXPECT_TRUE(f.done());
}

TEST(Fiber, ExceptionPropagatesOnResume) {
    Fiber f([] { throw std::runtime_error("boom"); });

    EXPECT_THROW(f.resume(), std::runtime_error);
    EXPECT_TRUE(f.done());
}

// ---- C-6 misuse death tests (always-on JAF_ASSERT) -------------------------

TEST(FiberDeathTest, ResumeWhileRunningAborts) {
    // C-6a: re-entrantly resuming a fiber that is already Running.
    Fiber* self = nullptr;
    Fiber f([&] { self->resume(); });   // resume ourselves while Running
    self = &f;
    EXPECT_DEATH({ f.resume(); }, "already-running fiber");
}

TEST(FiberDeathTest, YieldOutsideFiberAborts) {
    // C-6b: yielding with no current fiber (called on the thread's own stack).
    EXPECT_DEATH({ jaf::this_fiber::yield(); }, "outside of a fiber");
}

TEST(FiberDeathTest, TinyStackAborts) {
    // C-6d: a stack below config::kMinStackSize would let make_fcontext write
    // below the allocation.
    EXPECT_DEATH({ Fiber f([] {}, 1024); (void)f; }, "kMinStackSize");
}

TEST(Fiber, NestedResume) {
    int order = 0;
    int inner_ran_at = 0;

    Fiber inner([&] { inner_ran_at = ++order; });

    Fiber outer([&] {
        inner.resume();             // drive a fiber from inside a fiber
        EXPECT_TRUE(inner.done());
        jaf::this_fiber::yield();
    });

    outer.resume();                 // outer runs inner to completion, then yields
    EXPECT_EQ(inner_ran_at, 1);
    EXPECT_TRUE(inner.done());
    EXPECT_FALSE(outer.done());

    outer.resume();                 // finish outer
    EXPECT_TRUE(outer.done());
}
