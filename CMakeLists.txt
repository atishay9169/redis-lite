cmake_minimum_required(VERSION 3.16)
project(redis-lite CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Default to an optimized build. Without this, CMake's default build type
# is empty -> no optimization flags at all (-O0), which is fine for
# debugging/sanitizer runs but NOT what you want to benchmark -- always
# measure a Release build, never a debug one.
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()
set(CMAKE_CXX_FLAGS_RELEASE "-O2 -DNDEBUG")

add_executable(redis-lite
    src/main.cpp
    src/EventLoop.cpp
    src/RespParser.cpp
    src/Commands.cpp
)

target_compile_options(redis-lite PRIVATE -Wall -Wextra -Wpedantic)

enable_testing()

# CMAKE_BUILD_TYPE=Release (set above) adds -DNDEBUG globally, which turns
# every assert() into a no-op. That's fine for the main binary, but our
# test executables use assert() AS their verification mechanism -- with
# NDEBUG defined, both test_resp and test_ttl would compile, run, print
# "all tests passed," and exit 0 even if every check inside them failed.
# -UNDEBUG explicitly undefines it again for these two targets only, so
# assertions stay live regardless of build type.
add_executable(test_resp tests/test_resp.cpp src/RespParser.cpp)
target_compile_options(test_resp PRIVATE -Wall -Wextra -UNDEBUG
    -fsanitize=address,undefined -fno-omit-frame-pointer -g)
target_link_options(test_resp PRIVATE -fsanitize=address,undefined)
add_test(NAME resp_parser COMMAND test_resp)

add_executable(test_ttl tests/test_ttl.cpp)
target_compile_options(test_ttl PRIVATE -Wall -Wextra -UNDEBUG
    -fsanitize=address,undefined -fno-omit-frame-pointer -g)
target_link_options(test_ttl PRIVATE -fsanitize=address,undefined)
add_test(NAME store_ttl COMMAND test_ttl)

# cmake -B build -DSANITIZE=ON && cmake --build build
option(SANITIZE "Build with AddressSanitizer + UBSan" OFF)
if(SANITIZE)
    target_compile_options(redis-lite PRIVATE
        -fsanitize=address,undefined -fno-omit-frame-pointer -g)
    target_link_options(redis-lite PRIVATE
        -fsanitize=address,undefined)
endif()