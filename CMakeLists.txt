cmake_minimum_required(VERSION 3.14)
project(order_book LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

option(ENABLE_SANITIZERS "Build with AddressSanitizer and UBSan (GCC/Clang only)" OFF)

# Warning flags differ per compiler.
function(set_warnings target)
  if(MSVC)
    target_compile_options(${target} PRIVATE /W4)
  else()
    target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic)
  endif()
endfunction()

# Core: matching engine + text protocol (no sockets, fully unit-testable).
add_library(orderbook src/order_book.cpp src/protocol.cpp)
target_include_directories(orderbook PUBLIC include)
set_warnings(orderbook)

add_executable(demo src/main.cpp)
target_link_libraries(demo PRIVATE orderbook)

add_executable(order_book_tests tests/test_order_book.cpp)
target_link_libraries(order_book_tests PRIVATE orderbook)
set_warnings(order_book_tests)

add_executable(protocol_tests tests/test_protocol.cpp)
target_link_libraries(protocol_tests PRIVATE orderbook)
set_warnings(protocol_tests)

add_executable(bench_book bench/bench_book.cpp)
target_link_libraries(bench_book PRIVATE orderbook)
set_warnings(bench_book)

set(ALL_TARGETS orderbook demo order_book_tests protocol_tests bench_book)

# server/client use POSIX sockets (Linux/macOS/WSL), so they are skipped on Windows.
if(UNIX)
  add_executable(server src/server.cpp)
  target_link_libraries(server PRIVATE orderbook)
  set_warnings(server)

  add_executable(client src/client.cpp)
  target_link_libraries(client PRIVATE orderbook)
  set_warnings(client)

  list(APPEND ALL_TARGETS server client)
endif()

if(ENABLE_SANITIZERS AND NOT MSVC)
  foreach(t ${ALL_TARGETS})
    target_compile_options(${t} PRIVATE -fsanitize=address,undefined -fno-omit-frame-pointer)
    target_link_options(${t} PRIVATE -fsanitize=address,undefined)
  endforeach()
endif()

enable_testing()
add_test(NAME order_book_tests COMMAND order_book_tests)
add_test(NAME protocol_tests COMMAND protocol_tests)