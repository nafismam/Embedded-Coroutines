/**
 * @file    EmbCoroutines.hpp
 * @brief   Single-header master include for the EmbCoroutines library.
 *
 * @details Include this one file to pull the entire library into a translation
 *          unit.  All components are header-only, so no separate compilation
 *          step is required.
 *
 * ┌──────────────────────────────────────────────────────────────────────────┐
 * │  EmbCoroutines — Industrial-Grade Embedded C++20 Coroutine Library       │
 * │                                                                          │
 * │  Design pillars                                                          │
 * │  ─────────────────────────────────────────────────────────────────────  │
 * │  • ZERO heap allocation — all frames and state live in static storage.  │
 * │  • Multi-coroutine — dozens of concurrent tasks from a single thread.   │
 * │  • ISR-friendly — safe set_from_isr() on Event / EventGroup.            │
 * │  • Deterministic — no OS, no dynamic containers, no exceptions.         │
 * │  • Fully observable — compile-time Stats (EMB_STATS_ENABLED) and        │
 * │    Trace (EMB_TRACE_ENABLED) with zero overhead when disabled.           │
 * └──────────────────────────────────────────────────────────────────────────┘
 *
 * Quick-start
 * ───────────
 * @code
 * #include "EmbCoroutines/EmbCoroutines.hpp"
 *
 * emb::DefaultTask my_task() {
 *     while (true) {
 *         co_await emb::yield();
 *     }
 * }
 *
 * int main() {
 *     auto t = my_task();
 *     while (true) {
 *         emb::g_timer.tick();            // call from SysTick ISR
 *         emb::g_scheduler.run_once();    // call from main loop
 *     }
 * }
 * @endcode
 *
 * Compile-time configuration
 * ──────────────────────────
 * All tuneable knobs live in Config.hpp.  Override them BEFORE including
 * this header, or pass them on the compiler command line:
 *
 *   -DEMB_SCHEDULER_QUEUE_DEPTH=64
 *   -DEMB_MAX_TIMERS=32
 *   -DEMB_TRACE_ENABLED=1
 *
 * Component inventory
 * ───────────────────
 *   Config.hpp       — Compile-time knobs and global limits.
 *   Assert.hpp       — EMB_ASSERT, EMB_TRAP, EMB_TRACE, EMB_WARN.
 *   StaticPool.hpp   — Slot allocator: StaticPool<SlotSize, SlotCount>.
 *   Scheduler.hpp    — Round-robin ring-buffer: Scheduler<QueueDepth>.
 *   Timer.hpp        — Tick-driven sleep: TimerManager<MaxTimers>, sleep_ticks().
 *   Task.hpp         — Coroutine return type: Task<FrameSize, MaxConcurrent>.
 *   Awaitables.hpp   — yield(), join().
 *   Event.hpp        — Binary signal: Event.
 *   EventGroup.hpp   — Multi-bit flags: EventGroup<MaxWaiters>.
 *   Channel.hpp      — Typed SPSC queue: Channel<T, Capacity>.
 *   Semaphore.hpp    — Counting semaphore: Semaphore<MaxWaiters>.
 *   Mutex.hpp        — Cooperative mutex + RAII guard: Mutex<MaxWaiters>.
 *
 * @version  1.0.0
 * @author   EmbCoroutines Contributors
 * @license  MIT
 */
#pragma once

// ── 1. Configuration — must come first ───────────────────────────────────────
#include "Config.hpp"

// ── 2. Diagnostics infrastructure ────────────────────────────────────────────
#include "Assert.hpp"

// ── 3. Memory allocator ───────────────────────────────────────────────────────
#include "StaticPool.hpp"

// ── 4. Execution engine ───────────────────────────────────────────────────────
#include "Scheduler.hpp"
#include "Timer.hpp"

// ── 5. Coroutine return type ──────────────────────────────────────────────────
#include "Task.hpp"

// ── 6. Built-in awaitables ────────────────────────────────────────────────────
#include "Awaitables.hpp"

// ── 7. Synchronisation primitives ────────────────────────────────────────────
#include "Event.hpp"
#include "EventGroup.hpp"
#include "Channel.hpp"
#include "Semaphore.hpp"
#include "Mutex.hpp"

// ─────────────────────────────────────────────────────────────────────────────
//  Version string (accessible at runtime for logging / version checks)
// ─────────────────────────────────────────────────────────────────────────────
namespace emb {

/// Library version encoded as (major << 16) | (minor << 8) | patch.
inline constexpr uint32_t kVersion = (1u << 16) | (0u << 8) | 0u;

/// Human-readable version string.
inline constexpr const char* kVersionStr = "1.0.0";

} // namespace emb
