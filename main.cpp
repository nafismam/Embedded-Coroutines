/**
 * @file    main.cpp
 * @brief   Comprehensive use-case examples for the EmbCoroutines library.
 *
 * @details This file demonstrates every major primitive provided by
 *          EmbCoroutines through a series of realistic embedded scenarios.
 *          Each demo is isolated in its own namespace and can be run
 *          independently.
 *
 * ┌──────────────────────────────────────────────────────────────────────────┐
 * │  Demo inventory                                                          │
 * │                                                                          │
 * │  1. UART Protocol Parser      — single-byte state machine via Channel   │
 * │  2. Multi-LED Blinker         — independent timing via sleep_ticks()    │
 * │  3. Sensor Pipeline           — producer/consumer with Channel<>        │
 * │  4. ISR ↔ Coroutine Handoff   — Event / EventGroup with set_from_isr()  │
 * │  5. Resource Arbitration      — Semaphore limiting concurrent SPI users │
 * │  6. Shared-Bus Mutex          — Mutex + MutexGuard protecting I²C bus   │
 * │  7. Task Lifecycle & yield()  — cooperative scheduling, join()          │
 * │  8. Statistics Dump           — pool, scheduler, timer stats at a glance│
 * └──────────────────────────────────────────────────────────────────────────┘
 *
 * Build (host machine, for testing on desktop):
 *   g++ -std=c++20 -fcoroutines -I. -O2 -Wall -Wextra main.cpp -o demo
 *   clang++ -std=c++20 -I. -O2 -Wall -Wextra main.cpp -o demo
 *
 * For ARM Cortex-M targets:
 *   arm-none-eabi-g++ -std=c++20 -mcpu=cortex-m4 -mthumb \
 *       -fno-exceptions -fno-rtti -fno-threadsafe-statics  \
 *       -I. -Os -Wall -Wextra main.cpp -o demo.elf
 */

#include <cstdio>
#include <cstdint>
#include <cinttypes>

// ── Single master include ─────────────────────────────────────────────────────
#include "EmbCoroutines/EmbCoroutines.hpp"

// ═════════════════════════════════════════════════════════════════════════════
//  Printing helpers
// ═════════════════════════════════════════════════════════════════════════════

/// Coloured section banner so each demo stands out in the console.
static void print_banner(const char* title) {
    printf("\n");
    printf("╔══════════════════════════════════════════════════════════════╗\n");
    printf("║  %-60s║\n", title);
    printf("╚══════════════════════════════════════════════════════════════╝\n");
}

static void print_separator(const char* label = nullptr) {
    if (label)
        printf("  ── %s ─────────────────────────────────────────────────────\n", label);
    else
        printf("  ────────────────────────────────────────────────────────────\n");
}

// ═════════════════════════════════════════════════════════════════════════════
//  Simulated hardware abstraction
//
//  On a real MCU these would be volatile registers or inline functions
//  calling HAL drivers.  Here they are simple globals so the demo can
//  run on a host machine.
// ═════════════════════════════════════════════════════════════════════════════

namespace hw {

/// Simulated UART receive register — written by the "ISR" in main().
volatile uint8_t UART_RDR = 0;

/// Simulated GPIO pin states (0 = LED off, 1 = LED on).
volatile int LED_RED   = 0;
volatile int LED_GREEN = 0;
volatile int LED_BLUE  = 0;

/// Simulated ADC result register (12-bit raw value).
volatile uint16_t ADC_DR = 0;

/// Simulated SPI transfer result.
volatile uint8_t SPI_RXDR = 0;

/// Simulated I2C data byte.
volatile uint8_t I2C_RXDR = 0;

/// Toggle a named LED and log it.
void set_led(const char* colour, volatile int& pin, int state) {
    pin = state;
    printf("    [HW] LED %-6s → %s\n", colour, state ? "ON " : "OFF");
}

} // namespace hw

// ═════════════════════════════════════════════════════════════════════════════
//  Tick counter — used everywhere as a simulated SysTick
// ═════════════════════════════════════════════════════════════════════════════

static uint32_t g_tick = 0;

/// Advance the simulated tick counter and feed the timer manager.
static void advance_tick(uint32_t n = 1) {
    for (uint32_t i = 0; i < n; ++i) {
        ++g_tick;
        emb::g_timer.tick();
    }
}

/// Run the scheduler until idle, then advance one tick.
static void step() {
    emb::g_scheduler.run_until_idle();
    advance_tick();
}

// ═════════════════════════════════════════════════════════════════════════════
//
//  Demo 1 — UART Protocol Parser
//  ─────────────────────────────
//  Models a 3-byte packet:  [0xAA] [CMD] [DATA]
//
//  Architecture:
//    • A Channel<uint8_t, 8> acts as a lock-free byte FIFO between the
//      simulated ISR and the parser coroutine.
//    • The parser co_awaits channel.receive() for each byte, so it is
//      suspended (and uses zero CPU) whenever the channel is empty.
//    • The ISR path calls channel.try_send() from a non-coroutine context.
//
// ═════════════════════════════════════════════════════════════════════════════

namespace demo_uart {

// 8-byte channel — enough to buffer a burst of 2 full packets before the
// parser drains them.
emb::Channel<uint8_t, 8> uart_channel;

// A small Task type: 256-byte frame, up to 2 concurrent parsers.
using ParserTask = emb::Task<1024, 2>;

// Counter incremented each time a valid packet is fully decoded.
static int packets_received = 0;

/**
 * @brief  Protocol parser coroutine.
 *
 * @details Runs as a state machine driven purely by co_await on the channel:
 *
 *   State 1 — Sync byte (0xAA)
 *     Wait for 0xAA; discard and warn on any other value.
 *
 *   State 2 — Command byte
 *     Accept any byte; stored into `cmd`.
 *
 *   State 3 — Data byte
 *     Accept any byte; packet is complete.
 *
 *   Repeat.
 *
 * The coroutine never polls — it is suspended at each `co_await` and resumed
 * only when a byte is available.
 */
ParserTask protocol_parser() {
    printf("  [Parser] Coroutine started, waiting for packets…\n");

    while (true) {
        // ── STATE 1: Hunt for the sync byte ──────────────────────────────────
        uint8_t sync = co_await uart_channel.receive();

        if (sync != 0xAA) {
            printf("  [Parser] Framing error: expected 0xAA, got 0x%02X — "
                   "discarding byte and re-syncing.\n", sync);
            continue;  // loop back and wait for the next byte
        }

        printf("  [Parser] Sync byte received (0xAA).\n");

        // ── STATE 2: Read command byte ────────────────────────────────────────
        uint8_t cmd  = co_await uart_channel.receive();
        printf("  [Parser] Command byte received: 0x%02X (%u).\n", cmd, cmd);

        // ── STATE 3: Read data byte ───────────────────────────────────────────
        uint8_t data = co_await uart_channel.receive();
        printf("  [Parser] Data byte received: 0x%02X (%u).\n", data, data);

        // ── Packet complete ───────────────────────────────────────────────────
        printf("  [Parser] ★ PACKET #%d COMPLETE — CMD=0x%02X  DATA=0x%02X\n",
               ++packets_received, cmd, data);
    }
}

/**
 * @brief  Simulate the UART receive ISR feeding one byte into the channel.
 *         On a real MCU, this would be called from the USART_IRQHandler.
 */
static void isr_receive(uint8_t byte) {
    printf("  [ISR]    UART received byte: 0x%02X\n", byte);
    bool ok = uart_channel.try_send(byte);
    if (!ok) {
        printf("  [ISR]    WARNING — channel full, byte 0x%02X dropped!\n", byte);
    }
}

void run() {
    print_banner("Demo 1 — UART Protocol Parser");

    // Start the parser coroutine.  It suspends immediately at the first
    // co_await uart_channel.receive() since the channel is empty.
    auto parser = protocol_parser();

    printf("\n  Feeding a stream with framing errors and two valid packets:\n\n");

    // Stream: a garbage byte, then two valid packets, with interleaved steps.
    const uint8_t stream[] = {
        0xFF,        // framing error — should be discarded
        0xAA, 0x01, 0x42,  // valid packet: CMD=1, DATA=0x42
        0xAA, 0x02, 0x99,  // valid packet: CMD=2, DATA=0x99
    };

    for (uint8_t b : stream) {
        isr_receive(b);
        // Let the scheduler run — gives the parser a chance to consume bytes.
        emb::g_scheduler.run_until_idle();
    }

    printf("\n  Total packets decoded: %d (expected 2)\n", packets_received);
}

} // namespace demo_uart

// ═════════════════════════════════════════════════════════════════════════════
//
//  Demo 2 — Multi-LED Blinker with Independent Timing
//  ───────────────────────────────────────────────────
//  Three LEDs (Red, Green, Blue) blink at different tick rates.  Each LED
//  is controlled by its own coroutine that calls co_await emb::sleep_ticks().
//  They share one cooperative thread — no pre-emption, no RTOS.
//
// ═════════════════════════════════════════════════════════════════════════════

namespace demo_blinker {

using BlinkerTask = emb::Task<1024, 4>;

/**
 * @brief  Generic LED blinker.  Toggles an LED every `half_period` ticks.
 *
 * @param  colour       Human-readable LED name for logging.
 * @param  pin          Reference to the simulated LED GPIO register.
 * @param  half_period  Ticks between each toggle (half the full blink period).
 */
BlinkerTask led_blinker(const char* colour,
                         volatile int& pin,
                         uint32_t half_period) {
    printf("  [Blinker] %s LED task started (period=%u ticks).\n",
           colour, half_period * 2);

    int state = 0;
    while (true) {
        state ^= 1;                           // toggle
        hw::set_led(colour, pin, state);
        co_await emb::sleep_ticks(half_period);
    }
}

void run() {
    print_banner("Demo 2 — Multi-LED Blinker (Independent Timing)");

    printf("\n  Creating three blinker coroutines:\n");
    printf("    Red   blinks every  4 ticks\n");
    printf("    Green blinks every  6 ticks\n");
    printf("    Blue  blinks every 10 ticks\n\n");

    auto t_red   = led_blinker("Red",   hw::LED_RED,   2);   // 4-tick period
    auto t_green = led_blinker("Green", hw::LED_GREEN, 3);   // 6-tick period
    auto t_blue  = led_blinker("Blue",  hw::LED_BLUE,  5);   // 10-tick period

    // Run for 20 ticks so we can observe the different blink rates.
    printf("  Running scheduler for 20 ticks…\n\n");
    for (int i = 0; i < 20; ++i) {
        printf("  ── Tick %2d ─────────────────────────────────────────────────\n", i + 1);
        step();
    }

    printf("\n  Final LED states: RED=%d  GREEN=%d  BLUE=%d\n",
           hw::LED_RED, hw::LED_GREEN, hw::LED_BLUE);
}

} // namespace demo_blinker

// ═════════════════════════════════════════════════════════════════════════════
//
//  Demo 3 — Sensor Sampling Pipeline (Channel Producer / Consumer)
//  ───────────────────────────────────────────────────────────────
//  An ADC sampler coroutine periodically reads the ADC and sends raw values
//  into a Channel<uint16_t, 4>.  A filter coroutine consumes those values,
//  computes a moving average, and dispatches alerts when the value is high.
//
//  This decouples acquisition rate from processing rate — a common pattern
//  in sensor signal chains.
//
// ═════════════════════════════════════════════════════════════════════════════

namespace demo_pipeline {

emb::Channel<uint16_t, 4> adc_channel;   // buffered ADC readings

using PipelineTask = emb::Task<1024, 8>;

static int sample_count  = 0;
static int alert_count   = 0;

/**
 * @brief  ADC sampler — reads every 2 ticks and pushes to adc_channel.
 */
PipelineTask adc_sampler() {
    printf("  [Sampler] ADC sampler started (period = 2 ticks).\n");

    const uint16_t sequence[] = { 120, 340, 4020, 3900, 200, 150 };

    for (uint16_t raw : sequence) {
        co_await emb::sleep_ticks(2);

        hw::ADC_DR = raw;    // simulate hardware write
        printf("  [Sampler] ADC raw = %4u → pushing to channel.\n", raw);
        co_await adc_channel.send(raw);

        ++sample_count;
    }

    printf("  [Sampler] All samples sent.\n");
}

/**
 * @brief  Digital low-pass filter — consumes from adc_channel.
 *
 * @details Computes an exponential moving average (EMA) and fires an alert
 *          if the value exceeds a threshold (3000).
 */
PipelineTask adc_filter() {
    printf("  [Filter ] ADC filter started.\n");

    constexpr uint32_t ALERT_THRESHOLD = 3000;
    constexpr uint32_t ALPHA_NUM       = 1;    // EMA alpha = 1/4
    constexpr uint32_t ALPHA_DEN       = 4;

    uint32_t ema = 0;
    bool     first = true;

    while (true) {
        uint16_t raw = co_await adc_channel.receive();

        if (first) {
            ema   = raw;
            first = false;
        } else {
            // EMA update: ema = ema + alpha*(raw - ema) = (ema*(4-1) + raw) / 4
            ema = (ema * (ALPHA_DEN - ALPHA_NUM) + raw) / ALPHA_DEN;
        }

        printf("  [Filter ] raw=%4u  ema=%5" PRIu32, raw, ema);

        if (ema > ALERT_THRESHOLD) {
            printf("  ⚠ ALERT: EMA exceeded threshold %" PRIu32 "!\n",
                   ALERT_THRESHOLD);
            ++alert_count;
        } else {
            printf("  OK\n");
        }
    }
}

void run() {
    print_banner("Demo 3 — Sensor Pipeline (Producer / Consumer Channel)");

    auto sampler = adc_sampler();
    auto filter  = adc_filter();

    printf("\n  Running scheduler until pipeline is drained…\n\n");

    // Run for enough ticks to process all 6 samples.
    for (int i = 0; i < 20; ++i) {
        step();
    }

    printf("\n  Samples sent: %d  Alerts fired: %d\n",
           sample_count, alert_count);
}

} // namespace demo_pipeline

// ═════════════════════════════════════════════════════════════════════════════
//
//  Demo 4 — ISR to Coroutine Handoff (Event & EventGroup)
//  ────────────────────────────────────────────────────────
//  Part A: A single Event — one coroutine waits for a button press that is
//          signalled by a simulated ISR via event.set_from_isr().
//
//  Part B: An EventGroup — multiple coroutines each wait for different
//          combinations of hardware events (DMA complete, EXTI, timer
//          overflow) using wait_any() and wait_all().
//
// ═════════════════════════════════════════════════════════════════════════════

namespace demo_events {

// Part A: simple binary event
emb::Event button_event;

// Part B: multi-bit event group for hardware flags
//   Bit 0 = DMA transfer complete
//   Bit 1 = External interrupt (EXTI line 0)
//   Bit 2 = TIM2 overflow
static constexpr uint32_t EV_DMA   = (1u << 0);
static constexpr uint32_t EV_EXTI  = (1u << 1);
static constexpr uint32_t EV_TIM2  = (1u << 2);

emb::EventGroup<4> hw_events;

using EventTask = emb::Task<1024, 6>;

// ── Part A ────────────────────────────────────────────────────────────────────

EventTask button_handler() {
    printf("  [Button ] Handler started — waiting for button press.\n");

    int press_count = 0;
    while (press_count < 3) {
        co_await button_event.wait();   // auto-clears on consume
        ++press_count;
        printf("  [Button ] Button press #%d detected!\n", press_count);
    }

    printf("  [Button ] Received %d presses — task complete.\n", press_count);
}

// ── Part B ────────────────────────────────────────────────────────────────────

/// Waits for DMA complete alone (wait_any with single-bit mask).
EventTask dma_handler() {
    printf("  [DMA    ] Handler waiting for DMA complete (bit 0).\n");
    uint32_t bits = co_await hw_events.wait_any(EV_DMA, /*auto_clear=*/true);
    printf("  [DMA    ] DMA complete! triggered bits=0x%02" PRIX32 "\n", bits);
}

/// Waits for BOTH EXTI and TIM2 together (wait_all).
EventTask exti_tim2_handler() {
    printf("  [EXTI+T2] Handler waiting for EXTI AND TIM2 (bits 1+2).\n");
    uint32_t bits = co_await hw_events.wait_all(EV_EXTI | EV_TIM2,
                                                /*auto_clear=*/true);
    printf("  [EXTI+T2] Both events received! triggered bits=0x%02" PRIX32 "\n", bits);
}

/// Waits for ANY hardware event (broad monitor).
EventTask any_hw_monitor() {
    printf("  [Monitor] Hardware event monitor started (any of DMA|EXTI|TIM2).\n");
    while (true) {
        uint32_t bits = co_await hw_events.wait_any(EV_DMA | EV_EXTI | EV_TIM2,
                                                    /*auto_clear=*/false);
        printf("  [Monitor] Hardware event fired: bits=0x%02" PRIX32 "\n", bits);
        // In a real system: log telemetry, update a status register, etc.
        co_await emb::yield();   // yield so other coroutines can run
    }
}

void run() {
    print_banner("Demo 4 — ISR to Coroutine Handoff (Event / EventGroup)");

    // ── Part A: binary event ─────────────────────────────────────────────────
    print_separator("Part A: single Event (button)");

    auto btn = button_handler();
    emb::g_scheduler.run_until_idle();  // let the coroutine reach its co_await

    for (int i = 0; i < 3; ++i) {
        printf("\n  [ISR] Button interrupt fired.\n");
        button_event.set_from_isr();    // safe to call from interrupt context
        emb::g_scheduler.run_until_idle();
    }

    // ── Part B: event group ──────────────────────────────────────────────────
    print_separator("Part B: EventGroup (DMA / EXTI / TIM2)");

    auto t_dma     = dma_handler();
    auto t_exti_t2 = exti_tim2_handler();
    auto t_monitor = any_hw_monitor();

    emb::g_scheduler.run_until_idle();  // reach co_await

    printf("\n  [ISR] DMA transfer complete interrupt.\n");
    hw_events.set(EV_DMA);
    emb::g_scheduler.run_until_idle();

    printf("\n  [ISR] EXTI line 0 interrupt.\n");
    hw_events.set(EV_EXTI);
    emb::g_scheduler.run_until_idle();

    printf("\n  [ISR] TIM2 overflow interrupt.\n");
    hw_events.set(EV_TIM2);
    emb::g_scheduler.run_until_idle();
}

} // namespace demo_events

// ═════════════════════════════════════════════════════════════════════════════
//
//  Demo 5 — Resource Arbitration with Semaphore
//  ─────────────────────────────────────────────
//  Three coroutines ("SPI workers") want to execute SPI transactions, but
//  the hardware SPI bus can only handle 2 simultaneous DMA channels.
//  A Semaphore(2) limits concurrency; the third worker must wait.
//
// ═════════════════════════════════════════════════════════════════════════════

namespace demo_semaphore {

// Semaphore with initial count 2 → at most 2 concurrent SPI users.
emb::Semaphore<4>  spi_slots{2};

using WorkerTask = emb::Task<1024, 8>;

static int transactions_completed = 0;

/**
 * @brief  SPI worker — acquires a DMA slot, runs a transaction, releases.
 *
 * @param  id    Worker identifier for logging.
 * @param  bytes Number of bytes to "transfer".
 */
WorkerTask spi_worker(int id, uint32_t transfer_ticks) {
    printf("  [Worker %d] Started — waiting for SPI DMA slot.\n", id);

    co_await spi_slots.acquire();   // blocks if both slots are taken

    printf("  [Worker %d] DMA slot acquired — starting %u-tick SPI transfer.\n",
           id, transfer_ticks);

    co_await emb::sleep_ticks(transfer_ticks);  // simulate transfer time

    printf("  [Worker %d] SPI transfer complete — releasing DMA slot.\n", id);
    spi_slots.release();

    ++transactions_completed;
}

void run() {
    print_banner("Demo 5 — Semaphore: Limiting Concurrent SPI DMA Channels");

    printf("\n  SPI semaphore count = 2  (max 2 simultaneous DMA transfers)\n");
    printf("  Launching 3 workers — Worker 3 must wait for a free slot.\n\n");

    auto w1 = spi_worker(1, 4);   // takes slot, finishes at tick 4
    auto w2 = spi_worker(2, 6);   // takes slot, finishes at tick 6
    auto w3 = spi_worker(3, 3);   // must wait — both slots occupied

    printf("\n  Running scheduler for 12 ticks…\n\n");
    for (int i = 0; i < 12; ++i) {
        printf("  ── Tick %2d ─────────────────────────────────────────────────\n", i + 1);
        step();
    }

    printf("\n  Transactions completed: %d (expected 3)\n", transactions_completed);
}

} // namespace demo_semaphore

// ═════════════════════════════════════════════════════════════════════════════
//
//  Demo 6 — Shared-Bus Protection with Mutex
//  ──────────────────────────────────────────
//  Two coroutines ("I2C masters") share a single I²C peripheral.  Concurrent
//  access would corrupt transactions, so they both co_await i2c_mutex.lock()
//  before touching the bus.  MutexGuard ensures the lock is always released,
//  even if the critical section co_awaits internally (e.g., waiting for a
//  DMA completion event).
//
// ═════════════════════════════════════════════════════════════════════════════

namespace demo_mutex {

emb::Mutex<4> i2c_mutex;

using I2CTask = emb::Task<1024, 8>;

static int i2c_transfers_done = 0;

/**
 * @brief  I²C master coroutine.
 *
 * @details Acquires the mutex, initiates a "transfer" (simulated by
 *          sleep_ticks), then releases the mutex via RAII guard.  A second
 *          co_await inside the critical section proves that the mutex remains
 *          held across yield points (unlike a traditional spin-lock).
 */
I2CTask i2c_master(int id, uint8_t dev_addr, uint32_t transfer_ticks) {
    printf("  [I2C %d] Master started (device=0x%02X).\n", id, dev_addr);

    // Each master performs 2 transactions.
    for (int tx = 0; tx < 2; ++tx) {
        printf("  [I2C %d] Requesting bus lock for transaction %d…\n", id, tx + 1);

        {
            // co_await lock() returns a MutexGuard.
            // If another coroutine holds the lock, we are suspended here.
            auto guard = co_await i2c_mutex.lock();

            printf("  [I2C %d] Bus lock ACQUIRED — starting transfer "
                   "(addr=0x%02X, %u ticks).\n", id, dev_addr, transfer_ticks);

            // Simulate DMA transfer — we sleep inside the critical section!
            // The mutex remains held because `guard` is still in scope.
            co_await emb::sleep_ticks(transfer_ticks);

            hw::I2C_RXDR = static_cast<uint8_t>(dev_addr + tx);

            printf("  [I2C %d] Transfer complete — RX=0x%02X — releasing lock.\n",
                   id, hw::I2C_RXDR);
            ++i2c_transfers_done;

        } // guard destructor calls mutex.release() here

        // Pause between transactions so the other master gets a chance.
        co_await emb::sleep_ticks(1);
    }

    printf("  [I2C %d] All transactions done.\n", id);
}

void run() {
    print_banner("Demo 6 — Mutex: Protecting a Shared I²C Bus");

    printf("\n  Two I²C masters share one peripheral — Mutex serialises access.\n\n");

    auto m1 = i2c_master(1, 0x48, 3);  // temperature sensor
    auto m2 = i2c_master(2, 0x68, 2);  // IMU

    printf("\n  Running scheduler for 30 ticks…\n\n");
    for (int i = 0; i < 30; ++i) {
        step();
        if (m1.done() && m2.done()) {
            printf("  Both I2C masters finished at tick %d.\n", i + 1);
            break;
        }
    }

    printf("\n  Total I²C transfers completed: %d (expected 4)\n", i2c_transfers_done);

#if EMB_STATS_ENABLED
    print_separator("Mutex statistics");
    i2c_mutex.print_stats("i2c_mutex");
#endif
}

} // namespace demo_mutex

// ═════════════════════════════════════════════════════════════════════════════
//
//  Demo 7 — Task Lifecycle: yield(), join(), done()
//  ─────────────────────────────────────────────────
//  Demonstrates:
//   • yield() — voluntarily hand back the CPU to other coroutines.
//   • join()  — a parent coroutine awaits completion of a child task.
//   • done()  — polling check for task completion.
//
// ═════════════════════════════════════════════════════════════════════════════

namespace demo_lifecycle {

using LifeTask = emb::Task<1024, 6>;

/// A background worker that counts down and then completes.
LifeTask countdown_worker(int id, int count) {
    printf("  [Worker %d] Started — counting down from %d.\n", id, count);
    for (int i = count; i > 0; --i) {
        printf("  [Worker %d] Count = %d\n", id, i);
        co_await emb::yield();
    }
    printf("  [Worker %d] Reached zero — task complete.\n", id);
}

/**
 * @brief  Supervisor coroutine that spawns two workers and joins both.
 *
 * @details Demonstrates that a coroutine can itself be a task (scheduled and
 *          managed by the same scheduler) while waiting on children.
 */
LifeTask supervisor() {
    printf("  [Super  ] Supervisor started — spawning workers.\n");

    auto w1 = countdown_worker(1, 3);
    auto w2 = countdown_worker(2, 5);

    printf("  [Super  ] Both workers spawned — joining w1…\n");
    co_await emb::join(w1);          // suspend until w1 is done

    printf("  [Super  ] w1 joined — joining w2…\n");
    co_await emb::join(w2);          // suspend until w2 is done

    printf("  [Super  ] Both workers joined — supervisor complete.\n");
}

void run() {
    print_banner("Demo 7 — Task Lifecycle (yield / join / done)");

    auto super = supervisor();

    printf("\n  Running scheduler until all tasks complete…\n\n");
    for (int i = 0; i < 30; ++i) {
        emb::g_scheduler.run_until_idle();
        if (super.done()) {
            printf("  Supervisor (and all children) done at iteration %d.\n", i);
            break;
        }
    }
}

} // namespace demo_lifecycle

// ═════════════════════════════════════════════════════════════════════════════
//
//  Demo 8 — Statistics Dump
//  ─────────────────────────
//  Prints a summary of every observable metric accumulated across all demos.
//
// ═════════════════════════════════════════════════════════════════════════════

namespace demo_stats {

void run() {
    print_banner("Demo 8 — Library Statistics Summary");

#if EMB_STATS_ENABLED
    printf("\n");

    // ── Scheduler ────────────────────────────────────────────────────────────
    print_separator("Scheduler  (emb::g_scheduler)");
    emb::g_scheduler.print_stats();

    // ── Timer Manager ────────────────────────────────────────────────────────
    print_separator("Timer Manager  (emb::g_timer)");
    emb::g_timer.print_stats();

    // ── Task pool stats ───────────────────────────────────────────────────────
    print_separator("Task Static Pools");
    demo_uart::ParserTask::print_pool_stats    ("ParserTask pool  ");
    demo_blinker::BlinkerTask::print_pool_stats("BlinkerTask pool ");
    demo_pipeline::PipelineTask::print_pool_stats("PipelineTask pool");
    demo_events::EventTask::print_pool_stats   ("EventTask pool   ");
    demo_semaphore::WorkerTask::print_pool_stats("WorkerTask pool  ");
    demo_mutex::I2CTask::print_pool_stats      ("I2CTask pool     ");
    demo_lifecycle::LifeTask::print_pool_stats ("LifeTask pool    ");

    // ── Semaphore ────────────────────────────────────────────────────────────
    print_separator("Semaphore  (demo_semaphore::spi_slots)");
    printf("    current count : %d\n",
           demo_semaphore::spi_slots.count());
    printf("    waiter count  : %zu\n",
           demo_semaphore::spi_slots.waiter_count());

    // ── Mutex ─────────────────────────────────────────────────────────────────
    print_separator("Mutex  (demo_mutex::i2c_mutex)");
    demo_mutex::i2c_mutex.print_stats("i2c_mutex");

#else
    printf("\n  EMB_STATS_ENABLED is 0 — recompile with -DEMB_STATS_ENABLED=1\n"
           "  to see runtime statistics.\n");
#endif // EMB_STATS_ENABLED

    printf("\n");
}

} // namespace demo_stats

// ═════════════════════════════════════════════════════════════════════════════
//  main
// ═════════════════════════════════════════════════════════════════════════════

int main() {
    system("chcp 65001");
    printf("╔══════════════════════════════════════════════════════════════╗\n");
    printf("║      EmbCoroutines v%-8s — Full Feature Demo             ║\n",
           emb::kVersionStr);
    printf("║      Zero-heap  ·  Multi-task  ·  C++20  ·  ISR-safe       ║\n");
    printf("╚══════════════════════════════════════════════════════════════╝\n");

    demo_uart::run();
    demo_blinker::run();
    demo_pipeline::run();
    demo_events::run();
    demo_semaphore::run();
    demo_mutex::run();
    demo_lifecycle::run();
    demo_stats::run();

    printf("\n");
    printf("╔══════════════════════════════════════════════════════════════╗\n");
    printf("║  All demos complete.  Total simulated ticks: %-14" PRIu32 "║\n",
           g_tick);
    printf("╚══════════════════════════════════════════════════════════════╝\n");

    return 0;
}