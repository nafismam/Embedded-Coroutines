/**
 * @file    Task.hpp
 * @brief   Coroutine return type that allocates frames from a StaticPool.
 *
 * @details Task<FrameSize, MaxConcurrent> is the library's primary coroutine
 *          return type.  It replaces heap-allocated coroutine frames with a
 *          per-type static pool.  Key behaviours:
 *
 *   • Allocation  — operator new routes to the type's StaticPool.  Traps on
 *                   pool exhaustion rather than returning nullptr.
 *   • Scheduling  — get_return_object() auto-enqueues the new handle into
 *                   g_scheduler.  Coroutines start suspended (initial_suspend
 *                   returns suspend_always) and run when the scheduler resumes
 *                   them.
 *   • Ownership   — Task<> is move-only.  The destructor destroys the handle.
 *   • Completion  — final_suspend returns suspend_always so the Task<> owner
 *                   can observe the done() state before the frame is freed.
 *
 * Sizing Guide
 * ────────────
 *   FrameSize must be >= the coroutine's actual frame size.  To find the
 *   exact size on GCC/Clang: compile with -fcoroutines and grep the .map
 *   file for the coroutine's frame struct.  Add ~64 bytes of headroom.
 *
 *   Alternatively: start large (512), watch the pool's peak_used_slots at
 *   runtime, and reduce if memory is tight.
 *
 * Multiple Task Types
 * ───────────────────
 *   Each unique <FrameSize, MaxConcurrent> pair is a distinct C++ type and
 *   gets its own independent StaticPool:
 *
 *     using SmallTask = emb::Task<128, 4>;   // 4 × 128 B = 512 B total
 *     using LargeTask = emb::Task<1024, 2>;  // 2 × 1024 B = 2 KB total
 *
 *     SmallTask  fast_handler();  // body declared as returning SmallTask
 *     LargeTask  slow_parser();   // body declared as returning LargeTask
 *
 * @tparam FrameSize      Bytes per coroutine-frame slot.
 * @tparam MaxConcurrent  Max simultaneously alive coroutines of this type.
 */
#pragma once

#include <coroutine>
#include <cstddef>

#include "Assert.hpp"
#include "Config.hpp"
#include "StaticPool.hpp"
#include "Scheduler.hpp"

namespace emb {

// ─────────────────────────────────────────────────────────────────────────────

template<
    std::size_t FrameSize      = EMB_DEFAULT_FRAME_SIZE,
    std::size_t MaxConcurrent  = EMB_DEFAULT_MAX_TASKS>
class Task {
public:
    // ── One static pool shared by all instances of this Task<> type ──────────
    inline static StaticPool<FrameSize, MaxConcurrent> s_pool;

    // ─────────────────────────────────────────────────────────────────────────
    //  promise_type — the compiler-mandated coroutine interface
    // ─────────────────────────────────────────────────────────────────────────
    struct promise_type {

        // ── Coroutine lifecycle ───────────────────────────────────────────────

        /**
         * @brief  Build and return the Task<> object, then auto-schedule
         *         the new coroutine.  Called by the compiler before the
         *         coroutine body starts.
         */
        Task get_return_object() noexcept {
            auto h = std::coroutine_handle<promise_type>::from_promise(*this);

            // Enqueue into the global scheduler. The coroutine is suspended
            // at initial_suspend() and will run on the next scheduler tick.
            bool ok = g_scheduler.schedule(h);
            EMB_ASSERT(ok, "Task: failed to schedule new coroutine — "
                           "Scheduler queue full. Increase EMB_SCHEDULER_QUEUE_DEPTH.");

            EMB_TRACE("Task: created and scheduled handle %p\n",
                      static_cast<void*>(h.address()));
            return Task{h};
        }

        /**
         * @brief  Suspend immediately on creation.
         *         The scheduler decides when to first run the coroutine body.
         */
        std::suspend_always initial_suspend() noexcept { return {}; }

        /**
         * @brief  Suspend at completion so the owning Task<> can observe
         *         the done() state before the frame is torn down.
         *         The frame is freed by Task<>'s destructor via handle.destroy().
         */
        auto final_suspend() noexcept {

            struct FinalAwaiter {

                bool await_ready() noexcept { return false; }

                void await_suspend(std::coroutine_handle<promise_type> h) noexcept {
                    h.destroy();   // free pool slot here
                }

                void await_resume() noexcept {}

            };

            return FinalAwaiter{};
        }

        /// void-returning coroutines have no value to return.
        void return_void() noexcept {}

        /// Unhandled exceptions in embedded firmware are always fatal.
        void unhandled_exception() noexcept {
            EMB_TRAP("Task: unhandled exception thrown inside a coroutine. "
                     "Embed code must not throw.");
        }

        // ── Custom allocator — routes to the per-type StaticPool ─────────────

        /**
         * @brief  Called by the compiler to allocate the coroutine frame.
         *         Routes to this Task<> type's static pool.
         *
         * @note   The `noexcept` + `get_return_object_on_allocation_failure`
         *         combination tells the compiler to use the nothrow new path
         *         (C++20 §9.5.4).  The pool never actually returns nullptr —
         *         it traps instead — so the failure path is a safety belt.
         */
        [[nodiscard]]
        static void* operator new(std::size_t size) noexcept {
            // Pool traps internally on overflow, so nullptr is never returned.
            return Task<FrameSize, MaxConcurrent>::s_pool.allocate(size);
        }

        /**
         * @brief  Called when the coroutine frame is destroyed.
         *         Returns the slot to the pool.
         */
        static void operator delete(void* ptr, std::size_t /*size*/) noexcept {
            Task<FrameSize, MaxConcurrent>::s_pool.deallocate(ptr);
        }

        /**
         * @brief  Fallback invoked by the C++20 runtime when operator new
         *         returns nullptr.  Since our pool traps before returning
         *         nullptr, this is truly unreachable — it exists purely to
         *         satisfy the standard and ensure the nothrow new path is
         *         selected by the compiler.
         */
        [[nodiscard]]
        static Task get_return_object_on_allocation_failure() noexcept {
            EMB_TRAP("Task: coroutine frame allocation failed (unreachable "
                     "in normal operation — pool traps first).");
            return Task{nullptr};
        }
    };

    // ─────────────────────────────────────────────────────────────────────────
    //  Task<> public interface
    // ─────────────────────────────────────────────────────────────────────────

    using HandleType = std::coroutine_handle<promise_type>;

    // Not copyable — a handle has exactly one owner.
    Task(const Task&)            = delete;
    Task& operator=(const Task&) = delete;

    // Movable — transfer ownership.
    Task(Task&& other) noexcept : m_handle(other.m_handle) {
        other.m_handle = nullptr;
    }
    Task& operator=(Task&& other) noexcept {
        if (this != &other) {
            if (m_handle) m_handle.destroy();
            m_handle       = other.m_handle;
            other.m_handle = nullptr;
        }
        return *this;
    }

    /**
     * @brief  Destructor — destroys the coroutine frame and returns its
     *         memory to the pool (via promise_type::operator delete).
     *
     * @note   It is safe to destroy a Task<> that has already completed
     *         (done() == true) — that is the intended lifecycle.
     */
    ~Task() noexcept = default;

    // ── Observation ──────────────────────────────────────────────────────────

    /// True if the coroutine has run to completion (reached co_return or end).
    bool       done()   const noexcept { return !m_handle || m_handle.done(); }
    HandleType handle() const noexcept { return m_handle;                      }

    // ── Class-level pool diagnostics ─────────────────────────────────────────

    static const typename StaticPool<FrameSize,MaxConcurrent>::Stats&
    pool_stats() noexcept { return s_pool.stats(); }

    static void print_pool_stats(const char* name = "Task::s_pool") noexcept {
        s_pool.print_stats(name);
    }

private:
    explicit Task(HandleType h) noexcept : m_handle(h) {}

    HandleType m_handle;

    // Allow promise_type to call the private constructor.
    friend struct promise_type;
};

// ─────────────────────────────────────────────────────────────────────────────
//  Convenience alias — used for quick prototyping / simple coroutines.
//  Prefer explicit Task<FrameSize, MaxConcurrent> in production code.
// ─────────────────────────────────────────────────────────────────────────────
using DefaultTask = Task<EMB_DEFAULT_FRAME_SIZE, EMB_DEFAULT_MAX_TASKS>;

} // namespace emb
