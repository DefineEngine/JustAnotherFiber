#pragma once

// jaf/Task.hpp — a move-only, type-erased void() callable with a small-buffer
// optimization. The unit of work the scheduler runs and the entry a Fiber stores.

#include <jaf/Config.hpp>

#include <concepts>
#include <cstddef>
#include <cstring>
#include <new>
#include <type_traits>
#include <utility>

namespace jaf {

/// A move-only, type-erased callable unit of work. Closures up to kBufferSize
/// bytes with a noexcept move live inline; larger ones are heap-allocated.
class Task {
public:
    static constexpr std::size_t kBufferSize = 48;
    // At least 16 so 16-byte-aligned captures (__m128 / XMVECTOR / game math
    // types) stay inline; alignof(max_align_t) is only 8 on MSVC x64.
    static constexpr std::size_t kBufferAlign =
        (alignof(std::max_align_t) < 16) ? 16 : alignof(std::max_align_t);

private:
    struct Ops {
        void (*Move)(void* dst, void* src) noexcept;
        void (*Destroy)(void* p) noexcept;
        void (*Invoke)(void* p);
    };

    template <class T>
    static const Ops* InlineOps() noexcept {
        static const Ops ops = {
            [](void* dst, void* src) noexcept {
                T* s = std::launder(reinterpret_cast<T*>(src));
                ::new (dst) T(std::move(*s));
            },
            [](void* p) noexcept { std::launder(reinterpret_cast<T*>(p))->~T(); },
            [](void* p) { (*std::launder(reinterpret_cast<T*>(p)))(); },
        };
        return &ops;
    }

    template <class T>
    static const Ops* HeapOps() noexcept {
        static const Ops ops = {
            [](void* dst, void* src) noexcept {
                void* ptr = nullptr;
                std::memcpy(&ptr, src, sizeof ptr);
                std::memcpy(dst, &ptr, sizeof ptr);
                void* nil = nullptr;
                std::memcpy(src, &nil, sizeof nil);   // src becomes a null T* -> Destroy is a no-op
            },
            [](void* p) noexcept {
                T* obj = nullptr;
                std::memcpy(&obj, p, sizeof obj);
                delete obj;
            },
            [](void* p) {
                T* obj = nullptr;
                std::memcpy(&obj, p, sizeof obj);
                (*obj)();
            },
        };
        return &ops;
    }

    alignas(kBufferAlign) std::byte m_buf[kBufferSize];
    const Ops*                      m_ops = nullptr;

    void MoveFrom(Task& o) noexcept {
        m_ops = o.m_ops;
        if (m_ops) {
            m_ops->Move(m_buf, o.m_buf);
            m_ops->Destroy(o.m_buf);
            o.m_ops = nullptr;
        }
    }

public:
    Task() noexcept = default;
    Task(std::nullptr_t) noexcept {}

    /// Wrap any object invocable as void(). Stored inline when small and nothrow
    /// move constructible, else heap-allocated.
    template <class F, class D = std::decay_t<F>>
        requires(!std::same_as<D, Task> && std::invocable<D&>)
    Task(F&& f) {
        if constexpr (sizeof(D) <= kBufferSize && alignof(D) <= kBufferAlign &&
                      std::is_nothrow_move_constructible_v<D>) {
            ::new (static_cast<void*>(m_buf)) D(std::forward<F>(f));
            m_ops = InlineOps<D>();
        } else {
            D* p = new D(std::forward<F>(f));
            std::memcpy(m_buf, &p, sizeof p);
            m_ops = HeapOps<D>();
        }
    }

    Task(Task&& o) noexcept { MoveFrom(o); }
    Task& operator=(Task&& o) noexcept {
        if (this != &o) {
            reset();
            MoveFrom(o);
        }
        return *this;
    }

    Task(const Task&)            = delete;
    Task& operator=(const Task&) = delete;

    ~Task() { reset(); }

    /// Destroy the wrapped callable and become empty.
    void reset() noexcept {
        if (m_ops) {
            m_ops->Destroy(m_buf);
            m_ops = nullptr;
        }
    }

    /// Invoke the callable. Precondition: not empty.
    void operator()() {
        JAF_ASSERT(m_ops != nullptr, "jaf::Task invoked while empty or moved-from");
        m_ops->Invoke(m_buf);
    }

    [[nodiscard]] explicit operator bool() const noexcept { return m_ops != nullptr; }
};

static_assert(sizeof(Task) == 64, "Task should stay 64 bytes (SBO buffer + ops ptr)");
static_assert(alignof(Task) >= 16, "Task must be at least 16-byte aligned for SIMD captures");

} // namespace jaf
