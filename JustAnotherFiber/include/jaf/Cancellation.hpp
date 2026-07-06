#pragma once

// jaf/Cancellation.hpp -- cooperative cancellation via a shared atomic flag.

#include <jaf/Config.hpp>

#include <atomic>
#include <memory>

namespace jaf {

/// A shared, copyable cancellation flag; copies share one flag so any holder can cancel.
class CancellationToken {
public:
    CancellationToken() : m_flag(std::make_shared<std::atomic<bool>>(false)) {}

    /// Request cancellation (idempotent). Safe from any thread.
    void cancel() noexcept { m_flag->store(true, std::memory_order_release); }

    /// True once cancel() has been called on this or a copy.
    [[nodiscard]] bool cancelled() const noexcept {
        return m_flag->load(std::memory_order_acquire);
    }

    /// Internal: the raw flag for the scheduler to observe from a job.
    [[nodiscard]] std::atomic<bool>* internal_flag() const noexcept { return m_flag.get(); }

private:
    std::shared_ptr<std::atomic<bool>> m_flag;
};

namespace this_job {
/// True if the currently-running job's CancellationToken is cancelled.
[[nodiscard]] bool cancelled() noexcept;
} // namespace this_job

} // namespace jaf
