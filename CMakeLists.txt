cmake_minimum_required(VERSION 4.0)
project(EmbeddedCoroutines)

set(CMAKE_CXX_STANDARD 26)

add_executable(EmbeddedCoroutines main.cpp)

# Add the EmbCoroutines header-only library
target_include_directories(EmbeddedCoroutines PRIVATE ${CMAKE_SOURCE_DIR})

