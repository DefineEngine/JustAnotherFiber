#pragma once

// scheduler/IoService.hpp -- owns one Windows I/O completion port and one
// dedicated thread that drains it.

#include <jaf/Io.hpp>   // jaf::io::ReadRequest / IoResult

#include <thread>

namespace jaf {
class Counter;
namespace detail {

struct SchedulerImpl;

class IoService {
public:
    /// @param sched owning scheduler, for outstanding-I/O accounting.
    explicit IoService(SchedulerImpl* sched);
    ~IoService();

    IoService(const IoService&)            = delete;
    IoService& operator=(const IoService&) = delete;

    /// @brief Post an overlapped read. On completion the I/O thread fills `out`
    ///        (if non-null) and calls jaf::signal(*on_complete).
    void PostRead(const io::ReadRequest& req, Counter* on_complete, io::IoResult* out);

private:
    void Loop();   // defined in Io.cpp; drains the completion port until shutdown

    SchedulerImpl* m_sched = nullptr;   ///< io_inflight accounting.
    void*          m_port  = nullptr;   ///< HANDLE (opaque here; real type in Io.cpp).
    std::thread    m_thread;
};

} // namespace detail
} // namespace jaf
