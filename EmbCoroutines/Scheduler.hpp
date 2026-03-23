/**
 * @file    Scheduler.hpp
 * @brief   Single-threaded cooperative task scheduler.
 *
 * @details The Scheduler maintains a ring-buffer of "ready" coroutine handles.
 *          It does NOT own or manage task lifetimes — that is the job of
 *          Task<> — it only decides the order in which handles are resumed.
 *
 * Scheduling Policy
 * ─────────────────
 *  Round-robin FIFO: tasks are resumed in the order they become ready.
 *  run_once() dequeues the oldest ready handle and resumes it exactly once.
 *  A resumed coroutine runs until it either:
 *    (a) completes — handle.done() becomes true, it is not re-queued, and
 *        the owning Task<> RAII destructor will eventually destroy the frame.
 *    (b) suspends — the awaitable's await_suspend() is responsible for
 *        re-enqueuing the handle (e.g. yield(), sleep_ticks(), Event::wait()).
 *
 * Global Instance
 * ───────────────
 *  A single `emb::g_scheduler` global is defined at the bottom of this header.
 *  All library primitives (Event, Channel, …) operate on this instance.
 *  If you need multiple schedulers, instantiate Scheduler<> directly.
 *
 * ISR Safety
 * ──────────
 *  schedule() modifies the ring buffer.  If called from an ISR concurrently
 *  with run_once() in the main loop, protect both with a critical section
 *  (disable/enable interrupts on ARM: __disable_irq() / __enable_irq()).
 *
 * @tparam QueueDepth  Maximum simultaneously-queued handles.  Keep power-of-2.
 */
#pragma once

#include <coroutine>
#include <cstddef>
#include <cstdint>

#include "Assert.hpp"
#include "Config.hpp"

namespace emb {

// ─────────────────────────────────────────────────────────────────────────────

template<std::size_t QueueDepth = EMB_SCHEDULER_QUEUE_DEPTH>
class Scheduler {
    static_assert(QueueDepth > 0,
        "Scheduler QueueDepth must be at least 1.");

    // Power-of-2 lets the compiler optimise modulo to bitwise AND.
    static_assert((QueueDepth & (QueueDepth - 1)) == 0,
        "Scheduler QueueDepth should be a power of 2 for efficiency.");

public:
    // ── Statistics ───────────────────────────────────────────────────────────

    struct Stats {
        std::uint32_t total_scheduled  = 0;  ///< Lifetime schedule() calls
        std::uint32_t total_resumed    = 0;  ///< Lifetime handle resumptions
        std::uint32_t total_completed  = 0;  ///< Handles that reached done()
        std::uint32_t queue_full_drops = 0;  ///< Dropped tasks (queue full)
        std::size_t   peak_queue_depth = 0;  ///< Highest simultaneous depth
    };

    // ── Core API ─────────────────────────────────────────────────────────────

    /**
     * @brief  Enqueue a coroutine handle into the ready queue.
     *
     * @param  handle  A valid, non-done coroutine handle.
     * @return true  if successfully enqueued.
     *         false if the queue is full (logs a warning; never traps).
     *
     * @note   May be called from an ISR — but see ISR Safety note in the
     *         file header regarding critical sections.
     */
    bool schedule(std::coroutine_handle<> handle) noexcept {
        if (!handle || handle.done()) {
            EMB_WARN("Scheduler::schedule() called with null or done handle — ignored.\n");
            return false;
        }

        if (is_full()) {
#if EMB_STATS_ENABLED
            ++m_stats.queue_full_drops;
#endif
            EMB_WARN("Scheduler queue full (depth=%zu)! "
                     "Handle dropped. Increase EMB_SCHEDULER_QUEUE_DEPTH.\n",
                     QueueDepth);
            return false;
        }

        m_queue[m_tail] = handle;
        m_tail = advance(m_tail);
        ++m_count;

#if EMB_STATS_ENABLED
        ++m_stats.total_scheduled;
        if (m_count > m_stats.peak_queue_depth) {
            m_stats.peak_queue_depth = m_count;
        }
#endif
        EMB_TRACE("Scheduler: enqueued handle, queue depth now %zu\n", m_count);
        return true;
    }

    /**
     * @brief  Dequeue and resume exactly ONE ready task.
     *
     * @return true  if a task was dequeued and resumed.
     *         false if the queue was empty (nothing to do).
     */
    bool run_once() noexcept {
        if (is_empty()) return false;

        std::coroutine_handle<> h = m_queue[m_head];
        m_queue[m_head] = {};  // Clear slot
        m_head = advance(m_head);
        --m_count;

#if EMB_STATS_ENABLED
        ++m_stats.total_resumed;
#endif

        if (h && !h.done()) {
            EMB_TRACE("Scheduler: resuming handle (queue depth after=%zu)\n", m_count);
            h.resume();  // ← Coroutine runs until next suspension point
        }

        if (h && h.done()) {
#if EMB_STATS_ENABLED
            ++m_stats.total_completed;
#endif
            EMB_TRACE("Scheduler: handle completed.\n");
        }

        return true;
    }

    /**
     * @brief  Drain the ready queue, resuming ALL currently queued tasks.
     *
     * @note   Tasks that re-enqueue themselves (via yield()) will be run
     *         in the next call to run_until_idle(), not the current one.
     *         This prevents infinite loops when every task yields.
     */
    void run_until_idle() noexcept {
        // Snapshot the depth so newly-added handles from within this
        // batch of resumptions are deferred to the next call.
        const std::size_t batch = m_count;
        for (std::size_t i = 0; i < batch; ++i) {
            run_once();
        }
    }

    /**
     * @brief  Run tasks until the queue is truly empty (including re-queued).
     * @warning  Use carefully — if every task yields, this loops forever.
     */
    void run_until_all_idle() noexcept {
        while (!is_empty()) {
            run_once();
        }
    }

    // ── Inspection ───────────────────────────────────────────────────────────

    std::size_t pending_count() const noexcept { return m_count;              }
    bool        is_idle()       const noexcept { return m_count == 0;         }
    bool        is_full()       const noexcept { return m_count == QueueDepth;}
    bool        is_empty()      const noexcept { return m_count == 0;         }

    const Stats& stats() const noexcept { return m_stats; }

    void print_stats() const noexcept {
        ::printf("[Scheduler] queued=%zu  scheduled=%u  resumed=%u  "
                 "completed=%u  drops=%u  peak=%zu\n",
                 m_count,
                 m_stats.total_scheduled,
                 m_stats.total_resumed,
                 m_stats.total_completed,
                 m_stats.queue_full_drops,
                 m_stats.peak_queue_depth);
    }

private:
    // Ring-buffer helpers
    static constexpr std::size_t advance(std::size_t idx) noexcept {
        // Safe for power-of-2 QueueDepth; falls back to modulo otherwise.
        return (idx + 1) & (QueueDepth - 1);
    }

    std::coroutine_handle<> m_queue[QueueDepth]{};
    std::size_t             m_head  = 0;
    std::size_t             m_tail  = 0;
    std::size_t             m_count = 0;
    Stats                   m_stats{};
};

// ─────────────────────────────────────────────────────────────────────────────
//  Global singleton instance
//  All library primitives reference this by default.
// ─────────────────────────────────────────────────────────────────────────────
inline Scheduler<EMB_SCHEDULER_QUEUE_DEPTH> g_scheduler;

} // namespace emb
