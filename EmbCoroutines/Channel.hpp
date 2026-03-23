/**
 * @file    Channel.hpp
 * @brief   Bounded, typed SPSC message channel with coroutine-awaitable
 *          send and receive operations.
 *
 * @details Channel<T, Capacity> is a fixed-size circular FIFO buffer.
 *          Both the producer and consumer sides have awaitable operations
 *          that suspend when the channel is full (send) or empty (receive).
 *
 * Concurrency Model
 * ─────────────────
 *  • SPSC (Single Producer, Single Consumer) — at most one coroutine may
 *    be blocked on send at any time, and at most one on receive.
 *  • For multiple producers or consumers, guard with a Semaphore or Mutex.
 *
 * Direct-Handoff Design
 * ─────────────────────
 *  When a writer is suspended (buffer full) and a reader pops a value:
 *    1. The reader's await_resume() pushes the writer's pending value
 *       directly into the now-free slot (direct handoff).
 *    2. The reader schedules the writer to wake it.
 *    3. The writer's await_resume() is called — because the push was
 *       already performed by the reader, the writer simply returns.
 *
 *  This eliminates a potential ABA race where another producer could
 *  fill the freed slot between the reader's dequeue and the writer's
 *  actual push.
 *
 * Template Parameters
 * ───────────────────
 * @tparam T         Element type.  Must be default-constructible and
 *                   copy-constructible.
 * @tparam Capacity  Maximum number of elements the channel can hold.
 *                   Must be >= 1.
 *
 * Example
 * ───────
 *   emb::Channel<SensorReading, 4> sensor_ch;
 *
 *   // Producer coroutine:
 *   co_await sensor_ch.send(SensorReading{42, 1024});
 *
 *   // Consumer coroutine:
 *   SensorReading r = co_await sensor_ch.receive();
 */
#pragma once

#include <coroutine>
#include <cstddef>
#include <utility>   // std::move

#include "Assert.hpp"
#include "Scheduler.hpp"

namespace emb {

// ─────────────────────────────────────────────────────────────────────────────

template<typename T, std::size_t Capacity>
class Channel {
    static_assert(Capacity > 0, "Channel Capacity must be at least 1.");

public:
    // ── Awaitable: send ───────────────────────────────────────────────────────

    /**
     * @brief  Send a value, suspending if the channel is full.
     *
     * @details If the channel has a free slot, the value is pushed immediately
     *          in await_resume() and any waiting receiver is woken.
     *          If full, the coroutine suspends; the next receiver to dequeue
     *          performs the push via direct handoff.
     *
     * @code
     *   co_await channel.send(my_value);
     * @endcode
     */
    [[nodiscard]] auto send(T value) noexcept {
        struct SendAwaitable {
            Channel* ch;
            T        val;
            bool     m_did_suspend = false;

            /// Buffer has room — don't bother suspending.
            bool await_ready() noexcept { return ch->m_count < Capacity; }

            /// Buffer is full — park this coroutine and stash the value.
            void await_suspend(std::coroutine_handle<> h) noexcept {
                m_did_suspend     = true;
                ch->m_pending_write = val;  // Stash for reader to do handoff
                ch->m_writer        = h;
                EMB_TRACE("Channel::send() — full, suspending writer.\n");
            }

            /**
             * @brief  Perform the actual buffer write.
             *
             *  Non-suspended path (m_did_suspend == false):
             *    Space was available; we push val now and wake any blocked reader.
             *
             *  Suspended path (m_did_suspend == true):
             *    The reader performed the push for us via direct handoff.
             *    Nothing to do here.
             */
            void await_resume() noexcept {
                if (!m_did_suspend) {
                    ch->do_push(val);
                    if (ch->m_reader) {
                        auto h       = ch->m_reader;
                        ch->m_reader = {};
                        g_scheduler.schedule(h);
                        EMB_TRACE("Channel::send() — pushed and woke reader.\n");
                    }
                }
                // Suspended path: reader already pushed our value.
            }
        };
        return SendAwaitable{this, std::move(value)};
    }

    // ── Awaitable: receive ────────────────────────────────────────────────────

    /**
     * @brief  Receive a value, suspending if the channel is empty.
     *
     * @code
     *   T value = co_await channel.receive();
     * @endcode
     */
    [[nodiscard]] auto receive() noexcept {
        struct ReceiveAwaitable {
            Channel* ch;

            /// Data is available — don't suspend.
            bool await_ready() noexcept { return ch->m_count > 0; }

            /// Channel is empty — park the reader.
            void await_suspend(std::coroutine_handle<> h) noexcept {
                ch->m_reader = h;
                EMB_TRACE("Channel::receive() — empty, suspending reader.\n");
            }

            /**
             * @brief  Pop and return the front value.
             *
             *  After popping, if a blocked writer is present:
             *    • Its pending value is pushed into the freed slot (direct handoff).
             *    • The writer is scheduled to resume.
             */
            T await_resume() noexcept {
                T value = ch->do_pop();

                if (ch->m_writer) {
                    // Direct handoff: push the waiting writer's value right here.
                    ch->do_push(ch->m_pending_write);
                    auto h       = ch->m_writer;
                    ch->m_writer = {};
                    g_scheduler.schedule(h);
                    EMB_TRACE("Channel::receive() — popped + writer handoff performed.\n");
                }

                return value;
            }
        };
        return ReceiveAwaitable{this};
    }

    // ── Non-blocking helpers ──────────────────────────────────────────────────

    /**
     * @brief  Attempt to push without suspending.
     * @return true on success, false if channel is full.
     */
    bool try_send(const T& value) noexcept {
        if (m_count == Capacity) return false;
        do_push(value);
        if (m_reader) {
            auto h   = m_reader;
            m_reader = {};
            g_scheduler.schedule(h);
        }
        return true;
    }

    /**
     * @brief  Attempt to pop without suspending.
     * @param  out  Receives the dequeued value on success.
     * @return true on success, false if channel is empty.
     */
    bool try_receive(T& out) noexcept {
        if (m_count == 0) return false;
        out = do_pop();
        if (m_writer) {
            do_push(m_pending_write);
            auto h   = m_writer;
            m_writer = {};
            g_scheduler.schedule(h);
        }
        return true;
    }

    // ── Inspection ───────────────────────────────────────────────────────────

    std::size_t size()     const noexcept { return m_count;              }
    bool        empty()    const noexcept { return m_count == 0;         }
    bool        full()     const noexcept { return m_count == Capacity;  }
    bool        has_writer_waiting() const noexcept { return static_cast<bool>(m_writer); }
    bool        has_reader_waiting() const noexcept { return static_cast<bool>(m_reader); }

private:
    // ── Ring-buffer helpers ───────────────────────────────────────────────────

    void do_push(const T& value) noexcept {
        m_buffer[m_tail] = value;
        m_tail = (m_tail + 1) % Capacity;
        ++m_count;
    }

    T do_pop() noexcept {
        T value  = m_buffer[m_head];
        m_head   = (m_head + 1) % Capacity;
        --m_count;
        return value;
    }

    // ── Data members ─────────────────────────────────────────────────────────

    T                       m_buffer[Capacity]{};   ///< Ring buffer
    std::size_t             m_head  = 0;
    std::size_t             m_tail  = 0;
    std::size_t             m_count = 0;

    T                       m_pending_write{};      ///< Value stashed by a blocked writer
    std::coroutine_handle<> m_writer{};             ///< Blocked sender (or null)
    std::coroutine_handle<> m_reader{};             ///< Blocked receiver (or null)
};

} // namespace emb
