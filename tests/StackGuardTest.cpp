// C-5: guarded fiber stacks.
//
//  * SmallStackOverflowFaults (guarded builds only): a fiber that overruns its
//    64 KiB Small stack hits the PAGE_NOACCESS guard page and faults immediately,
//    instead of silently corrupting adjacent committed memory. An access
//    violation carries no diagnostic string of its own, so the death matcher is
//    intentionally permissive here (unlike the misuse asserts, which match a
//    specific message).
//  * LargeStackHandlesDeepRecursion: a StackClass::Large job that uses well over
//    64 KiB of stack completes -- verifying the Large routing end-to-end (the
//    same recursion would overflow a Small stack).

#include <jaf/Jaf.hpp>

#include <gtest/gtest.h>

#include <atomic>
#include <cstddef>
#include <vector>

namespace {

jaf::SchedulerOptions with_workers(unsigned n) {
    jaf::SchedulerOptions opt;
    opt.worker_count = n;
    return opt;
}

// Consume ~8 KiB of stack per frame, `depth` frames deep. volatile writes at both
// ends of the frame defeat elision; JAF_NOINLINE prevents the recursion from
// collapsing. With depth huge this overflows any bounded stack.
JAF_NOINLINE void use_stack(int depth, volatile char* sink) {
    volatile char buf[8192];
    buf[0]    = static_cast<char>(depth & 0xFF);
    buf[8191] = static_cast<char>(depth & 0xFF);
    if (sink) *sink = buf[0];
    if (depth > 0) use_stack(depth - 1, &buf[0]);
}

} // namespace

#if JAF_GUARDED_STACKS
TEST(StackGuardDeathTest, SmallStackOverflowFaults) {
    EXPECT_DEATH(
        {
            // L1 Fiber default stack is Small (64 KiB); unbounded recursion runs
            // it into the guard page.
            jaf::Fiber f([] { use_stack(1000000, nullptr); });
            f.resume();
        },
        "");   // access violation: no message, only "the child died" matters
}
#endif

TEST(StackGuard, LargeStackHandlesDeepRecursion) {
    jaf::Scheduler sched(with_workers(2));
    std::atomic<bool> done{false};
    sched.run([&] {
        jaf::Counter c;
        std::vector<jaf::Task> jobs;
        jobs.emplace_back([&] {
            use_stack(20, nullptr);            // ~160 KiB: overflows Small, fits Large
            done.store(true, std::memory_order_relaxed);
        });
        jaf::run(jobs, &c, jaf::StackClass::Large);
        jaf::wait_for_counter(&c, 0);
    });
    EXPECT_TRUE(done.load());
}
