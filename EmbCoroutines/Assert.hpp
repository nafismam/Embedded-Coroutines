/**
 * @file    Assert.hpp
 * @brief   Assertion, fatal-trap, and diagnostic-trace facilities.
 *
 * @details Three macro families are provided:
 *
 *  EMB_TRAP(msg)              — Unconditional fatal halt.  Use for truly
 *                               unreachable / impossible states.
 *
 *  EMB_ASSERT(cond, msg)      — Halts if `cond` is false.
 *                               Compiled out in NDEBUG builds IF you define
 *                               EMB_ASSERT_DISABLE_IN_RELEASE=1.
 *
 *  EMB_TRACE(fmt, ...)        — printf-style trace; compiled to nothing unless
 *                               EMB_TRACE_ENABLED=1.
 *
 *  EMB_WARN(fmt, ...)         — Always-on warning (present in all builds).
 *
 * @section Platform Override
 *   Define EMB_PLATFORM_TRAP before including this header to redirect the
 *   fatal-halt path to your MCU's debugger break / reset sequence:
 *
 *     // For ARM Cortex-M with a connected debugger:
 *     #define EMB_PLATFORM_TRAP()  __BKPT(0)
 *
 *     // For production firmware (trigger watchdog reset):
 *     #define EMB_PLATFORM_TRAP()  NVIC_SystemReset()
 */
#pragma once

#include <cstdio>
#include "Config.hpp"

namespace emb::detail {

/// Core fatal-halt implementation.  Prints a diagnostic message and then
/// either invokes the user-supplied platform trap or spins forever (which
/// a hardware watchdog will convert to a reset).
[[noreturn]] inline void trap_impl(
    const char* msg,
    const char* file,
    int         line) noexcept
{
    // Print to wherever stdout is wired (ITM/SWO, UART, semihosting…).
    ::printf("\n"
             "╔══════════════════════════════════════════╗\n"
             "║          [EMB] FATAL TRAP                ║\n"
             "╚══════════════════════════════════════════╝\n"
             "  Reason : %s\n"
             "  File   : %s\n"
             "  Line   : %d\n\n",
             msg, file, line);

#ifdef EMB_PLATFORM_TRAP
    // User-supplied hook (debugger breakpoint, NVIC reset, etc.)
    EMB_PLATFORM_TRAP();
#endif

    // Fallback: spin forever.
    // A hardware watchdog will detect the stall and issue a system reset.
    // Declare the loop volatile so an optimising compiler never removes it.
    while (true) { /* watchdog reset expected */ }
}

} // namespace emb::detail


// ─────────────────────────────────────────────────────────────────────────────
//  Public macros
// ─────────────────────────────────────────────────────────────────────────────

/// Unconditional fatal halt with a descriptive message.
#define EMB_TRAP(msg) \
    ::emb::detail::trap_impl((msg), __FILE__, __LINE__)

/// Conditional assert.  Halts if `cond` evaluates to false.
/// Both sides of the condition are always evaluated (no UB surprises).
#define EMB_ASSERT(cond, msg) \
    do { if (!(cond)) { EMB_TRAP(msg); } } while (false)

/// Verbose trace — completely elided unless EMB_TRACE_ENABLED=1.
#if EMB_TRACE_ENABLED
#  define EMB_TRACE(fmt, ...) \
     ::printf("[EMB TRACE] " fmt, ##__VA_ARGS__)
#else
#  define EMB_TRACE(fmt, ...) \
     do { (void)0; } while (false)
#endif

/// Warning — always present; use sparingly for recoverable anomalies.
#define EMB_WARN(fmt, ...) \
    ::printf("[EMB WARN ] " fmt, ##__VA_ARGS__)
