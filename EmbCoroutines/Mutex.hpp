/**
 * @file    Mutex.hpp
 * @brief   Cooperative mutex with RAII guard for embedded coroutines.
 *
 * @details Mutex<MaxWaiters> provides mutual-exclusion between coroutines
 *          running cooperatively on a single thread.  Because embedded systems
 *          have no pre-emption between coroutines, a "lock" here simply means
 *          that at most one coroutine may hold the mutex at a time; any other
 *          coroutine that calls acquire() while the mutex is held will be
 *          suspended and placed in an internal FIFO waiter ring-buffer.  When
 *          the holder calls release() (or its MutexGuard goes out of scope),
 *          the oldest waiting coroutine is directly woken by rescheduling it
 *          into g_scheduler — no spinning, no busy-wait.
 *
 * ┌──────────────────────────────────────────────────────────────────────────┐
 * │  Ownership model                                                         │
 * │                                                                          │
 * │  • Only one coroutine holds the mutex at a time.                         │
 * │  • Recursive acquisition by the same coroutine is NOT supported and will │
 * │    deadlock.  This is by design — recursive mutexes hide design flaws.   │
 * │  • release() must always be called by the coroutine that called          │
 * │    acquire().  MutexGuard enforces this via RAII.                        │
 * └──────────────────────────────────────────────────────────────────────────┘
 *
 * Typical usage
 * ─────────────
 * @code
 *   emb::Mutex<4>  uart_mutex;   // at most 4 waiters queued simultaneously
 *
 *   emb::DefaultTask writer_a() {
 *       while (true) {
 *           {
 *               auto guard = co_await uart_mutex.lock();  // acquire
 *               // --- critical section ---
 *               co_await emb::sleep_ticks(5);
 *           }  // guard destructor releases the mutex
 *           co_await emb::yield();
 *       }
 *   }
 * @endcode
 *
 * @tparam MaxWaiters  Maximum coroutines that can queue behind the lock
 *                     simultaneously.  Exceeding this count traps.
 *                     Defaults to EMB_DEFAULT_MUTEX_WAITERS.
 */
#pragma once

#include <coroutine>
#include <cstddef>
#include <cstdint>
#include <cinttypes>
#include <cstdio>

#include "Assert.hpp"
#include "Config.hpp"
#include "Scheduler.hpp"

namespace emb {

// ─────────────────────────────────────────────────────────────────────────────
//  Forward declarations
// ─────────────────────────────────────────────────────────────────────────────
template<std::size_t MaxWaiters>
class Mutex;

// ─────────────────────────────────────────────────────────────────────────────
//  MutexGuard — RAII wrapper returned by co_await mutex.lock()
// ─────────────────────────────────────────────────────────────────────────────

/**
 * @brief  Scoped ownership token for a Mutex<>.
 *
 * @details A MutexGuard is constructed by Mutex<>::LockAwaitable::await_resume()
 *          upon successful acquisition and carries a back-pointer to its parent
 *          mutex.  Destroying (or explicitly releasing) the guard calls
 *          mutex.release(), which wakes the next waiter or marks the mutex free.
 *
 *          MutexGuard is move-only.  It must not outlive the Mutex that created
 *          it, which in practice means the Mutex should have static storage
 *          duration (as all embedded primitives normally do).
 */
template<std::size_t MaxWaiters>
class MutexGuard {
public:
    explicit MutexGuard(Mutex<MaxWaiters>& m) noexcept : m_mutex(&m) {}

    // Not copyable.
    MutexGuard(const MutexGuard&)            = delete;
    MutexGuard& operator=(const MutexGuard&) = delete;

    // Movable — transfer ownership (e.g. return from await_resume).
    MutexGuard(MutexGuard&& other) noexcept
        : m_mutex(other.m_mutex) { other.m_mutex = nullptr; }

    MutexGuard& operator=(MutexGuard&& other) noexcept {
        if (this != &other) {
            release();
            m_mutex       = other.m_mutex;
            other.m_mutex = nullptr;
        }
        return *this;
    }

    /**
     * @brief  Destructor — releases the mutex if this guard still holds it.
     *         Safe to call even if the guard has been moved-from.
     */
    ~MutexGuard() noexcept { release(); }

    /**
     * @brief  Explicitly release the lock before the guard goes out of scope.
     *         Idempotent — calling release() twice is safe.
     */
    void release() noexcept;   // defined after Mutex<> below

private:
    Mutex<MaxWaiters>* m_mutex;
};

// ─────────────────────────────────────────────────────────────────────────────
//  Mutex<MaxWaiters>
// ─────────────────────────────────────────────────────────────────────────────

template<std::size_t MaxWaiters = EMB_DEFAULT_MUTEX_WAITERS>
class Mutex {
public:
    // ── Statistics ───────────────────────────────────────────────────────────
    struct Stats {
        uint32_t total_acquisitions  = 0; ///< Times the lock was successfully taken.
        uint32_t total_releases      = 0; ///< Times release() was called.
        uint32_t contention_count    = 0; ///< Times a caller had to wait (lock was busy).
        uint32_t peak_waiter_count   = 0; ///< Highest simultaneous waiter count observed.
    };

    // ── Construction ─────────────────────────────────────────────────────────

    constexpr Mutex() noexcept = default;

    // Mutexes are not copyable or movable — they own waiter-queue state.
    Mutex(const Mutex&)            = delete;
    Mutex& operator=(const Mutex&) = delete;
    Mutex(Mutex&&)                 = delete;
    Mutex& operator=(Mutex&&)      = delete;

    // ─────────────────────────────────────────────────────────────────────────
    //  lock() — the primary coroutine interface
    // ─────────────────────────────────────────────────────────────────────────

    /**
     * @brief  Acquire the mutex, suspending the caller if it is already held.
     *
     * @details Returns a LockAwaitable.  When co_awaited:
     *
     *   • If the mutex is free:  await_ready() returns true, the coroutine is
     *     NOT suspended, and await_resume() returns a MutexGuard immediately.
     *
     *   • If the mutex is held:  await_ready() returns false, the coroutine is
     *     suspended in await_suspend(), its handle is stored in the waiter
     *     ring-buffer, and it will be re-scheduled (in FIFO order) by the next
     *     release() call.  await_resume() then returns the MutexGuard.
     *
     * @code
     *   auto guard = co_await my_mutex.lock();
     *   // --- protected region ---
     * @endcode
     */
    [[nodiscard]] auto lock() noexcept {

        // ── Inner awaitable ───────────────────────────────────────────────────
        struct LockAwaitable {
            Mutex& m;

            /**
             * @brief  Fast-path: acquire immediately if the mutex is free.
             */
            bool await_ready() noexcept {
                if (!m.m_locked) {
                    // Nobody holds the lock — take it right now.
                    m.m_locked = true;
#if EMB_STATS_ENABLED
                    ++m.m_stats.total_acquisitions;
#endif
                    EMB_TRACE("Mutex %p: acquired (fast path)\n",
                              static_cast<void*>(&m));
                    return true;  // coroutine is NOT suspended
                }
                return false;     // coroutine will be suspended
            }

            /**
             * @brief  Slow-path: the mutex is held; park this coroutine.
             *
             * @details Stores the caller's handle in the next free waiter slot.
             *          If the waiter ring-buffer is full, the firmware has a
             *          design problem — trap hard so it surfaces immediately.
             */
            void await_suspend(std::coroutine_handle<> caller) noexcept {
                EMB_ASSERT(m.m_waiter_count < MaxWaiters,
                           "Mutex: waiter queue overflow.  More coroutines are "
                           "contending than MaxWaiters allows.  Increase MaxWaiters "
                           "or reduce concurrency on this resource.");

                // Store handle in the ring-buffer.
                std::size_t idx = (m.m_waiter_head + m.m_waiter_count) % MaxWaiters;
                m.m_waiters[idx] = caller;
                ++m.m_waiter_count;

#if EMB_STATS_ENABLED
                ++m.m_stats.contention_count;
                if (m.m_waiter_count > m.m_stats.peak_waiter_count)
                    m.m_stats.peak_waiter_count =
                        static_cast<uint32_t>(m.m_waiter_count);
#endif
                EMB_TRACE("Mutex %p: contention — handle %p queued (waiters=%zu)\n",
                          static_cast<void*>(&m),
                          static_cast<void*>(caller.address()),
                          m.m_waiter_count);
            }

            /**
             * @brief  Called when this coroutine is resumed by release().
             *         The lock is already marked as held for us by release(),
             *         so we simply hand back the ownership guard.
             */
            MutexGuard<MaxWaiters> await_resume() noexcept {
                // When resumed via the slow path, release() has already set
                // m_locked = true on our behalf.
                EMB_TRACE("Mutex %p: acquired (slow path, resumed)\n",
                          static_cast<void*>(&m));
                return MutexGuard<MaxWaiters>{m};
            }
        };

        return LockAwaitable{*this};
    }

    // ─────────────────────────────────────────────────────────────────────────
    //  release() — called by MutexGuard's destructor
    // ─────────────────────────────────────────────────────────────────────────

    /**
     * @brief  Release the mutex.
     *
     * @details If the waiter queue is non-empty, the oldest waiting coroutine
     *          is directly re-scheduled (the lock is handed off, not released
     *          to "free" first).  If there are no waiters, the mutex is simply
     *          marked free.
     *
     * @note    Must only be called by the current holder.  MutexGuard enforces
     *          this automatically.  Do not call release() directly unless you
     *          manage locking manually.
     */
    void release() noexcept {
        EMB_ASSERT(m_locked,
                   "Mutex: release() called on an unheld mutex — ownership "
                   "violation.  Check for double-release or missing acquire.");

#if EMB_STATS_ENABLED
        ++m_stats.total_releases;
#endif

        if (m_waiter_count == 0) {
            // No waiters — simply free the mutex.
            m_locked = false;
            EMB_TRACE("Mutex %p: released (no waiters)\n",
                      static_cast<void*>(this));
            return;
        }

        // Wake the oldest waiter.  The lock remains "held" — ownership
        // transfers directly without briefly becoming free (avoids a
        // second caller sneaking in via await_ready() between the two steps).
        std::coroutine_handle<> next = m_waiters[m_waiter_head];
        m_waiters[m_waiter_head]     = nullptr;
        m_waiter_head                = (m_waiter_head + 1) % MaxWaiters;
        --m_waiter_count;

        // m_locked stays true — the woken coroutine is the new owner.
#if EMB_STATS_ENABLED
        ++m_stats.total_acquisitions;
#endif
        EMB_TRACE("Mutex %p: ownership transferred to handle %p (waiters=%zu)\n",
                  static_cast<void*>(this),
                  static_cast<void*>(next.address()),
                  m_waiter_count);

        bool ok = g_scheduler.schedule(next);
        EMB_ASSERT(ok, "Mutex: failed to schedule woken waiter — "
                       "Scheduler queue full.");
    }

    // ── Observation ──────────────────────────────────────────────────────────

    /// True if the mutex is currently held by any coroutine.
    bool is_locked()     const noexcept { return m_locked;        }

    /// Number of coroutines currently queued behind this mutex.
    std::size_t waiter_count() const noexcept { return m_waiter_count; }

    // ── Diagnostics ──────────────────────────────────────────────────────────

#if EMB_STATS_ENABLED
    const Stats& stats() const noexcept { return m_stats; }

    void print_stats(const char* name = "Mutex") const noexcept {
        printf("[%-24s] locked=%-5s  acquisitions=%-6" PRIu32
               "  releases=%-6" PRIu32
               "  contentions=%-6" PRIu32
               "  peak_waiters=%" PRIu32 "\n",
               name,
               m_locked ? "true" : "false",
               m_stats.total_acquisitions,
               m_stats.total_releases,
               m_stats.contention_count,
               m_stats.peak_waiter_count);
    }
#endif

private:
    // ── State ─────────────────────────────────────────────────────────────────
    bool        m_locked       = false;
    std::size_t m_waiter_head  = 0;  ///< Index of the oldest waiter.
    std::size_t m_waiter_count = 0;  ///< Number of active entries in the ring.

    /// FIFO ring-buffer of suspended coroutine handles.
    std::coroutine_handle<> m_waiters[MaxWaiters] = {};

#if EMB_STATS_ENABLED
    Stats m_stats{};
#endif

    // MutexGuard calls release() directly.
    friend class MutexGuard<MaxWaiters>;
};

// ─────────────────────────────────────────────────────────────────────────────
//  MutexGuard::release() — defined here after Mutex<> is complete
// ─────────────────────────────────────────────────────────────────────────────

template<std::size_t MaxWaiters>
void MutexGuard<MaxWaiters>::release() noexcept {
    if (m_mutex) {
        m_mutex->release();
        m_mutex = nullptr;
    }
}

// ─────────────────────────────────────────────────────────────────────────────
//  Convenience alias
// ─────────────────────────────────────────────────────────────────────────────

/// A mutex that supports up to EMB_DEFAULT_MUTEX_WAITERS waiting coroutines.
using DefaultMutex = Mutex<EMB_DEFAULT_MUTEX_WAITERS>;

} // namespace emb
