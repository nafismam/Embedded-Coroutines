/**
 * @file    Semaphore.hpp
 * @brief   Counting semaphore for coroutine-based resource management.
 *
 * @details A Semaphore has an integer counter representing available resource
 *          units.  acquire() decrements the counter (suspending if it would
 *          go negative).  release() increments the counter and wakes the
 *          oldest waiting coroutine if any are blocked.
 *
 * Common Patterns
 * ───────────────
 *  • Binary semaphore (initial count = 1):
 *      Behaves like a signalling flag — one release() lets one acquire() through.
 *      Use Mutex<> instead if you need RAII unlock.
 *
 *  • Resource pool guard (initial count = N):
 *      Models N identical resources (e.g. 3 DMA channels):
 *        emb::Semaphore<4> dma_sem{3};  // 3 DMA channels
 *        co_await dma_sem.acquire();    // blocks if all 3 are busy
 *        // … use DMA …
 *        dma_sem.release();
 *
 *  • Rate limiter / producer gate (initial count = 0):
 *      Consumer starts at 0 and blocks until producer calls release().
 *      Very similar to Event but allows counting up to MaxWaiters releases.
 *
 * @tparam MaxWaiters  Maximum simultaneously blocked coroutines.
 *                     Traps if exceeded.
 */
#pragma once

#include <coroutine>
#include <cstddef>
#include <cstdint>

#include "Assert.hpp"
#include "Config.hpp"
#include "Scheduler.hpp"

namespace emb {

// ─────────────────────────────────────────────────────────────────────────────

template<std::size_t MaxWaiters = EMB_DEFAULT_SEMAPHORE_WAITERS>
class Semaphore {
    static_assert(MaxWaiters > 0, "Semaphore MaxWaiters must be >= 1.");

public:
    /**
     * @param initial_count  Starting resource count.  Use 0 for a pure
     *                       signalling semaphore, 1 for a binary semaphore,
     *                       or N for a resource-pool semaphore.
     */
    explicit Semaphore(int initial_count = 1) noexcept
        : m_count(initial_count) {}

    // ── acquire() — awaitable ─────────────────────────────────────────────────

    /**
     * @brief  Decrement the counter, suspending if it would go below zero.
     *
     * @details If the counter is > 0, decrements immediately and continues
     *          without suspending.  If == 0, the coroutine is queued and
     *          suspended until a matching release() arrives.
     *
     * @code
     *   co_await my_semaphore.acquire();
     *   // ← count was > 0, or we slept until release() woke us
     * @endcode
     */
    [[nodiscard]] auto acquire() noexcept {
        struct AcquireAwaitable {
            Semaphore* sem;

            bool await_ready() noexcept {
                if (sem->m_count > 0) {
                    --sem->m_count;
                    EMB_TRACE("Semaphore::acquire() — immediate (count now %d)\n",
                              sem->m_count);
                    return true;
                }
                return false;
            }

            void await_suspend(std::coroutine_handle<> h) noexcept {
                bool ok = sem->enqueue_waiter(h);
                EMB_ASSERT(ok,
                    "Semaphore waiter queue full. "
                    "Increase the MaxWaiters template parameter.");
                EMB_TRACE("Semaphore::acquire() — count=0, suspending coroutine.\n");
            }

            constexpr void await_resume() noexcept {
                // If we suspended, release() already decremented for us.
            }
        };
        return AcquireAwaitable{this};
    }

    // ── release() ────────────────────────────────────────────────────────────

    /**
     * @brief  Release one unit.  Wakes the oldest blocked acquirer if any.
     *
     * @note   May be called from the main loop or from an ISR (see ISR note
     *         in Scheduler.hpp regarding critical sections).
     */
    void release() noexcept {
        if (m_waiter_count > 0) {
            // Don't increment m_count — the woken coroutine "consumes" this
            // release unit directly without the count ever going positive.
            auto h = dequeue_waiter();
            g_scheduler.schedule(h);
            EMB_TRACE("Semaphore::release() — woke queued waiter.\n");
        } else {
            ++m_count;
            EMB_TRACE("Semaphore::release() — count now %d\n", m_count);
        }
    }

    // ── Inspection ───────────────────────────────────────────────────────────

    int         count()        const noexcept { return m_count;         }
    std::size_t waiter_count() const noexcept { return m_waiter_count;  }
    bool        is_available() const noexcept { return m_count > 0;     }

private:
    // ── FIFO waiter queue (ring buffer) ───────────────────────────────────────

    bool enqueue_waiter(std::coroutine_handle<> h) noexcept {
        if (m_waiter_count == MaxWaiters) return false;
        m_waiters[m_tail] = h;
        m_tail = (m_tail + 1) % MaxWaiters;
        ++m_waiter_count;
        return true;
    }

    std::coroutine_handle<> dequeue_waiter() noexcept {
        EMB_ASSERT(m_waiter_count > 0, "Semaphore: dequeue from empty queue.");
        auto h = m_waiters[m_head];
        m_head = (m_head + 1) % MaxWaiters;
        --m_waiter_count;
        return h;
    }

    // ── Data members ─────────────────────────────────────────────────────────

    int  m_count;  ///< Current resource count

    // Ring buffer of waiting handles
    std::coroutine_handle<> m_waiters[MaxWaiters]{};
    std::size_t             m_head         = 0;
    std::size_t             m_tail         = 0;
    std::size_t             m_waiter_count = 0;
};

} // namespace emb
