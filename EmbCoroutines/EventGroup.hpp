/**
 * @file    EventGroup.hpp
 * @brief   Multi-bit event group — allows multiple coroutines to wait on
 *          combinations of boolean flags simultaneously.
 *
 * @details An EventGroup holds a 32-bit flag word.  Multiple coroutines can
 *          register interest in different subsets of those bits using two
 *          waiting modes:
 *
 *   wait_any(mask)  — wake when ANY bit in mask is set.
 *   wait_all(mask)  — wake when ALL bits in mask are simultaneously set.
 *
 *  When a waiter is released its matched bits are optionally cleared
 *  (auto_clear = true, the default).
 *
 * Fan-out
 * ───────
 *  Multiple coroutines can wait on the same EventGroup simultaneously, each
 *  with a different mask.  When set() is called, ALL waiters whose masks are
 *  satisfied are woken in registration order.
 *
 * Typical Usage (hardware-interrupt flags)
 * ─────────────────────────────────────────
 *   inline emb::EventGroup<> system_events;
 *
 *   constexpr uint32_t EVT_UART_RX  = (1u << 0);
 *   constexpr uint32_t EVT_SPI_DONE = (1u << 1);
 *   constexpr uint32_t EVT_BUTTON   = (1u << 2);
 *
 *   // In ISR:
 *   system_events.set(EVT_UART_RX);
 *
 *   // In coroutine A:
 *   uint32_t fired = co_await system_events.wait_any(EVT_UART_RX | EVT_SPI_DONE);
 *   if (fired & EVT_UART_RX) handle_uart();
 *
 *   // In coroutine B (waits until BOTH flags set simultaneously):
 *   co_await system_events.wait_all(EVT_SPI_DONE | EVT_BUTTON);
 *
 * @tparam MaxWaiters  Maximum simultaneous waiting coroutines.
 */
#pragma once

#include <coroutine>
#include <cstddef>
#include <cstdint>

#include "Assert.hpp"
#include "Scheduler.hpp"
#include "Config.hpp"

namespace emb {

// ─────────────────────────────────────────────────────────────────────────────

template<std::size_t MaxWaiters = EMB_DEFAULT_EVENT_GROUP_WAITERS>
class EventGroup {
    static_assert(MaxWaiters > 0, "EventGroup MaxWaiters must be >= 1.");

public:
    // ── Signalling ────────────────────────────────────────────────────────────

    /**
     * @brief  Set one or more flag bits and wake any satisfied waiters.
     * @param  bits  Bitmask of flags to set (OR'd into the current state).
     */
    void set(uint32_t bits) noexcept {
        m_bits |= bits;
        EMB_TRACE("EventGroup::set(0x%08X) → state=0x%08X\n", bits, m_bits);

        // Check each registered waiter — wake those whose condition is met.
        for (auto& w : m_waiters) {
            if (!w.active) continue;

            const bool satisfied = w.wait_all
                ? ((m_bits & w.mask) == w.mask)   // ALL bits must be set
                : ((m_bits & w.mask) != 0u);       // ANY bit suffices

            if (satisfied) {
                w.active = false;
                if (w.auto_clear) {
                    m_bits &= ~w.mask;  // Clear matched bits before waking
                }
                EMB_TRACE("EventGroup: waking waiter (mask=0x%08X, mode=%s)\n",
                          w.mask, w.wait_all ? "ALL" : "ANY");
                g_scheduler.schedule(w.handle);
            }
        }
    }

    /**
     * @brief  Clear one or more flag bits (without waking waiters).
     * @param  bits  Bitmask of flags to clear.
     */
    void clear(uint32_t bits) noexcept {
        m_bits &= ~bits;
        EMB_TRACE("EventGroup::clear(0x%08X) → state=0x%08X\n", bits, m_bits);
    }

    /// Read the current flag state (non-blocking, no side effects).
    uint32_t get() const noexcept { return m_bits; }

    // ── Awaitable factories ───────────────────────────────────────────────────

    /**
     * @brief  Suspend until ANY bit in `mask` is set.
     *
     * @param  mask        Which bits to watch.  Must be != 0.
     * @param  auto_clear  If true (default), the matched bits are cleared
     *                     when this waiter is released.
     * @return Awaitable — co_await it from a coroutine.
     *         await_resume() returns the subset of mask bits that triggered
     *         the wakeup.
     *
     * @code
     *   uint32_t fired = co_await events.wait_any(FLAG_A | FLAG_B);
     *   if (fired & FLAG_A) { … }
     * @endcode
     */
    [[nodiscard]] auto wait_any(uint32_t mask, bool auto_clear = true) noexcept {
        return make_awaitable(mask, /*wait_all=*/false, auto_clear);
    }

    /**
     * @brief  Suspend until ALL bits in `mask` are simultaneously set.
     *
     * @param  mask        All of these bits must be set at the same time.
     * @param  auto_clear  If true (default), ALL matched bits are cleared
     *                     on wakeup.
     *
     * @code
     *   co_await events.wait_all(FLAG_INIT_DONE | FLAG_CONFIG_LOADED);
     * @endcode
     */
    [[nodiscard]] auto wait_all(uint32_t mask, bool auto_clear = true) noexcept {
        return make_awaitable(mask, /*wait_all=*/true, auto_clear);
    }

    // ── set_from_isr ──────────────────────────────────────────────────────────
    void set_from_isr(uint32_t bits) noexcept { set(bits); }

private:
    // ── Internal waiter registry ──────────────────────────────────────────────

    struct Waiter {
        uint32_t                mask       = 0;
        std::coroutine_handle<> handle{};
        bool                    wait_all   = false;
        bool                    auto_clear = true;
        bool                    active     = false;
    };

    bool add_waiter(std::coroutine_handle<> h,
                    uint32_t mask, bool wait_all, bool auto_clear) noexcept {
        for (auto& w : m_waiters) {
            if (!w.active) {
                w = {mask, h, wait_all, auto_clear, true};
                return true;
            }
        }
        EMB_TRAP("EventGroup: waiter slots exhausted. "
                 "Increase the MaxWaiters template parameter.");
        return false;
    }

    // ── Awaitable implementation ─────────────────────────────────────────────

    struct Awaitable {
        EventGroup* eg;
        uint32_t    mask;
        bool        wait_all_mode;
        bool        auto_clear;
        uint32_t    triggered_bits = 0;  // Populated in await_resume

        bool await_ready() noexcept {
            const bool satisfied = wait_all_mode
                ? ((eg->m_bits & mask) == mask)
                : ((eg->m_bits & mask) != 0u);

            if (satisfied) {
                triggered_bits = eg->m_bits & mask;
                if (auto_clear) eg->m_bits &= ~mask;
            }
            return satisfied;
        }

        void await_suspend(std::coroutine_handle<> h) noexcept {
            eg->add_waiter(h, mask, wait_all_mode, auto_clear);
        }

        uint32_t await_resume() noexcept {
            // If we suspended, compute triggered_bits now (bits were cleared by set())
            if (triggered_bits == 0) {
                triggered_bits = mask;  // All relevant bits fired
            }
            return triggered_bits;
        }
    };

    Awaitable make_awaitable(uint32_t mask,
                             bool wait_all,
                             bool auto_clear) noexcept {
        EMB_ASSERT(mask != 0, "EventGroup: wait mask must not be 0.");
        return Awaitable{this, mask, wait_all, auto_clear};
    }

    // ── Data members ─────────────────────────────────────────────────────────

    uint32_t m_bits = 0;
    Waiter   m_waiters[MaxWaiters]{};
};

} // namespace emb
