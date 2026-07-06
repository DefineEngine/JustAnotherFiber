#include <jaf/Scheduler.hpp>

#include <jaf/Config.hpp>
#include <jaf/Counter.hpp>

#include "scheduler/PooledFiber.hpp"
#include "scheduler/Runtime.hpp"
#include "scheduler/SchedulerImpl.hpp"
#include "scheduler/Sentinels.hpp"
#include "context/FContext.hpp"

#include <chrono>
#include <cstdint>
#include <exception>
#include <thread>
#include <utility>

namespace jaf {
namespace detail {

namespace {
constexpr auto kIdleBackoff    = std::chrono::microseconds(200);
constexpr auto kBlockedBackoff = std::chrono::microseconds(100);
} // namespace

SchedulerImpl::SchedulerImpl(const SchedulerOptions& opts)
    : pool(opts.small_fibers, opts.large_fibers),
      caller_is_worker(opts.caller_is_worker),
      pin_threads(opts.pin_threads),
      enable_watchdog(opts.enable_watchdog),
      pool_exhaustion_timeout(opts.pool_exhaustion_timeout) {
    unsigned n = opts.worker_count != 0
                     ? static_cast<unsigned>(opts.worker_count)
                     : std::thread::hardware_concurrency();
    if (n == 0) n = 1;
    bg_count = n;
    const unsigned total = n + (caller_is_worker ? 1u : 0u);
    workers.reserve(total);
    for (unsigned i = 0; i < total; ++i) {
        workers.push_back(std::make_unique<Worker>(this, i));
    }
    for (std::size_t t = 0; t < named_workers.size(); ++t) {
        named_workers[t] = std::make_unique<Worker>(this, static_cast<unsigned>(total + t));
    }
}

SchedulerImpl::~SchedulerImpl() = default;

void SchedulerImpl::Start() {
    io = std::make_unique<IoService>(this);
    for (unsigned i = 0; i < bg_count; ++i) workers[i]->Start();
    if (pin_threads) {
        for (unsigned i = 0; i < bg_count; ++i) workers[i]->Pin(i);
    }
}

void SchedulerImpl::Submit(JobItem&& item, Worker* from) {
    Worker* target = from;
    if (target == nullptr) {
        const unsigned idx =
            next_worker.fetch_add(1, std::memory_order_relaxed) % WorkerCount();
        target = workers[idx].get();
    }
    target->PushJob(std::move(item));
}

void SchedulerImpl::ScheduleReady(PooledFiber* f) noexcept {
    Worker* w = f->m_home;
    if (w == nullptr) {
        w = CurrentWorker();
        if (w == nullptr) {
            const unsigned idx =
                next_worker.fetch_add(1, std::memory_order_relaxed) % WorkerCount();
            w = workers[idx].get();
        }
    }
    w->PushFiber(f);
}

void SchedulerImpl::OnJobComplete() noexcept {
    if (inflight.fetch_sub(1, std::memory_order_acq_rel) - 1 == 0) {
        {
            std::lock_guard<std::mutex> lk(done_mutex);
            done_cv.notify_all();
        }
        WakeAllWorkers();
    }
}

void SchedulerImpl::CaptureException(std::exception_ptr e) noexcept {
    std::lock_guard<std::mutex> lk(exc_mutex);
    if (!first_exception) first_exception = e;
}

void SchedulerImpl::DumpStuckState(const char* reason) noexcept {
    const FiberPool::ClassOccupancy sm = pool.Occupancy(StackClass::Small);
    const FiberPool::ClassOccupancy lg = pool.Occupancy(StackClass::Large);
    std::fprintf(stderr,
                 "JAF watchdog: %s\n"
                 "  fibers: Small %zu/%zu in use, Large %zu/%zu in use\n"
                 "  inflight=%lld pending=%lld io_inflight=%lld  idle_workers=%u/%u\n",
                 reason, sm.in_use, sm.total, lg.in_use, lg.total,
                 static_cast<long long>(inflight.load(std::memory_order_acquire)),
                 static_cast<long long>(pending.load(std::memory_order_acquire)),
                 static_cast<long long>(io_inflight.load(std::memory_order_acquire)),
                 idle_workers.load(std::memory_order_acquire), bg_count);
    std::abort();
}

void SchedulerImpl::WorkerIdleWait() noexcept {
    std::unique_lock<std::mutex> lk(sleep_mutex);
    work_cv.wait_for(lk, kIdleBackoff, [this] {
        return pending.load(std::memory_order_acquire) > 0 ||
               draining.load(std::memory_order_acquire);
    });
}

void SchedulerImpl::WorkerBlockedWait() noexcept {
    std::unique_lock<std::mutex> lk(sleep_mutex);
    work_cv.wait_for(lk, kBlockedBackoff);
}

void SchedulerImpl::RegisterTimer(TimedWaiter* t) noexcept {
    std::lock_guard<std::mutex> lk(timer_mutex);
    t->prev = nullptr;
    t->next = timer_head;
    if (timer_head) timer_head->prev = t;
    timer_head = t;
    t->linked  = true;
    timed_waiters.fetch_add(1, std::memory_order_relaxed);
}

void SchedulerImpl::RemoveTimer(TimedWaiter* t) noexcept {
    std::lock_guard<std::mutex> lk(timer_mutex);
    if (!t->linked) return;
    if (t->prev) t->prev->next = t->next; else timer_head = t->next;
    if (t->next) t->next->prev = t->prev;
    t->prev = t->next = nullptr;
    t->linked = false;
    timed_waiters.fetch_sub(1, std::memory_order_relaxed);
}

void SchedulerImpl::SweepTimers() noexcept {
    const auto now = std::chrono::steady_clock::now();
    // Hold timer_mutex across the whole sweep so a resumed fiber's stack-resident
    // TimedWaiter cannot be reclaimed while we still touch it.
    std::lock_guard<std::mutex> lk(timer_mutex);
    for (TimedWaiter* t = timer_head; t != nullptr;) {
        TimedWaiter* next = t->next;
        if (now >= t->deadline) {
            if (CounterTryUnlinkWaiter(*t->counter, &t->w)) {
                t->timed_out = true;
                ScheduleReady(t->fiber);
            }
            if (t->prev) t->prev->next = t->next; else timer_head = t->next;
            if (t->next) t->next->prev = t->prev;
            t->prev = t->next = nullptr;
            t->linked = false;
            timed_waiters.fetch_sub(1, std::memory_order_relaxed);
        }
        t = next;
    }
}

void ScheduleReady(PooledFiber* f) noexcept { ActiveScheduler()->ScheduleReady(f); }

void CaptureCurrentException(std::exception_ptr e) noexcept {
    ActiveScheduler()->CaptureException(e);
}

unsigned ActiveWorkerCount() noexcept {
    SchedulerImpl* s = ActiveScheduler();
    return s ? s->WorkerCount() : 0u;
}

void CoroInflightBegin() noexcept {
    ActiveScheduler()->inflight.fetch_add(1, std::memory_order_relaxed);
}
void CoroInflightEnd() noexcept {
    ActiveScheduler()->OnJobComplete();
}

} // namespace detail

namespace {
// Runs on the worker stack, immediately after the parking fiber switched off its
// own stack via ontop_fcontext. Registering here -- not before the switch -- is
// what prevents another worker from resuming a not-yet-suspended stack.
detail::transfer_t OnParkedGeneric(detail::transfer_t t) noexcept {
    auto* self    = static_cast<detail::PooledFiber*>(t.data);
    self->m_ctx   = t.fctx;
    self->m_state = detail::FiberState::Parked;

    if (!self->m_park_link(self, self->m_park_ctx)) {
        detail::ScheduleReady(self);
    }
    return detail::transfer_t{nullptr, reinterpret_cast<void*>(detail::kParked)};
}

bool LinkCounterWaiter(detail::PooledFiber* self, void* ctx) noexcept {
    return detail::CounterAddWaiter(*static_cast<Counter*>(ctx), &self->m_waiter);
}

void TimedWake(void* ctx) noexcept {
    auto* t = static_cast<detail::TimedWaiter*>(ctx);
    t->timed_out = false;
    detail::ScheduleReady(t->fiber);
}

bool LinkTimedWaiter(detail::PooledFiber* self, void* ctx) noexcept {
    (void)self;
    auto* t = static_cast<detail::TimedWaiter*>(ctx);
    if (!detail::CounterAddWaiter(*t->counter, &t->w)) return false;
    detail::ActiveScheduler()->RegisterTimer(t);
    return true;
}

struct BlockingWaiter {
    detail::Waiter          w;
    std::mutex              m;
    std::condition_variable cv;
    bool                    done = false;
};

void BlockingWake(void* ctx) noexcept {
    auto* b = static_cast<BlockingWaiter*>(ctx);
    {
        std::lock_guard<std::mutex> lk(b->m);
        b->done = true;
    }
    b->cv.notify_one();
}
} // namespace

namespace detail {
void ParkCurrentFiber(ParkLinkFn link, void* ctx) noexcept {
    PooledFiber* self = CurrentFiber();
    self->m_park_link = link;
    self->m_park_ctx  = ctx;
    transfer_t back   = ontop_fcontext(self->m_worker_ctx, self, &OnParkedGeneric);
    self->m_worker_ctx = back.fctx;
    self->m_park_link  = nullptr;
    self->m_park_ctx   = nullptr;
}
} // namespace detail

void wait_for_counter(Counter* counter, std::int64_t target) {
    detail::PooledFiber* self = detail::CurrentFiber();
    JAF_ASSERT(self != nullptr,
               "jaf::wait_for_counter must be called from inside a fiber/job");
#ifndef NDEBUG
    JAF_ASSERT(self->m_locks_held == 0,
               "jaf::wait_for_counter called while holding a jaf::Mutex (see Thread-migration rules)");
#endif

    self->m_wait_counter  = counter;
    self->m_waiter.target = target;
    detail::ParkCurrentFiber(&LinkCounterWaiter, counter);
    self->m_wait_counter = nullptr;
}

bool wait_for_counter_checked(Counter* counter, std::int64_t target) {
    wait_for_counter(counter, target);
    return !counter->poisoned();
}

bool try_wait_for_counter(Counter* counter, std::int64_t target) noexcept {
    return detail::CounterTrySatisfied(*counter, target);
}

bool wait_for_counter_until(Counter* counter, std::int64_t target,
                            std::chrono::steady_clock::time_point deadline) {
    detail::PooledFiber* self = detail::CurrentFiber();
    JAF_ASSERT(self != nullptr,
               "jaf::wait_for_counter_until must be called from inside a fiber/job");
#ifndef NDEBUG
    JAF_ASSERT(self->m_locks_held == 0,
               "jaf::wait_for_counter_until called while holding a jaf::Mutex (see Thread-migration rules)");
#endif

    detail::TimedWaiter t;
    t.w.target   = target;
    t.w.fiber    = nullptr;
    t.w.wake     = &TimedWake;
    t.w.wake_ctx = &t;
    t.fiber      = self;
    t.counter    = counter;
    t.deadline   = deadline;

    self->m_wait_counter = counter;
    detail::ParkCurrentFiber(&LinkTimedWaiter, &t);
    self->m_wait_counter = nullptr;

    detail::ActiveScheduler()->RemoveTimer(&t);
    return !t.timed_out;
}

bool wait_for_counter_blocking(Counter* counter, std::int64_t target) {
    JAF_ASSERT(detail::CurrentFiber() == nullptr,
               "jaf::wait_for_counter_blocking must NOT be called from inside a fiber "
               "(use jaf::wait_for_counter)");

    BlockingWaiter b;
    b.w.target   = target;
    b.w.fiber    = nullptr;
    b.w.wake     = &BlockingWake;
    b.w.wake_ctx = &b;
    if (!detail::CounterAddWaiter(*counter, &b.w)) return true;

    std::unique_lock<std::mutex> lk(b.m);
    b.cv.wait(lk, [&] { return b.done; });
    return true;
}

bool wait_for_counter_blocking_until(Counter* counter, std::int64_t target,
                                     std::chrono::steady_clock::time_point deadline) {
    JAF_ASSERT(detail::CurrentFiber() == nullptr,
               "jaf::wait_for_counter_blocking_until must NOT be called from inside a fiber "
               "(use jaf::wait_for_counter_until)");

    BlockingWaiter b;
    b.w.target   = target;
    b.w.fiber    = nullptr;
    b.w.wake     = &BlockingWake;
    b.w.wake_ctx = &b;
    if (!detail::CounterAddWaiter(*counter, &b.w)) return true;

    {
        std::unique_lock<std::mutex> lk(b.m);
        b.cv.wait_until(lk, deadline, [&] { return b.done; });
    }

    // Whoever unlinks the waiter under the counter lock owns the timeout outcome.
    if (detail::CounterTryUnlinkWaiter(*counter, &b.w)) return false;
    std::unique_lock<std::mutex> lk(b.m);
    b.cv.wait(lk, [&] { return b.done; });
    return true;
}

void run(std::span<Task> jobs, Counter* counter, StackClass stack_class, Priority prio,
         const char* name) {
    detail::SchedulerImpl* s = detail::ActiveScheduler();
    JAF_ASSERT(s != nullptr, "jaf::run called with no active jaf::Scheduler");

    const std::int64_t n = static_cast<std::int64_t>(jobs.size());
    if (n == 0) return;

    if (counter) detail::CounterAdd(*counter, n);
    s->inflight.fetch_add(n, std::memory_order_relaxed);

    detail::Worker* from = detail::CurrentWorker();
    for (auto& job : jobs) {
        s->Submit(detail::JobItem{std::move(job), counter, stack_class, prio,
                                  /*cancel*/ nullptr, name},
                  from);
    }
}

void run(std::span<Task> jobs, Counter* counter, const CancellationToken& token,
         StackClass stack_class, Priority prio) {
    detail::SchedulerImpl* s = detail::ActiveScheduler();
    JAF_ASSERT(s != nullptr, "jaf::run called with no active jaf::Scheduler");

    const std::int64_t n = static_cast<std::int64_t>(jobs.size());
    if (n == 0) return;

    if (counter) detail::CounterAdd(*counter, n);
    s->inflight.fetch_add(n, std::memory_order_relaxed);

    std::atomic<bool>* cancel = token.internal_flag();
    detail::Worker*    from   = detail::CurrentWorker();
    for (auto& job : jobs) {
        s->Submit(detail::JobItem{std::move(job), counter, stack_class, prio, cancel}, from);
    }
}

namespace this_job {
bool cancelled() noexcept {
    detail::PooledFiber* f = detail::CurrentFiber();
    return f != nullptr && f->m_cancel != nullptr &&
           f->m_cancel->load(std::memory_order_acquire);
}
} // namespace this_job

void run_on(ThreadTag tag, std::span<Task> jobs, Counter* counter, StackClass stack_class,
            Priority prio, const char* name) {
    detail::SchedulerImpl* s = detail::ActiveScheduler();
    JAF_ASSERT(s != nullptr, "jaf::run_on called with no active jaf::Scheduler");

    const std::int64_t n = static_cast<std::int64_t>(jobs.size());
    if (n == 0) return;

    if (counter) detail::CounterAdd(*counter, n);
    s->inflight.fetch_add(n, std::memory_order_relaxed);

    detail::Worker* nw = s->NamedWorker(tag);
    for (auto& job : jobs) {
        nw->PushJob(detail::JobItem{std::move(job), counter, stack_class, prio,
                                    /*cancel*/ nullptr, name});
    }
}

Scheduler::Scheduler(SchedulerOptions options)
    : m_impl(std::make_unique<detail::SchedulerImpl>(options)) {
    JAF_ASSERT(detail::ActiveScheduler() == nullptr,
               "only one jaf::Scheduler may be active at a time");
    detail::SetActiveScheduler(m_impl.get());
    m_impl->Start();
}

Scheduler::~Scheduler() {
    m_impl->draining.store(true, std::memory_order_release);
    // Stop the I/O thread first so no late completion signals a counter / pushes a
    // fiber onto a worker that is about to join.
    m_impl->io.reset();
    m_impl->WakeAllWorkers();
    for (auto& w : m_impl->workers) w->Join();
    detail::SetActiveScheduler(nullptr);
}

void Scheduler::run(Task root) {
    m_impl->inflight.fetch_add(1, std::memory_order_relaxed);
    m_impl->Submit(detail::JobItem{std::move(root), nullptr, StackClass::Small, Priority::High},
                   nullptr);

    if (detail::Worker* caller = m_impl->CallerWorker()) {
        caller->PumpUntilDrained();
    } else if (m_impl->enable_watchdog) {
        std::unique_lock<std::mutex> lk(m_impl->done_mutex);
        bool          tracking = false;
        std::uint64_t dl_gen   = 0;
        auto          dl_since = std::chrono::steady_clock::now();
        while (m_impl->inflight.load(std::memory_order_acquire) != 0) {
            m_impl->done_cv.wait_for(lk, m_impl->pool_exhaustion_timeout, [this] {
                return m_impl->inflight.load(std::memory_order_acquire) == 0;
            });
            if (m_impl->inflight.load(std::memory_order_acquire) == 0) break;
            const std::uint64_t gen = m_impl->pool.ReleaseGeneration();
            const auto          now = std::chrono::steady_clock::now();
            if (m_impl->LooksDeadlocked()) {
                if (!tracking || gen != dl_gen) {
                    tracking = true;
                    dl_since = now;
                    dl_gen   = gen;
                } else if (now - dl_since > m_impl->pool_exhaustion_timeout) {
                    m_impl->DumpStuckState(
                        "deadlock -- caller idle with work outstanding and no progress");
                }
            } else {
                tracking = false;
            }
        }
    } else {
        std::unique_lock<std::mutex> lk(m_impl->done_mutex);
        m_impl->done_cv.wait(lk, [this] {
            return m_impl->inflight.load(std::memory_order_acquire) == 0;
        });
    }

    std::exception_ptr e;
    {
        std::lock_guard<std::mutex> lk(m_impl->exc_mutex);
        e = m_impl->first_exception;
        m_impl->first_exception = nullptr;
    }
    if (e) std::rethrow_exception(e);
}

void Scheduler::pump(ThreadTag tag) {
    m_impl->NamedWorker(tag)->DrainOnce();
}

Scheduler::Stats Scheduler::stats() const noexcept {
    const auto small = m_impl->pool.Occupancy(StackClass::Small);
    const auto large = m_impl->pool.Occupancy(StackClass::Large);
    Stats s;
    s.small_total  = small.total;
    s.small_in_use = small.in_use;
    s.large_total  = large.total;
    s.large_in_use = large.in_use;
    s.inflight     = m_impl->inflight.load(std::memory_order_acquire);
    s.pending      = m_impl->pending.load(std::memory_order_acquire);
    for (const auto& w : m_impl->workers) {
        s.jobs_run += w->JobsRun();
        s.steals   += w->Steals();
    }
    for (const auto& w : m_impl->named_workers) {
        if (w) { s.jobs_run += w->JobsRun(); s.steals += w->Steals(); }
    }
    return s;
}

} // namespace jaf
