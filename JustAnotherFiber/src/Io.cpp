#include <jaf/Io.hpp>

#include <jaf/Config.hpp>
#include <jaf/Counter.hpp>

#include "scheduler/IoService.hpp"
#include "scheduler/Runtime.hpp"
#include "scheduler/SchedulerImpl.hpp"

#include <cstddef>

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

namespace jaf::detail {

namespace {
// Distinct completion key posted by ~IoService to unblock and exit the loop.
constexpr ULONG_PTR kShutdownKey = 1;

// Per-request state owned by the I/O thread. OVERLAPPED MUST be the first member
// so we can recover the IoOp* from the lpOverlapped the port hands back.
struct IoOp {
    OVERLAPPED    ov{};
    HANDLE        file     = INVALID_HANDLE_VALUE;
    Counter*      counter  = nullptr;
    io::IoResult* out      = nullptr;
    std::size_t   expected = 0;
};
} // namespace

IoService::IoService(SchedulerImpl* sched) : m_sched(sched) {
    m_port = ::CreateIoCompletionPort(INVALID_HANDLE_VALUE, nullptr, 0, 1);
    JAF_ASSERT(m_port != nullptr, "jaf::io: CreateIoCompletionPort failed");
    m_thread = std::thread([this] { Loop(); });
}

IoService::~IoService() {
    if (m_port != nullptr) {
        ::PostQueuedCompletionStatus(static_cast<HANDLE>(m_port), 0, kShutdownKey, nullptr);
        if (m_thread.joinable()) m_thread.join();
        ::CloseHandle(static_cast<HANDLE>(m_port));
        m_port = nullptr;
    }
}

void IoService::Loop() {
    HANDLE port = static_cast<HANDLE>(m_port);
    for (;;) {
        DWORD        bytes = 0;
        ULONG_PTR    key   = 0;
        OVERLAPPED*  ov    = nullptr;
        const BOOL   ok    = ::GetQueuedCompletionStatus(port, &bytes, &key, &ov, INFINITE);

        if (key == kShutdownKey) break;   // ~IoService asked us to stop
        if (ov == nullptr) continue;      // spurious/error with no associated op

        auto* op = reinterpret_cast<IoOp*>(ov);   // OVERLAPPED is IoOp's first member
        if (op->out != nullptr) {
            if (!ok) {
                op->out->status   = io::IoStatus::ReadFailed;
                op->out->bytes    = 0;
                op->out->os_error = ::GetLastError();
            } else {
                op->out->bytes  = static_cast<std::size_t>(bytes);
                op->out->status = (op->out->bytes < op->expected) ? io::IoStatus::ShortRead
                                                                  : io::IoStatus::Ok;
            }
        }
        ::CloseHandle(op->file);
        Counter* c = op->counter;
        delete op;
        m_sched->io_inflight.fetch_sub(1, std::memory_order_relaxed);
        if (c != nullptr) jaf::signal(*c);   // wake the parked fiber (any thread)
    }
}

void IoService::PostRead(const io::ReadRequest& req, Counter* on_complete, io::IoResult* out) {
    // Helper to report an immediate (pre-completion) failure and wake the waiter.
    auto fail = [&](io::IoStatus status, HANDLE file) {
        if (out != nullptr) {
            out->status   = status;
            out->bytes    = 0;
            out->os_error = ::GetLastError();
        }
        if (file != INVALID_HANDLE_VALUE) ::CloseHandle(file);
        if (on_complete != nullptr) jaf::signal(*on_complete);
    };

    HANDLE file = ::CreateFileA(req.path, GENERIC_READ, FILE_SHARE_READ, nullptr,
                                OPEN_EXISTING, FILE_FLAG_OVERLAPPED, nullptr);
    if (file == INVALID_HANDLE_VALUE) { fail(io::IoStatus::OpenFailed, INVALID_HANDLE_VALUE); return; }

    if (::CreateIoCompletionPort(file, static_cast<HANDLE>(m_port), 0, 0) == nullptr) {
        fail(io::IoStatus::OpenFailed, file);
        return;
    }

    auto* op      = new IoOp();
    op->file      = file;
    op->counter   = on_complete;
    op->out       = out;
    op->expected  = req.dst.size();
    op->ov.Offset     = static_cast<DWORD>(req.offset & 0xFFFFFFFFull);
    op->ov.OffsetHigh = static_cast<DWORD>(req.offset >> 32);

    const BOOL ok = ::ReadFile(file, req.dst.data(),
                               static_cast<DWORD>(req.dst.size()), nullptr, &op->ov);
    if (!ok && ::GetLastError() != ERROR_IO_PENDING) {
        // Real synchronous failure: no completion will be posted for this op.
        if (out != nullptr) {
            out->status   = io::IoStatus::ReadFailed;
            out->bytes    = 0;
            out->os_error = ::GetLastError();
        }
        ::CloseHandle(file);
        delete op;
        if (on_complete != nullptr) jaf::signal(*on_complete);
        return;
    }
    m_sched->io_inflight.fetch_add(1, std::memory_order_relaxed);
}

} // namespace jaf::detail

namespace jaf::io {

void read_file(const ReadRequest& req, Counter* on_complete, IoResult* out) {
    detail::SchedulerImpl* s = detail::ActiveScheduler();
    JAF_ASSERT(s != nullptr, "jaf::io::read_file called with no active jaf::Scheduler");
    JAF_ASSERT(s->io != nullptr, "jaf::io::read_file: I/O service not running");

    // Bump synchronously BEFORE posting, so the completion's signal can never
    // drive the counter below the waiter's target early.
    if (on_complete != nullptr) detail::CounterAdd(*on_complete, 1);
    s->io->PostRead(req, on_complete, out);
}

} // namespace jaf::io
