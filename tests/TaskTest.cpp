// Tests for jaf::Task -- the move-only SBO callable (jaf/Task.hpp).

#include <jaf/Task.hpp>

#include <gtest/gtest.h>

#include <array>
#include <cstddef>
#include <memory>
#include <utility>

using jaf::Task;

TEST(Task, DefaultIsEmpty) {
    Task t;
    EXPECT_FALSE(static_cast<bool>(t));

    Task n = nullptr;
    EXPECT_FALSE(static_cast<bool>(n));
}

TEST(Task, StoresAndInvokes) {
    int calls = 0;
    Task t = [&calls] { ++calls; };
    EXPECT_TRUE(static_cast<bool>(t));
    t();
    t();
    EXPECT_EQ(calls, 2);
}

TEST(Task, MoveLeavesSourceEmpty) {
    int calls = 0;
    Task src = [&calls] { ++calls; };
    Task dst = std::move(src);

    EXPECT_FALSE(static_cast<bool>(src));
    EXPECT_TRUE(static_cast<bool>(dst));
    dst();
    EXPECT_EQ(calls, 1);
}

TEST(Task, MoveAssignReplacesTarget) {
    int a = 0, b = 0;
    Task t = [&a] { ++a; };
    t = [&b] { ++b; };           // old target must be destroyed, new one installed
    t();
    EXPECT_EQ(a, 0);
    EXPECT_EQ(b, 1);
}

TEST(Task, ResetClears) {
    int calls = 0;
    Task t = [&calls] { ++calls; };
    t.reset();
    EXPECT_FALSE(static_cast<bool>(t));
}

TEST(Task, LargeCaptureGoesToHeapAndInvokes) {
    // A capture larger than the inline buffer must fall back to the heap.
    std::array<std::size_t, 32> payload{};   // 256 bytes >> kBufferSize
    for (std::size_t i = 0; i < payload.size(); ++i) payload[i] = i;

    std::size_t sum = 0;
    Task t = [payload, &sum] {
        for (std::size_t v : payload) sum += v;
    };
    static_assert(sizeof(payload) > Task::kBufferSize, "test intends the heap path");
    t();
    EXPECT_EQ(sum, 31u * 32u / 2u);   // 0+1+...+31
}

TEST(Task, MoveOnlyCaptureSurvivesMove) {
    // std::function cannot hold this; Task can. The captured unique_ptr must move
    // intact through Task's own move.
    auto p = std::make_unique<int>(42);
    int observed = 0;
    Task src = [p = std::move(p), &observed] { observed = *p; };

    Task dst = std::move(src);
    EXPECT_FALSE(static_cast<bool>(src));
    dst();
    EXPECT_EQ(observed, 42);
}

namespace {
struct DestructionCounter {
    int* count;
    explicit DestructionCounter(int* c) : count(c) {}
    DestructionCounter(DestructionCounter&& o) noexcept : count(o.count) { o.count = nullptr; }
    DestructionCounter& operator=(DestructionCounter&&) = delete;
    ~DestructionCounter() { if (count) ++*count; }
    void operator()() const {}
};
} // namespace

TEST(Task, DestroysStoredCallableExactlyOnce) {
    int destructions = 0;
    {
        Task t = DestructionCounter{&destructions};
        Task moved = std::move(t);   // moving must not double-destroy
        (void)moved;
    }
    EXPECT_EQ(destructions, 1);
}

TEST(TaskDeathTest, InvokeEmptyAborts) {
    // C-6c: invoking an empty/moved-from Task calls through a null vtable.
    Task t;
    EXPECT_DEATH({ t(); }, "empty or moved-from");
}
