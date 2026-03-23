#import "@preview/codly:1.3.0": *
#import "@preview/codly-languages:0.1.1": *
#show: codly-init.with()
#set text(
  font: "JetBrainsMono NF",
  size: 14pt
)
#set math.equation(numbering: "(1)")
#set page(numbering: "1")
#set heading(numbering: "1.")
#codly(languages: codly-languages)


= Finding out How C++ Coroutines Work

C++ Coroutines are often criticized in embedded circles because, by default they allocate their "state" (the coroutine frame) on the heap. However, the standard was written with hooks to override this feature to allow static allocation.

```cpp
Task my_coroutine() {
    int x = 10;
    co_await some_event();
    x++;
    printf("%d", x);
}
    
```

When i am writing this code the compiler performs a Lowering pass. It converts that function into a c++ class which is a coroutine frame. The compiler assumes that this frames needs to be created dynamically because the coroutines might outlive the function that called it. Therefore it inserts a call to operator new to create the object.The Solution We need to provide a custom promise_type that contains an overload for operator new.

= Implementation Strategy

We cant easily determine the exact size of the frame at the time we write the code (in dynamic allocation). However in embedded systems we can solve it in 2 ways

== Placement New 

- Instead of the default we overwrite it with a static buffer and tell the coroutine to live there.
== Heap Elision
- Compiler based , not robust enough

= The Main Infrastructure

We Need a return type for our coroutine. Lets call it staticTask , this class will house the promise_type which handles the memory

```cpp
#include<coroutine>
#include<cstddef>
#include<cstdio>
#include<new> // For std::align_val_t

alignas(std::max_align_t) static uint8_t g_coroutine_buffer[1024];
static bool g_buffer_used = false;

struct StaticTask {
    // The Compiler looks for 'promise_type' inside the return type
    struct promise_type {

        // 1. INITIALIZATION
        StaticTask get_return_object() {
            return StaticTask{std::coroutine_handle<promise_type>::from_promise(*this)};
        }
        std::suspend_never initial_suspend() { return {}; } // Start immediately? Yes.
        std::suspend_always final_suspend() noexcept { return {}; } // Don't destroy automatically

        void return_void() {}
        void unhandled_exception() { /* Trap error here */ }

        // 
        // THE MAGIC: Custom Allocator
        // 
        // When the compiler wants to create the Frame, it calls this.
        void* operator new(std::size_t size) {
            if (size > sizeof(g_coroutine_buffer)) {
                // Error: Static buffer too small!
                // In embedded, trigger a blinking LED or assert.
                return nullptr;
            }
            if (g_buffer_used) {
                // Error: Coroutine re-entrancy not supported in this simple example
                return nullptr;
            }

            g_buffer_used = true;
            printf("[System] Allocating %zu bytes from static buffer\n", size);
            return g_coroutine_buffer;
        }

        void operator delete(void* ptr, std::size_t size) {
            printf("[System] Freeing static buffer\n");
            g_buffer_used = false;
        }
    };

    std::coroutine_handle<promise_type> handle;

    // RAII to clean up
    ~StaticTask() {
        if (handle) handle.destroy();
    }
};


```

= Now Lets Pretend We have to parse some data asynchronously.

We will simulate a data stream coming in one byte at a time like (UART ISR)

```cpp

// Awaitable: Waits for a single byte
struct ByteReader {
    char* value_ptr;

    bool await_ready() { return false; } // Always suspend to wait for data
    void await_suspend(std::coroutine_handle<>) {
        // In a real system, you would register this handle with the UART ISR here.
    }
    char await_resume() { return *value_ptr; }
};

// Global "register" to simulate hardware
char UART_DATA_REGISTER = 0;

// Helper to wait for next byte
auto next_byte() {
    struct Awaiter {
        bool await_ready() { return false; }
        // We pause execution here and return control to main loop
        void await_suspend(std::coroutine_handle<>) {}
        char await_resume() { return UART_DATA_REGISTER; }
    };
    return Awaiter{};
}

StaticTask protocol_parser() {
    printf("  [Co] Parser started.\n");

    while(true) {
        // STATE 1: Wait for Header 0xAA
        char byte = co_await next_byte();

        if (byte != (char)0xAA) {
            printf("  [Co] Invalid Header: %02x, waiting...\n", (unsigned char)byte);
            continue;
        }

        // STATE 2: Read Command
        char cmd = co_await next_byte();

        // STATE 3: Read Data
        char data = co_await next_byte();

        printf("  [Co] PACKET RECEIVED: Cmd: %d, Data: %d\n", cmd, data);
    }
}


```

New We can use it in the main function like:

```cpp
int main() {
    printf("Starting System...\n");

    // 1. Create the coroutine.
    // This triggers 'operator new', allocates from static buffer,
    // runs to the first 'co_await', and returns the handle.
    StaticTask task = protocol_parser();

    // 2. Simulate Incoming Data Stream (0xAA, 0x01, 0x05, 0xFF...)
    char incoming_stream[] = { 0x00, 0xAA, 0x01, 0x50, 0xAA, 0x02, 0x99 };

    for (char b : incoming_stream) {
        printf("\n[HW] Receive IRQ: %02x\n", (unsigned char)b);

        // Load data into "register"
        UART_DATA_REGISTER = b;

        // Resume the coroutine (Tell it data is ready)
        if (!task.handle.done()) {
            task.handle.resume();
        }
    }

    return 0;
}

```