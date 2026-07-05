cmake_minimum_required(VERSION 3.16)
project(redis-lite CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

enable_testing()

add_executable(redis-lite
    src/main.cpp
    src/EventLoop.cpp
    src/RespParser.cpp
    src/Commands.cpp
)

add_executable(test_resp
    tests/test_resp.cpp
    src/RespParser.cpp
)

target_compile_options(redis-lite PRIVATE -Wall -Wextra -Wpedantic)
target_compile_options(test_resp PRIVATE -Wall -Wextra -fsanitize=address,undefined -g)
target_link_options(test_resp PRIVATE -fsanitize=address,undefined)

add_test(NAME resp_parser COMMAND test_resp)

# cmake -B build -DSANITIZE=ON && cmake --build build
option(SANITIZE "Build with AddressSanitizer + UBSan" OFF)
if(SANITIZE)
    target_compile_options(redis-lite PRIVATE
        -fsanitize=address,undefined -fno-omit-frame-pointer -g)
    target_link_options(redis-lite PRIVATE
        -fsanitize=address,undefined)
endif()
