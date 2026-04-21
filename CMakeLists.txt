cmake_minimum_required(VERSION 4.0)
project(EmbeddedCoroutines)

set(CMAKE_CXX_STANDARD 26)

add_executable(EmbeddedCoroutines main.cpp
        EmbCoroutines/SimpleCoroutine.hpp)

# Add the EmbCoroutines header-only library
target_include_directories(EmbeddedCoroutines PRIVATE ${CMAKE_SOURCE_DIR})

# #pragma comment(lib, "Ws2_32.lib") only works in MSVC.
# MinGW/GCC (used by CLion's cmake-build) silently ignores it, so the
# winsock symbols are undefined at link time. This line is the real fix.
target_link_libraries(EmbeddedCoroutines PRIVATE ws2_32)

# Enable coroutine support on GCC/Clang if needed
if (CMAKE_CXX_COMPILER_ID STREQUAL "GNU" OR CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
    target_compile_options(EmbeddedCoroutines PRIVATE -fcoroutines)
endif()