// C++20 coroutine layer: jaf::Coro<T>, co_spawn, and co_await on a Counter, a
// child Coro, a GraphRun, and jaf::io::co_read.

#include <jaf/Jaf.hpp>

#include <gtest/gtest.h>

#include <array>
#include <atomic>
#include <chrono>
#include <coroutine>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <span>
#include <stdexcept>
#include <string>
#include <thread>

namespace {

jaf::SchedulerOptions with_workers(unsigned n) {
    jaf::SchedulerOptions opt;
    opt.worker_count = n;
    return opt;
}

using namespace std::chrono_literals;

} // namespace

TEST(Coro, AwaitCounterResumes) {
    jaf::Scheduler sched(with_workers(4));
    std::atomic<int> result{0};
    sched.run([&] {
        jaf::Counter done{1};   // event counter: co_spawn signals it once on completion
        jaf::co_spawn(
            [&]() -> jaf::Coro<void> {
                jaf::Counter             c{1};
                std::array<jaf::Task, 1> jobs = {jaf::Task{[&] { jaf::signal(c); }}};
                jaf::run(jobs, nullptr);
                co_await c;   // suspend until c hits 0 -- holds no fiber slot
                result.store(42, std::memory_order_relaxed);
                co_return;
            }(),
            &done);
        jaf::wait_for_counter(&done);
    });
    EXPECT_EQ(result.load(), 42);
}

TEST(Coro, AwaitChildReturnsValue) {
    jaf::Scheduler sched(with_workers(4));
    std::atomic<int> result{0};
    sched.run([&] {
        jaf::Counter done{1};   // event counter: co_spawn signals it once on completion
        jaf::co_spawn(
            [&]() -> jaf::Coro<void> {
                auto child = []() -> jaf::Coro<int> { co_return 7; };
                const int v = co_await child();
                result.store(v, std::memory_order_relaxed);
                co_return;
            }(),
            &done);
        jaf::wait_for_counter(&done);
    });
    EXPECT_EQ(result.load(), 7);
}

TEST(Coro, AwaitGraphRun) {
    jaf::Scheduler sched(with_workers(4));
    std::atomic<int> n{0};
    sched.run([&] {
        jaf::Counter done{1};   // event counter: co_spawn signals it once on completion
        jaf::co_spawn(
            [&]() -> jaf::Coro<void> {
                jaf::Graph g;
                for (int i = 0; i < 8; ++i) g.Emplace([&] { n.fetch_add(1, std::memory_order_relaxed); });
                jaf::GraphRun r = jaf::schedule_async(g);
                co_await r;   // await the whole DAG
                co_return;
            }(),
            &done);
        jaf::wait_for_counter(&done);
    });
    EXPECT_EQ(n.load(), 8);
}

TEST(Coro, ThrowingCoroutineSurfacesViaRun) {
    jaf::Scheduler sched(with_workers(2));
    bool threw = false;
    try {
        sched.run([&] {
            jaf::Counter done{1};   // event counter: co_spawn signals it once on completion
            jaf::co_spawn(
                []() -> jaf::Coro<void> {
                    co_await std::suspend_never{};   // makes this a coroutine
                    throw std::runtime_error("coro boom");
                }(),
                &done);
            jaf::wait_for_counter(&done);
        });
    } catch (const std::runtime_error& e) {
        threw = (std::string(e.what()) == "coro boom");
    }
    EXPECT_TRUE(threw);
}

TEST(Coro, ManySuspendedShareSmallPool) {
    // 200 coroutines suspend on a shared gate while the pool has only 16 fibers. A
    // released thread signals the gate after they have all suspended. If a
    // suspended coroutine held a fiber slot (as a parked fiber does), this would
    // trip the C-2 pool-exhaustion abort; it completes because it does not.
    jaf::SchedulerOptions opt = with_workers(4);
    opt.small_fibers        = 16;
    jaf::Scheduler   sched(opt);
    constexpr int    N = 200;
    jaf::Counter     gate{1};
    std::atomic<int> completed{0};

    std::thread releaser([&] {
        std::this_thread::sleep_for(100ms);
        jaf::signal(gate);
    });

    sched.run([&] {
        jaf::Counter alldone{N};
        for (int i = 0; i < N; ++i) {
            jaf::co_spawn(
                [&]() -> jaf::Coro<void> {
                    co_await gate;
                    completed.fetch_add(1, std::memory_order_relaxed);
                    co_return;
                }(),
                &alldone);
        }
        jaf::wait_for_counter(&alldone, 0);
    });
    releaser.join();
    EXPECT_EQ(completed.load(), N);
}

TEST(Coro, AwaitIoRead) {
    jaf::Scheduler sched(with_workers(2));
    const wchar_t*    path = L"jaf_coro_io_test.bin";
    const std::string data = "hello coro io";
    {
        std::ofstream f(path, std::ios::binary);
        f << data;
    }

    std::array<char, 13> buf{};   // exactly data.size()
    std::atomic<int>     status{-1};
    std::atomic<size_t>  bytes{0};
    sched.run([&] {
        jaf::Counter done{1};   // event counter: co_spawn signals it once on completion
        jaf::co_spawn(
            [&]() -> jaf::Coro<void> {
                jaf::io::ReadRequest req{path, std::as_writable_bytes(std::span<char>(buf)), 0};
                const jaf::io::IoResult r = co_await jaf::io::co_read(req);
                status.store(static_cast<int>(r.status), std::memory_order_relaxed);
                bytes.store(r.bytes, std::memory_order_relaxed);
                co_return;
            }(),
            &done);
        jaf::wait_for_counter(&done);
    });

    EXPECT_EQ(status.load(), static_cast<int>(jaf::io::IoStatus::Ok));
    EXPECT_EQ(bytes.load(), data.size());
    EXPECT_EQ(std::string(buf.data(), data.size()), data);
    std::filesystem::remove(path);
}
