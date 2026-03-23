/**
 * @file    Timer.hpp
 * @brief   Tick-driven sleep/wakeup facility for coroutines.
 *
 * @details The TimerManager maintains a list of (target_tick, handle) pairs.
 *          On each call to tick(), the internal tick counter advances by one
 *          and any handles whose target_tick has been reached are pushed into
 *          the global scheduler's ready queue.
 *
 * Integration
 * ───────────
 *  Call `emb::g_timer.tick()` once per system tick, typically from:
 *   • A hardware timer ISR (SysTick, TIM6, etc.)  — most accurate
 *   • The main superloop, if the loop period == 1 tick
 *
 *  Example (ARM Cortex-M SysTick ISR):
 *    extern "C" void SysTick_Handler() {
 *        emb::g_timer.tick();
 *    }
 *
 * Usage in a coroutine:
 *    co_await emb::sleep_ticks(100);  // suspend for 100 ticks
 *
 * @tparam MaxTimers  Maximum simultaneously sleeping coroutines.
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

template<std::size_t MaxTimers = EMB_MAX_TIMERS>
class TimerManager {
    static_assert(MaxTimers > 0, "MaxTimers must be at least 1.");

public:
    // ── Core API ─────────────────────────────────────────────────────────────

    /**
     * @brief  Advance the tick counter by one.
     *         Wakes all coroutines whose sleep deadline has elapsed.
     *
     * @return The new tick count (useful for timestamping).
     *
     * @note   Call this from your system-tick interrupt or main loop.
     *         In an ISR, schedule() on g_scheduler may race with the main
     *         loop — guard with a critical section if needed.
     */
    uint32_t tick() noexcept {
        ++m_tick;

        for (auto& entry : m_entries) {
            if (entry.active && m_tick >= entry.target_tick) {
                entry.active = false;
                EMB_TRACE("TimerManager: tick=%u, waking handle (target was %u)\n",
                          m_tick, entry.target_tick);
                g_scheduler.schedule(entry.handle);
            }
        }

        return m_tick;
    }

    /**
     * @brief  Register a coroutine handle to be woken after `delay_ticks`.
     *
     * @param  handle       The suspended coroutine to wake.
     * @param  delay_ticks  Number of ticks from now until wakeup.
     *                      delay_ticks == 0 schedules immediately.
     * @return true  on success.
     *         false if the timer list is full (asserts — increase MaxTimers).
     */
    bool add(std::coroutine_handle<> handle, uint32_t delay_ticks) noexcept {
        for (auto& entry : m_entries) {
            if (!entry.active) {
                entry.handle      = handle;
                entry.target_tick = m_tick + delay_ticks;
                entry.active      = true;
                EMB_TRACE("TimerManager: registered sleep, "
                          "now=%u target=%u delay=%u\n",
                          m_tick, entry.target_tick, delay_ticks);
                return true;
            }
        }

        // All slots occupied — this is a configuration error.
        EMB_TRAP("TimerManager full: increase EMB_MAX_TIMERS.");
        return false;
    }

    /// Cancel all pending timer entries for a specific handle.
    void cancel(std::coroutine_handle<> handle) noexcept {
        for (auto& entry : m_entries) {
            if (entry.active && entry.handle == handle) {
                entry.active = false;
                EMB_TRACE("TimerManager: cancelled timer for handle.\n");
            }
        }
    }

    // ── Inspection ───────────────────────────────────────────────────────────

    uint32_t    now()            const noexcept { return m_tick; }
    std::size_t pending_count()  const noexcept {
        std::size_t n = 0;
        for (const auto& e : m_entries) { if (e.active) ++n; }
        return n;
    }

    void print_stats() const noexcept {
        ::printf("[TimerManager] tick=%u  active_timers=%zu/%zu\n",
                 m_tick, pending_count(), MaxTimers);
    }

private:
    struct Entry {
        uint32_t                target_tick = 0;
        std::coroutine_handle<> handle{};
        bool                    active = false;
    };

    Entry    m_entries[MaxTimers]{};
    uint32_t m_tick = 0;
};

// ─────────────────────────────────────────────────────────────────────────────
//  Global singleton
// ─────────────────────────────────────────────────────────────────────────────
inline TimerManager<EMB_MAX_TIMERS> g_timer;


// ─────────────────────────────────────────────────────────────────────────────
//  sleep_ticks() — the coroutine-facing API
// ─────────────────────────────────────────────────────────────────────────────

/**
 * @brief  Suspend the calling coroutine for at least `ticks` system ticks.
 *
 * @details Usage:
 *    co_await emb::sleep_ticks(50);   // sleep ~50 ms if tick == 1 ms
 *
 *  If ticks == 0, the coroutine is immediately rescheduled (cooperative yield
 *  with a one-tick grace before re-running, effectively identical to yield()
 *  but routed through the timer rather than the scheduler directly).
 *
 * @param  ticks  Number of system ticks to sleep.
 * @return An awaitable that suspends the coroutine and registers it with
 *         g_timer.  Resumed by TimerManager::tick() when the deadline passes.
 */
[[nodiscard]] inline auto sleep_ticks(uint32_t ticks) noexcept {
    struct SleepAwaitable {
        uint32_t delay;

        /// If delay == 0 we could return true here for an instant re-run,
        /// but going through the timer ensures fair scheduling.
        constexpr bool await_ready() const noexcept { return false; }

        void await_suspend(std::coroutine_handle<> h) const noexcept {
            g_timer.add(h, delay);
        }

        constexpr void await_resume() const noexcept {}
    };
    return SleepAwaitable{ticks};
}

} // namespace emb
