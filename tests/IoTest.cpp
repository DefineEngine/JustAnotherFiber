// F-2: jaf::io async file reads. Uses real temp files (written with std::ofstream,
// not jaf) so the read path is exercised end to end.

#include <jaf/Jaf.hpp>

#include <gtest/gtest.h>

#include <array>
#include <atomic>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <span>
#include <string>
#include <vector>

namespace {

jaf::SchedulerOptions with_workers(unsigned n) {
    jaf::SchedulerOptions opt;
    opt.worker_count = n;
    return opt;
}

// Write `content` to a fresh temp file; returns its path.
std::string write_temp(const std::vector<std::byte>& content, const char* name) {
    const auto path = (std::filesystem::temp_directory_path() / name).string();
    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    f.write(reinterpret_cast<const char*>(content.data()),
            static_cast<std::streamsize>(content.size()));
    return path;
}

std::vector<std::byte> ramp(std::size_t n) {
    std::vector<std::byte> v(n);
    for (std::size_t i = 0; i < n; ++i) v[i] = static_cast<std::byte>(i & 0xFF);
    return v;
}

} // namespace

TEST(Io, ReadFileWakesFiberWithCorrectBytes) {
    const auto content = ramp(4096);
    const auto path    = write_temp(content, "jaf_io_read.bin");

    jaf::Scheduler sched(with_workers(2));
    std::vector<std::byte> buf(content.size());
    jaf::io::IoStatus      status = jaf::io::IoStatus::ReadFailed;
    std::size_t            got    = 0;

    sched.run([&] {
        jaf::Counter    c;
        jaf::io::IoResult r;
        jaf::io::read_file({path.c_str(), std::span<std::byte>(buf), 0}, &c, &r);
        jaf::wait_for_counter(&c, 0);   // parks; I/O thread signals on completion
        status = r.status;
        got    = r.bytes;
    });

    EXPECT_EQ(status, jaf::io::IoStatus::Ok);
    EXPECT_EQ(got, content.size());
    EXPECT_EQ(buf, content);
    std::filesystem::remove(path);
}

TEST(Io, WorkersProgressWhileReadsOutstanding) {
    const auto content = ramp(1 << 16);   // 64 KiB
    const auto path    = write_temp(content, "jaf_io_concurrent.bin");

    jaf::Scheduler sched(with_workers(4));
    std::atomic<int> cpu_done{0};
    constexpr int    kReads   = 4;
    constexpr int    kCpuJobs = 64;
    bool             all_ok   = true;

    sched.run([&] {
        // Kick several async reads; the issuing fiber does NOT block yet.
        jaf::Counter                        io_c;
        std::vector<std::vector<std::byte>> bufs(kReads, std::vector<std::byte>(content.size()));
        std::vector<jaf::io::IoResult>      results(kReads);
        for (int i = 0; i < kReads; ++i) {
            jaf::io::read_file({path.c_str(), std::span<std::byte>(bufs[i]), 0}, &io_c, &results[i]);
        }

        // CPU jobs must complete WHILE the reads are outstanding -- only possible
        // if no worker OS thread is blocked in a kernel read.
        jaf::Counter           cpu_c;
        std::vector<jaf::Task> jobs;
        jobs.reserve(kCpuJobs);
        for (int i = 0; i < kCpuJobs; ++i) {
            jobs.emplace_back([&] { cpu_done.fetch_add(1, std::memory_order_relaxed); });
        }
        jaf::run(jobs, &cpu_c);
        jaf::wait_for_counter(&cpu_c, 0);
        jaf::wait_for_counter(&io_c, 0);

        for (const auto& r : results) {
            if (r.status != jaf::io::IoStatus::Ok) all_ok = false;
        }
    });

    EXPECT_EQ(cpu_done.load(), kCpuJobs);
    EXPECT_TRUE(all_ok);
    std::filesystem::remove(path);
}

TEST(Io, MissingFileSurfacesError) {
    jaf::Scheduler sched(with_workers(2));
    jaf::io::IoStatus status = jaf::io::IoStatus::Ok;
    std::uint32_t     err    = 0;

    sched.run([&] {
        jaf::Counter          c;
        jaf::io::IoResult     r;
        std::array<std::byte, 16> buf{};
        jaf::io::read_file({"Z:\\no\\such\\jaf_missing.bin", std::span<std::byte>(buf), 0}, &c, &r);
        jaf::wait_for_counter(&c, 0);
        status = r.status;
        err    = r.os_error;
    });

    EXPECT_EQ(status, jaf::io::IoStatus::OpenFailed);
    EXPECT_NE(err, 0u);
}
