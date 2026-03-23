/**
 * @file    Event.hpp
 * @brief   Binary event flag — the fundamental coroutine synchronisation primitive.
 *
 * @details An Event has two states: *signalled* and *cleared*.
 *          A coroutine waiting on a cleared event suspends until another
 *          coroutine (or an ISR) calls set().
 *
 * Behaviour
 * ─────────
 *  • Only ONE coroutine may wait on a single Event at a time.
 *    For fan-out notification, use EventGroup.
 *  • When set() is called and a waiter is registered, the waiter is
 *    immediately enqueued in g_scheduler.  set() itself does not resume
 *    the waiter — the scheduler does on the next run_once() cycle.
 *  • The event is auto-cleared when a waiter is released (i.e. await_resume
 *    clears the signalled flag), matching FreeRTOS xEventGroupWaitBits
 *    auto-clear semantics.  Use is_set() to peek without clearing.
 *
 * ISR Usage
 * ─────────
 *  set_from_isr() is provided for interrupt-context use.  It calls the same
 *  g_scheduler.schedule() as set() — you must ensure the scheduler's ring
 *  buffer is not concurrently modified from the main loop (disable IRQ around
 *  the scheduler's run loop, or use a platform critical section).
 *
 * Typical Pattern
 * ───────────────
 *  // In a UART ISR (or test harness):
 *  emb::Event uart_rx_event;
 *
 *  extern "C" void USART1_IRQHandler() {
 *      uart_data_register = USART1->DR;
 *      uart_rx_event.set_from_isr();  // wake the waiting coroutine
 *  }
 *
 *  // In a coroutine:
 *  emb::Task<256,2> uart_reader() {
 *      while (true) {
 *          co_await uart_rx_event.wait();
 *          process(uart_data_register);
 *      }
 *  }
 */
#pragma once

#include <coroutine>
#include "Assert.hpp"
#include "Scheduler.hpp"

namespace emb {

// ─────────────────────────────────────────────────────────────────────────────

class Event {
public:
    // ── Signalling (may be called from main loop or ISR) ─────────────────────

    /**
     * @brief  Signal the event from the main cooperative context.
     *         If a coroutine is waiting, it is enqueued into g_scheduler.
     */
    void set() noexcept {
        m_signaled = true;
        if (m_waiter) {
            auto h    = m_waiter;
            m_waiter  = {};  // Clear before scheduling to avoid re-use issues
            g_scheduler.schedule(h);
            EMB_TRACE("Event::set() — waiter scheduled.\n");
        } else {
            EMB_TRACE("Event::set() — no waiter, flag latched.\n");
        }
    }

    /**
     * @brief  Signal the event from an ISR.
     *         Functionally identical to set() in this implementation —
     *         the distinction exists to document intent and to allow you
     *         to add platform-specific critical-section guards here.
     *
     * @note   You are responsible for protecting g_scheduler's ring buffer
     *         from concurrent access if called from an ISR. Example:
     *
     *           extern "C" void TIM2_IRQHandler() {
     *               __disable_irq();          // ARM Cortex-M
     *               my_event.set_from_isr();
     *               __enable_irq();
     *           }
     */
    void set_from_isr() noexcept { set(); }

    /// Clear the event flag without waking any waiter.
    void clear() noexcept {
        m_signaled = false;
        EMB_TRACE("Event::clear()\n");
    }

    /// Non-blocking peek: is the event currently signalled?
    bool is_set() const noexcept { return m_signaled; }

    /// True if a coroutine is currently suspended waiting on this event.
    bool has_waiter() const noexcept { return static_cast<bool>(m_waiter); }

    // ── Awaitable interface ───────────────────────────────────────────────────

    /**
     * @brief  Await this event from a coroutine.
     *
     * @return An awaitable that:
     *   • Returns immediately (no suspend) if the event is already signalled.
     *   • Suspends the coroutine if the event is not yet signalled, registering
     *     it as the sole waiter.  Traps if a second waiter attempts to register.
     *
     * @code
     *   co_await my_event.wait();   // suspend until set() is called
     * @endcode
     */
    [[nodiscard]] auto wait() noexcept {
        struct WaitAwaitable {
            Event* event;

            /// If already signalled — consume the signal and don't suspend.
            bool await_ready() noexcept {
                if (event->m_signaled) {
                    event->m_signaled = false;  // Auto-clear on consume
                    return true;
                }
                return false;
            }

            /// Register this coroutine as the waiter and suspend.
            void await_suspend(std::coroutine_handle<> h) noexcept {
                EMB_ASSERT(!event->m_waiter,
                    "Event: a second coroutine attempted to wait() on an Event "
                    "that already has a registered waiter.  "
                    "Use EventGroup for fan-out, or serialise access.");
                event->m_waiter = h;
                EMB_TRACE("Event::wait() — coroutine suspended, waiter registered.\n");
            }

            constexpr void await_resume() noexcept {}
        };
        return WaitAwaitable{this};
    }

private:
    bool                    m_signaled = false;
    std::coroutine_handle<> m_waiter{};
};

} // namespace emb
