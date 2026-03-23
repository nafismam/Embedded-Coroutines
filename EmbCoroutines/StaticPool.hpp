/**
 * @file    StaticPool.hpp
 * @brief   Fixed-size, zero-heap memory pool used to back coroutine frames.
 *
 * @details StaticPool<SlotSize, SlotCount> provides a simple first-fit
 *          allocator over a statically declared byte array.  It is used
 *          exclusively by Task<>::promise_type::operator new/delete so that
 *          coroutine frames are never sourced from the heap.
 *
 * Template Parameters
 * ───────────────────
 *  @tparam SlotSize   Bytes reserved per slot.  Must be >= the coroutine
 *                     frame size reported by the compiler.  If too small,
 *                     EMB_ASSERT fires on the first allocation.
 *  @tparam SlotCount  Maximum number of simultaneously alive coroutines of
 *                     this Task<> type.  Pool exhaustion triggers EMB_TRAP.
 *
 * Allocation Complexity
 * ─────────────────────
 *  allocate()   O(SlotCount) worst-case linear scan.
 *  deallocate() O(SlotCount) worst-case linear scan.
 *  Both are bounded constants in practice (typically << 10 iterations).
 *
 * Thread / ISR Safety
 * ───────────────────
 *  Not ISR-safe by default.  Wrap calls in a critical section if coroutines
 *  can be created or destroyed from interrupt context.
 */
#pragma once

#include <cstddef>
#include <cstdint>
#include <new>         // std::max_align_t

#include "Assert.hpp"

namespace emb {

// ─────────────────────────────────────────────────────────────────────────────

template<std::size_t SlotSize, std::size_t SlotCount>
class StaticPool {
    static_assert(SlotSize  > 0, "SlotSize must be > 0");
    static_assert(SlotCount > 0, "SlotCount must be > 0");

public:
    // ── Compile-time constants ───────────────────────────────────────────────

    static constexpr std::size_t k_slot_size  = SlotSize;
    static constexpr std::size_t k_slot_count = SlotCount;
    static constexpr std::size_t k_total_bytes = SlotSize * SlotCount;

    // ── Runtime statistics (collected when EMB_STATS_ENABLED=1) ─────────────

    struct Stats {
        std::size_t used_slots         = 0;  ///< Currently occupied slots
        std::size_t peak_used_slots    = 0;  ///< Historical maximum occupancy
        std::size_t total_allocations  = 0;  ///< Lifetime successful allocs
        std::size_t total_deallocations= 0;  ///< Lifetime successful deallocs
        std::size_t allocation_failures= 0;  ///< Pool-exhausted hits (should be 0)
    };

    // ── Allocation ───────────────────────────────────────────────────────────

    /**
     * @brief  Allocate one slot from the pool.
     *
     * @param  requested_size  The size the compiler passes to operator new.
     *                         Asserts if larger than SlotSize.
     * @return Pointer to the allocated slot.  Never returns nullptr — traps
     *         on pool exhaustion instead (embedded "fail-fast" policy).
     */
    [[nodiscard]] void* allocate(std::size_t requested_size) noexcept {
        EMB_ASSERT(requested_size <= SlotSize,
            "Coroutine frame exceeds pool slot size. "
            "Increase the FrameSize template parameter of your Task<>.");

        for (std::size_t i = 0; i < SlotCount; ++i) {
            if (!m_used[i]) {
                m_used[i] = true;

#if EMB_STATS_ENABLED
                ++m_stats.used_slots;
                ++m_stats.total_allocations;
                if (m_stats.used_slots > m_stats.peak_used_slots) {
                    m_stats.peak_used_slots = m_stats.used_slots;
                }
#endif
                EMB_TRACE("StaticPool: allocated slot %zu "
                          "(%zu B requested, %zu B reserved), "
                          "used=%zu/%zu\n",
                          i, requested_size, SlotSize,
                          m_stats.used_slots, SlotCount);

                return static_cast<void*>(m_slots[i].data);
            }
        }

#if EMB_STATS_ENABLED
        ++m_stats.allocation_failures;
#endif
        // Pool exhausted — halt.  On a healthy system this line is never
        // reached; treat it as a configuration error (increase SlotCount).
        EMB_TRAP("StaticPool exhausted: all slots are occupied. "
                 "Increase the MaxConcurrent template parameter of your Task<>.");
    }

    /**
     * @brief  Return a previously allocated slot back to the pool.
     * @param  ptr  Must be a pointer previously returned by allocate().
     *              Passing an unknown pointer triggers EMB_TRAP.
     */
    void deallocate(void* ptr) noexcept {
        if (ptr == nullptr) return;

        for (std::size_t i = 0; i < SlotCount; ++i) {
            if (static_cast<void*>(m_slots[i].data) == ptr) {
                EMB_ASSERT(m_used[i],
                    "StaticPool: double-free detected (slot already free).");
                m_used[i] = false;

#if EMB_STATS_ENABLED
                --m_stats.used_slots;
                ++m_stats.total_deallocations;
#endif
                EMB_TRACE("StaticPool: freed slot %zu, used=%zu/%zu\n",
                          i, m_stats.used_slots, SlotCount);
                return;
            }
        }
        EMB_TRAP("StaticPool: deallocate() called with unrecognised pointer "
                 "(pointer does not belong to this pool).");
    }

    // ── Inspection ───────────────────────────────────────────────────────────

    const Stats&  stats()       const noexcept { return m_stats;                     }
    std::size_t   used_count()  const noexcept { return m_stats.used_slots;           }
    std::size_t   free_count()  const noexcept { return SlotCount - m_stats.used_slots; }
    bool          is_full()     const noexcept { return m_stats.used_slots == SlotCount; }
    bool          is_empty()    const noexcept { return m_stats.used_slots == 0;      }

    /// Print a human-readable pool summary to stdout.
    void print_stats(const char* name = "StaticPool") const noexcept {
        ::printf("[%s] slots=%zu/%zu  peak=%zu  allocs=%zu  deallocs=%zu  failures=%zu\n",
                 name,
                 m_stats.used_slots, SlotCount,
                 m_stats.peak_used_slots,
                 m_stats.total_allocations,
                 m_stats.total_deallocations,
                 m_stats.allocation_failures);
    }

private:
    // Each slot is maximally aligned so any type can be placed within it.
    struct alignas(std::max_align_t) Slot {
        uint8_t data[SlotSize];
    };

    Slot  m_slots[SlotCount]{};  ///< Raw storage
    bool  m_used [SlotCount]{};  ///< Occupancy bitmap
    Stats m_stats{};             ///< Runtime counters
};

} // namespace emb
