
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>    // FIX: Must come before winsock2.h; provides Sleep()
#include <winsock2.h>
#include <ws2tcpip.h>
#include <coroutine>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <cstdlib>


// Best Proof
void* operator new(std::size_t size) {
    std::printf("\n[FATAL] Heap allocation attempted (%zu bytes)! Fix the code.\n", size);
    std::exit(1);
}
void operator delete(void*) noexcept {}
void operator delete(void*, std::size_t) noexcept {}




constexpr std::size_t MAX_TASKS  = 16;
constexpr std::size_t FRAME_SIZE = 8192;

struct FramePool {
    // alignas is platfrom dependent
    alignas(std::max_align_t) char slots[MAX_TASKS][FRAME_SIZE]{};
    bool used[MAX_TASKS]{};

    void* allocate(std::size_t n) noexcept {
        if (n > FRAME_SIZE) {
            std::printf("[POOL] Frame too large (%zu > %zu)! Increase FRAME_SIZE.\n",
                        n, FRAME_SIZE);
            return nullptr;
        }
        for (std::size_t i = 0; i < MAX_TASKS; ++i) {
            if (!used[i]) { used[i] = true; return slots[i]; }
        }
        return nullptr;
    }

    void deallocate(const void* p) noexcept {
        for (std::size_t i = 0; i < MAX_TASKS; ++i) {
            if (static_cast<const void*>(slots[i]) == p) {
                used[i] = false;
                return;
            }
        }
    }
} g_pool;



struct Task {
    struct promise_type {
        Task get_return_object() noexcept;

        // initial_suspend → suspend_always: the coroutine pauses immediately
        // on creation and won't run until we explicitly call handle.resume().
        // This gives our event loop full control over scheduling.
        [[nodiscard]] static std::suspend_always initial_suspend() noexcept { return {}; }

        // final_suspend → suspend_always: the coroutine stays alive (suspended)
        // after finishing so we can detect completion via handle.done().
        // If we used suspend_never here, the handle would self-destruct and
        // our ~Task() double-destroy would be UB.
        [[nodiscard]] static std::suspend_always final_suspend()   noexcept { return {}; }

        static void return_void()         noexcept {}
        static void unhandled_exception() noexcept {}

        // Intercept the compiler's frame allocation → use our pool.
        static void* operator new(std::size_t n) noexcept { return g_pool.allocate(n); }
        static void  operator delete(void* p)    noexcept { g_pool.deallocate(p); }

        // If operator new returned nullptr, the compiler calls this instead
        // of get_return_object(). We return an empty (invalid) Task.
        [[nodiscard]] static Task get_return_object_on_allocation_failure() noexcept;
    };

    std::coroutine_handle<promise_type> handle;

    [[nodiscard]] bool is_valid() const noexcept { return handle && !handle.done(); }

    // Advance the coroutine by one step (runs until the next co_await/co_return).
    // Returns true if the coroutine is still alive and has more work to do.
    bool resume() const noexcept {
        if (!handle || handle.done()) return false;
        handle.resume();
        return !handle.done();
    }

    // Default constructor produces an empty/invalid Task.
    Task() noexcept : handle(nullptr) {}

    Task(Task&& o) noexcept : handle(o.handle) { o.handle = nullptr; }
    Task& operator=(Task&& o) noexcept {
        if (this != &o) {
            if (handle) handle.destroy();
            handle = o.handle;
            o.handle = nullptr;
        }
        return *this;
    }
    Task(const Task&)            = delete;
    Task& operator=(const Task&) = delete;

    ~Task() { if (handle) handle.destroy(); }

    friend struct promise_type; // lets promise_type call the private constructor

private:
    explicit Task(std::coroutine_handle<promise_type> h) noexcept : handle(h) {}
};

Task Task::promise_type::get_return_object() noexcept {
    return Task{ std::coroutine_handle<promise_type>::from_promise(*this) };
}
Task Task::promise_type::get_return_object_on_allocation_failure() noexcept {
    return Task{};
}

//  co_await Yield{} is how a coroutine voluntarily gives up the CPU so the
//  event loop can service other connections. The three methods below are the
//  "awaitable protocol" the compiler calls:
//    await_ready()   → false means "don't skip the suspend, always pause"
//    await_suspend() → called with our handle; we do nothing (the event loop
//                      will call resume() on the next tick)
//    await_resume()  → called when we're resumed; nothing to return here
// this is kinda now Yield = std::suspend_always
struct Yield {
    [[nodiscard]] static bool await_ready()                          noexcept { return false; }
    static void               await_suspend(std::coroutine_handle<>) noexcept {}
    static void               await_resume()                         noexcept {}
};


//  This is where the magic is. The function looks like ordinary blocking code,
//  but every co_await Yield{} is actually a suspension point. The function
//  pauses, returns control to the event loop, and is resumed on the next tick.
//  No threads. No blocking. Just cooperative multitasking.

Task handle_client(SOCKET client_socket, int client_id) {
    char buffer[1024]{};
    int  bytes_read = 0;
    int  total_read = 0;

    // ── Phase 1: Wait for the full request ───────────────────────────────────
    while (total_read < static_cast<int>(sizeof(buffer) - 1)) {
        bytes_read = recv(client_socket,
                          buffer + total_read,
                          static_cast<int>(sizeof(buffer) - 1 - total_read),
                          0);

        if (bytes_read > 0) {
            total_read += bytes_read;
            buffer[total_read] = '\0'; // null-terminate what we currently have

            // Check if we reached the end of the HTTP headers
            if (std::strstr(buffer, "\r\n\r\n") != nullptr) {
                break; // Got the full request!
            }
            // If not, we just loop around. The next recv() will likely return
            // WSAEWOULDBLOCK, causing the coroutine to yield.
        }
        else if (bytes_read == 0) {
            std::printf("[Client %d] Disconnected cleanly.\n", client_id);
            (void)closesocket(client_socket);
            co_return;
        }
        else { // bytes_read < 0
            if (WSAGetLastError() == WSAEWOULDBLOCK) {
                co_await Yield{}; // ← suspend here, resume next event-loop tick
            } else {
                std::printf("[Client %d] recv() error %d.\n", client_id, WSAGetLastError());
                (void)closesocket(client_socket);
                co_return;
            }
        }
    }

    // ── Phase 2: Build the response ──────────────────────────────────────────
    const char* response = nullptr;

    if (std::strncmp(buffer, "GET", 3) == 0) {
        std::printf("[Client %d] GET request.\n", client_id);
        response =
            "HTTP/1.1 200 OK\r\n"
            "Content-Type: text/plain\r\n"
            "Content-Length: 26\r\n"
            "Connection: close\r\n"
            "\r\n"
            "Hello from GET Coroutine!\n";
    } else if (std::strncmp(buffer, "POST", 4) == 0) {
        std::printf("[Client %d] POST request.\n", client_id);
        response =
            "HTTP/1.1 200 OK\r\n"
            "Content-Type: text/plain\r\n"
            "Content-Length: 27\r\n"
            "Connection: close\r\n"
            "\r\n"
            "Hello from POST Coroutine!\n";
    } else {
        std::printf("[Client %d] Unknown request.\n", client_id);
        response =
            "HTTP/1.1 400 Bad Request\r\n"
            "Content-Length: 11\r\n"
            "Connection: close\r\n"
            "\r\n"
            "Bad Request";
    }

    // ── Phase 3: Send the response (also non-blocking) ───────────────────────
    std::size_t total_sent = 0;
    std::size_t to_send    = std::strlen(response);

    while (total_sent < to_send) {
        int sent = send(client_socket,
                        response + total_sent,
                        static_cast<int>(to_send - total_sent),
                        0);
        if (sent > 0) {
            total_sent += sent;
        } else {
            if (WSAGetLastError() == WSAEWOULDBLOCK) {
                co_await Yield{}; // ← suspend again; resume next tick
            } else {
                std::printf("[Client %d] send() error %d.\n", client_id, WSAGetLastError());
                break;
            }
        }
    }

    (void)closesocket(client_socket);
    std::printf("[Client %d] Done. Sent %zu/%zu bytes.\n", client_id, total_sent, to_send);
}
// ═════════════════════════════════════════════════════════════════════════════
//  5. MAIN EVENT LOOP
//
//  This is a classic single-threaded event loop (like Node.js's, but manual).
//  Each iteration:
//    1. Accept any new incoming connections → spawn a coroutine Task for each.
//    2. Tick every active Task forward by one step (resume it once).
//    3. Sleep 1 ms to avoid burning 100% CPU while idle.
//
//  Because each coroutine yields voluntarily when it would block, no single
//  connection can starve the others. This is "cooperative multitasking".
// ═════════════════════════════════════════════════════════════════════════════
int main() {
    // ── Winsock init ─────────────────────────────────────────────────────────
    WSADATA wsaData;
    if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0) {
        std::printf("[ERROR] WSAStartup failed.\n");
        return 1;
    }

    // ── Create listening socket ───────────────────────────────────────────────
    SOCKET listen_sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (listen_sock == INVALID_SOCKET) {
        std::printf("[ERROR] socket() failed: %d\n", WSAGetLastError());
        (void)WSACleanup();
        return 1;
    }

    // Allow address reuse so we can restart immediately after Ctrl+C
    int opt = 1;
    (void)setsockopt(listen_sock, SOL_SOCKET, SO_REUSEADDR,
                     reinterpret_cast<const char*>(&opt), sizeof(opt));

    // Make the listening socket non-blocking so accept() never stalls
    u_long mode = 1;
    if (ioctlsocket(listen_sock, FIONBIO, &mode) != 0) {
        std::printf("[ERROR] ioctlsocket(listen) failed: %d\n", WSAGetLastError());
        (void)closesocket(listen_sock);
        (void)WSACleanup();
        return 1;
    }

    sockaddr_in server_addr{};
    server_addr.sin_family      = AF_INET;
    server_addr.sin_addr.s_addr = INADDR_ANY;
    server_addr.sin_port        = htons(8080);

    if (bind(listen_sock, reinterpret_cast<sockaddr*>(&server_addr),
             sizeof(server_addr)) == SOCKET_ERROR) {
        std::printf("[ERROR] bind() failed: %d\n", WSAGetLastError());
        (void)closesocket(listen_sock);
        (void)WSACleanup();
        return 1;
    }

    if (listen(listen_sock, SOMAXCONN) == SOCKET_ERROR) {
        std::printf("[ERROR] listen() failed: %d\n", WSAGetLastError());
        (void)closesocket(listen_sock);
        (void)WSACleanup();
        return 1;
    }

    std::printf("══════════════════════════════════════════════\n");
    std::printf("  Coroutine Web Server on http://localhost:8080\n");
    std::printf("  Max concurrent connections: %zu\n", MAX_TASKS);
    std::printf("  Pool frame size: %zu bytes\n", FRAME_SIZE);
    std::printf("  Total static pool: %zu bytes (%.1f KB)\n",
                MAX_TASKS * FRAME_SIZE,
                static_cast<double>(MAX_TASKS * FRAME_SIZE) / 1024.0);
    std::printf("  Press Ctrl+C to exit.\n");
    std::printf("══════════════════════════════════════════════\n\n");

    // ── The Task array: our entire "thread pool" (but no threads!) ────────────
    Task active_tasks[MAX_TASKS]{};
    int  client_id_counter = 0;

    // ── Event loop ────────────────────────────────────────────────────────────
    while (true) {

        // Step 1: Accept new connections (non-blocking — returns immediately
        // with INVALID_SOCKET if nobody is waiting)
        SOCKET client_sock = accept(listen_sock, nullptr, nullptr);
        if (client_sock != INVALID_SOCKET) {
            // Make the client socket non-blocking too
            u_long client_mode = 1;
            if (ioctlsocket(client_sock, FIONBIO, &client_mode) == 0) {
                bool accepted = false;
                for (Task& task : active_tasks) {
                    if (!task.is_valid()) {
                        ++client_id_counter;
                        std::printf("[Client %d] Connected.\n", client_id_counter);
                        // Spawn the coroutine. It suspends immediately at
                        // initial_suspend — we don't call resume() yet.
                        task = handle_client(client_sock, client_id_counter);
                        accepted = true;
                        break;
                    }
                }
                if (!accepted) {
                    std::printf("[Server] Busy! Dropping connection.\n");
                    (void)closesocket(client_sock);
                }
            } else {
                (void)closesocket(client_sock);
            }
        }

        // Step 2: Tick every live coroutine forward by one step.
        // Each resume() runs the coroutine until its next co_await or co_return.
        for (Task& task : active_tasks) {
            if (task.is_valid()) {
                if (!task.resume()) {
                    // Coroutine is done (co_return reached). Release the slot.
                    task = Task{};
                }
            }
        }

        Sleep(1); // ~1 ms sleep to avoid hitting 100% CPU while idle
    }

    // Unreachable in normal operation, but satisfies compiler warning paths.
    (void)closesocket(listen_sock);
    (void)WSACleanup();
    return 0;
}