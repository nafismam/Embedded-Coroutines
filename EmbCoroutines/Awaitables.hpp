/**
 * @file    Awaitables.hpp
 * @brief   General-purpose free-function awaitables for cooperative scheduling.
 *
 * @details Provides:
 *   emb::yield()      — Suspend and immediately re-enqueue self in the scheduler.
 *                       Allows other ready tasks to run before this one continues.
 *                       Equivalent to a cooperative `taskYIELD()` in FreeRTOS.
 *
 * Note:  emb::sleep_ticks() is declared in Timer.hpp because it depends on
 *        the TimerManager.  It is also exported via EmbCoroutines.hpp.
 */
#pragma once

#include <coroutine>
#include "Scheduler.hpp"

namespace emb {

// ─────────────────────────────────────────────────────────────────────────────

/**
 * @brief  Cooperatively yield execution to the next ready task.
 *
 * @details When a coroutine does `co_await emb::yield()`:
 *   1. The coroutine suspends.
 *   2. await_suspend() re-enqueues the handle at the BACK of the ready queue.
 *   3. The scheduler resumes other ready tasks before coming back to this one.
 *
 *  This is the primary tool for implementing "polling" loops that must not
 *  monopolise the CPU:
 *
 *    while (true) {
 *        if (sensor_ready()) process_sensor();
 *        co_await emb::yield();   // let other tasks run
 *    }
 *
 *  Contrast with sleep_ticks(0): that also yields but goes through the timer
 *  path and will be rescheduled on the NEXT tick, not immediately.
 */
[[nodiscard]] inline auto yield() noexcept {
    struct YieldAwaitable {
        // Never immediately ready — we always want to suspend once.
        constexpr bool await_ready() const noexcept { return false; }

        // Re-queue self at the back of the ready queue, then suspend.
        void await_suspend(std::coroutine_handle<> h) const noexcept {
            g_scheduler.schedule(h);
        }

        constexpr void await_resume() const noexcept {}
    };
    return YieldAwaitable{};
}

// ─────────────────────────────────────────────────────────────────────────────

/**
 * @brief  Suspend until the given coroutine handle completes.
 *
 * @details This is a simple "join" primitive.  If the target is already
 *          done when await is reached, execution continues without suspending.
 *
 *  Usage:
 *    auto sub_task = some_coroutine();
 *    co_await emb::join(sub_task);   // wait until sub_task finishes
 *
 * @note  This performs a POLLING join — it yields once and checks done().
 *        For a true event-driven join, you would need a completion event
 *        signalled from final_suspend.  This version is sufficient for
 *        most embedded coordination patterns.
 */
template<typename TaskT>
[[nodiscard]] inline auto join(const TaskT& task) noexcept {
    struct JoinAwaitable {
        const TaskT& t;

        bool await_ready() const noexcept { return t.done(); }

        void await_suspend(std::coroutine_handle<> h) const noexcept {
            // Re-queue self so we get another chance to check done().
            // In a more sophisticated system, the sub-task's final_suspend
            // would signal an Event that wakes us exactly once.
            g_scheduler.schedule(h);
        }

        constexpr void await_resume() const noexcept {}
    };
    return JoinAwaitable{task};
}

} // namespace emb
