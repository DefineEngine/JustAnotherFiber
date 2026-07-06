#include "scheduler/Worker.hpp"

#include "scheduler/PooledFiber.hpp"
#include "scheduler/Runtime.hpp"
#include "scheduler/SchedulerImpl.hpp"
#include "scheduler/Sentinels.hpp"
#include "context/FContext.hpp"

#include <jaf/Counter.hpp>

#include <chrono>
#include <cstdint>
#include <cwchar>
#include <utility>

#if JAF_OS_WINDOWS
#  define WIN32_LEAN_AND_MEAN
#  define NOMINMAX
#  include <windows.h>
#endif

namespace jaf::detail {

Worker::Worker(SchedulerImpl* sched, unsigned index) noexcept
    : m_sched(sched), m_index(index) {}

void Worker::Start() { m_thread = std::thread([this] { Run(); }); }

void Worker::Join() {
    if (m_thread.joinable()) m_thread.join();
}

void Worker::Pin(unsigned core) noexcept {
#if JAF_OS_WINDOWS
    const DWORD_PTR mask = DWORD_PTR{1} << (core % (sizeof(DWORD_PTR) * 8));
    ::SetThreadAffinityMask(static_cast<HANDLE>(m_thread.native_handle()), mask);
#else
    (void)core;
#endif
}

namespace {
constexpr std::size_t PrioIndex(Priority p) noexcept { return static_cast<std::size_t>(p); }
} // namespace

void Worker::PushFiber(PooledFiber* f) {
    {
        std::lock_guard<std::mutex> lk(m_mutex);
        m_ready_fibers[PrioIndex(f->m_priority)].push_back(f);
        // Bump `pending` inside the lock so it can never be observed as 0 while an
        // item sits in the queue.
        m_sched->pending.fetch_add(1, std::memory_order_release);
    }
    m_sched->NotifyOne();
}

void Worker::PushJob(JobItem&& item) {
    const std::size_t idx = PrioIndex(item.prio);
    {
        std::lock_guard<std::mutex> lk(m_mutex);
        m_ready_jobs[idx].push_back(std::move(item));
        m_sched->pending.fetch_add(1, std::memory_order_release);
    }
    m_sched->NotifyOne();
}

PooledFiber* Worker::PopFiber() noexcept {
    std::lock_guard<std::mutex> lk(m_mutex);
    for (auto& q : m_ready_fibers) {
        if (q.empty()) continue;
        PooledFiber* f = q.back();
        q.pop_back();
        m_sched->pending.fetch_sub(1, std::memory_order_acq_rel);
        return f;
    }
    return nullptr;
}

bool Worker::PopJob(JobItem& out) noexcept {
    std::lock_guard<std::mutex> lk(m_mutex);
    for (auto& q : m_ready_jobs) {
        if (q.empty()) continue;
        out = std::move(q.front());
        q.pop_front();
        m_sched->pending.fetch_sub(1, std::memory_order_acq_rel);
        return true;
    }
    return false;
}

PooledFiber* Worker::StealFiber() noexcept {
    std::lock_guard<std::mutex> lk(m_mutex);
    for (auto& q : m_ready_fibers) {
        if (q.empty()) continue;
        PooledFiber* f = q.front();
        q.pop_front();
        m_sched->pending.fetch_sub(1, std::memory_order_acq_rel);
        return f;
    }
    return nullptr;
}

bool Worker::StealJob(JobItem& out) noexcept {
    std::lock_guard<std::mutex> lk(m_mutex);
    for (auto& q : m_ready_jobs) {
        if (q.empty()) continue;
        out = std::move(q.front());
        q.pop_front();
        m_sched->pending.fetch_sub(1, std::memory_order_acq_rel);
        return true;
    }
    return false;
}

bool Worker::TrySteal() noexcept {
    const unsigned n = m_sched->StealCount();
    for (unsigned i = 1; i < n; ++i) {
        Worker* victim = m_sched->workers[(m_index + i) % n].get();
        if (PooledFiber* f = victim->StealFiber()) {
            m_steals.fetch_add(1, std::memory_order_relaxed);
            PushFiber(f);
            return true;
        }
        JobItem item;
        if (victim->StealJob(item)) {
            m_steals.fetch_add(1, std::memory_order_relaxed);
            PushJob(std::move(item));
            return true;
        }
    }
    return false;
}

void Worker::ResumeFiber(PooledFiber* f) noexcept {
    f->m_state = FiberState::Running;
    SetCurrentFiber(f);
    transfer_t t = jump_fcontext(f->m_ctx, f);
    SetCurrentFiber(nullptr);

    if (reinterpret_cast<std::uintptr_t>(t.data) == kFinished) {
        m_jobs_run.fetch_add(1, std::memory_order_relaxed);
        f->m_ctx = t.fctx;
        Counter*   c      = f->m_owning_counter;
        const bool failed = f->m_job_failed;   // capture before Release clears it
        f->m_owning_counter = nullptr;
        m_sched->pool.Release(f);
        if (c) {
            // Poison BEFORE the completion so a waiter woken by this decrement
            // observes the failure at the join.
            if (failed) CounterPoison(*c);
            CounterCompleteOne(*c);
        }
        m_sched->OnJobComplete();
        m_sched->NotifyOne();
    }
    // kParked: OnParked already placed the fiber and set f->m_ctx. Nothing to do.
}

void Worker::Run() {
#if JAF_OS_WINDOWS
    wchar_t name[32];
    std::swprintf(name, 32, L"jaf-worker-%u", m_index);
    ::SetThreadDescription(::GetCurrentThread(), name);
#endif
    m_sched->active_loops.fetch_add(1, std::memory_order_acq_rel);
    SetCurrentWorker(this);
    RunLoop(/*caller=*/false);
    SetCurrentWorker(nullptr);
    m_sched->active_loops.fetch_sub(1, std::memory_order_acq_rel);
}

void Worker::PumpUntilDrained() {
    m_sched->active_loops.fetch_add(1, std::memory_order_acq_rel);
    SetCurrentWorker(this);
    RunLoop(/*caller=*/true);
    SetCurrentWorker(nullptr);
    m_sched->active_loops.fetch_sub(1, std::memory_order_acq_rel);
}

void Worker::RunLoop(bool caller) {
    bool                                  stalling = false;
    std::chrono::steady_clock::time_point stalled_since{};
    std::uint64_t                         gen_at_stall = 0;

    bool                                  dl_tracking = false;
    std::chrono::steady_clock::time_point dl_since{};
    std::uint64_t                         dl_gen = 0;

    auto end_stall = [&]() noexcept {
        if (stalling) {
            m_sched->blocked_workers.fetch_sub(1, std::memory_order_acq_rel);
            stalling = false;
        }
    };

    for (;;) {
        // 1. Resume a woken (parked-then-ready) fiber first.
        if (PooledFiber* f = PopFiber()) {
            end_stall();
            ResumeFiber(f);
            continue;
        }

        // 2. Start a queued job on a fresh fiber.
        JobItem item;
        if (PopJob(item)) {
            PooledFiber* f = m_sched->pool.Acquire(item.cls);
            if (f) {
                end_stall();
                f->m_job            = std::move(item.job);
                f->m_owning_counter = item.counter;
                f->m_priority       = item.prio;
                f->m_cancel         = item.cancel;
                f->m_name           = item.name;
                ResumeFiber(f);
            } else {
                // Pool exhausted for this class: requeue and back off. If every
                // active loop is stalled with no pool progress for the whole
                // window, that is a genuine livelock -- abort with a diagnostic.
                const std::uint64_t gen = m_sched->pool.ReleaseGeneration();
                const auto          now = std::chrono::steady_clock::now();
                if (!stalling) {
                    stalling      = true;
                    stalled_since = now;
                    gen_at_stall  = gen;
                    m_sched->blocked_workers.fetch_add(1, std::memory_order_acq_rel);
                } else if (gen != gen_at_stall) {
                    stalled_since = now;
                    gen_at_stall  = gen;
                } else if (now - stalled_since > m_sched->pool_exhaustion_timeout &&
                           m_sched->blocked_workers.load(std::memory_order_acquire) >=
                               m_sched->active_loops.load(std::memory_order_acquire)) {
                    m_sched->pool.ReportExhaustion(item.cls);
                }
                PushJob(std::move(item));
                m_sched->WorkerBlockedWait();
            }
            continue;
        }

        // 3. Steal from a sibling.
        if (TrySteal()) {
            end_stall();
            continue;
        }

        // 4. Nothing to do: exit per role, else park briefly.
        end_stall();

        if (m_sched->timed_waiters.load(std::memory_order_relaxed) > 0) {
            m_sched->SweepTimers();
        }

        if (caller) {
            if (m_sched->inflight.load(std::memory_order_acquire) == 0) break;

            if (m_sched->enable_watchdog) {
                const std::uint64_t gen = m_sched->pool.ReleaseGeneration();
                const auto          now = std::chrono::steady_clock::now();
                // Key the "stuck" window on a Release advancing the generation, not
                // on an instantaneous LooksDeadlocked(), so transient idle dips as
                // background workers wake and re-park do not reset it.
                if (dl_tracking && gen != dl_gen) dl_tracking = false;
                if (m_sched->LooksDeadlocked()) {
                    if (!dl_tracking) {
                        dl_tracking = true;
                        dl_since    = now;
                        dl_gen      = gen;
                    } else if (now - dl_since > m_sched->pool_exhaustion_timeout) {
                        m_sched->DumpStuckState(
                            "deadlock -- all workers idle with work outstanding and no progress "
                            "(a fiber is likely parked on a counter nothing will satisfy)");
                    }
                }
            }
        } else {
            if (m_sched->draining.load(std::memory_order_acquire) &&
                m_sched->pending.load(std::memory_order_acquire) == 0) {
                break;
            }
        }
        m_sched->idle_workers.fetch_add(1, std::memory_order_acq_rel);
        m_sched->WorkerIdleWait();
        m_sched->idle_workers.fetch_sub(1, std::memory_order_acq_rel);
    }

    end_stall();
}

void Worker::DrainOnce() {
    SetCurrentWorker(this);
    for (;;) {
        if (PooledFiber* f = PopFiber()) {
            ResumeFiber(f);
            continue;
        }
        JobItem item;
        if (!PopJob(item)) break;
        PooledFiber* f = m_sched->pool.Acquire(item.cls);
        if (f == nullptr) {
            PushJob(std::move(item));
            break;
        }
        f->m_job            = std::move(item.job);
        f->m_owning_counter = item.counter;
        f->m_priority       = item.prio;
        f->m_cancel         = item.cancel;
        f->m_name           = item.name;
        f->m_home           = this;
        ResumeFiber(f);
    }
    SetCurrentWorker(nullptr);
}

} // namespace jaf::detail
