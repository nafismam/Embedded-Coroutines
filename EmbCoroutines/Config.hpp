/**
 * @file    Config.hpp
 * @brief   Compile-time configuration knobs for the EmbCoroutines library.
 *
 * @details Every parameter here can be overridden either:
 *   (a) Before the first inclusion of EmbCoroutines.hpp:
 *           #define EMB_SCHEDULER_QUEUE_DEPTH 64
 *           #include "EmbCoroutines/EmbCoroutines.hpp"
 *
 *   (b) As a compiler flag (recommended for project-wide settings):
 *           -DEMB_SCHEDULER_QUEUE_DEPTH=64
 *           -DEMB_TRACE_ENABLED=1
 *
 * @note    All size constants should remain powers-of-2 where noted for optimal
 *          ring-buffer performance (compiler can replace % with bitwise AND).
 */
#pragma once

#include <cstddef>
#include <cstdint>

// ─────────────────────────────────────────────────────────────────────────────
//  Scheduler
// ─────────────────────────────────────────────────────────────────────────────

/// Depth of the global scheduler's ready-task ring buffer.
/// Must be large enough to hold all tasks that could be woken simultaneously
/// (e.g. N events fire inside a single ISR).
/// @note Keep as a power-of-2 for efficient modulo.
#ifndef EMB_SCHEDULER_QUEUE_DEPTH
#  define EMB_SCHEDULER_QUEUE_DEPTH 32u
#endif

// ─────────────────────────────────────────────────────────────────────────────
//  Timer Manager
// ─────────────────────────────────────────────────────────────────────────────

/// Maximum simultaneous sleep_ticks() registrations across all coroutines.
#ifndef EMB_MAX_TIMERS
#  define EMB_MAX_TIMERS 16u
#endif

// ─────────────────────────────────────────────────────────────────────────────
//  Task Defaults  (used when Task<> is declared without template arguments)
// ─────────────────────────────────────────────────────────────────────────────

/// Default coroutine frame-size estimate in bytes.
/// If you get a StaticPool assert at runtime, increase this value or
/// specify an explicit FrameSize in your Task<FrameSize, MaxConcurrent>.
///
/// Practical rule-of-thumb:
///   frame ≈ sizeof(all local variables) + sizeof(promise_type) + compiler overhead (~64 B)
/// Use -fno-omit-frame-pointer and a map file / size tool to measure precisely.
#ifndef EMB_DEFAULT_FRAME_SIZE
#  define EMB_DEFAULT_FRAME_SIZE 512u
#endif

/// Default maximum alive instances of Task<> (with no explicit template args).
#ifndef EMB_DEFAULT_MAX_TASKS
#  define EMB_DEFAULT_MAX_TASKS 8u
#endif

// ─────────────────────────────────────────────────────────────────────────────
//  Synchronisation Primitives — Waiter Queue Depths
// ─────────────────────────────────────────────────────────────────────────────

/// Default maximum blocked waiters on a single Semaphore instance.
#ifndef EMB_DEFAULT_SEMAPHORE_WAITERS
#  define EMB_DEFAULT_SEMAPHORE_WAITERS 4u
#endif

/// Default maximum blocked waiters on a single Mutex instance.
#ifndef EMB_DEFAULT_MUTEX_WAITERS
#  define EMB_DEFAULT_MUTEX_WAITERS 4u
#endif

/// Default maximum blocked waiters on a single EventGroup instance.
#ifndef EMB_DEFAULT_EVENT_GROUP_WAITERS
#  define EMB_DEFAULT_EVENT_GROUP_WAITERS 8u
#endif

// ─────────────────────────────────────────────────────────────────────────────
//  Diagnostics & Tracing
// ─────────────────────────────────────────────────────────────────────────────

/// Set to 1 during bring-up to enable verbose per-event printf tracing.
/// MUST be 0 in production firmware — all EMB_TRACE calls compile away to
/// nothing, eliminating every associated printf and format string.
#ifndef EMB_TRACE_ENABLED
#  define EMB_TRACE_ENABLED 0
#endif

/// Set to 1 to collect cumulative statistics in Scheduler and StaticPool.
/// Has a tiny RAM cost (~4 bytes per counter). Safe to leave on in production.
#ifndef EMB_STATS_ENABLED
#  define EMB_STATS_ENABLED 1
#endif
