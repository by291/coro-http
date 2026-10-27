cmake_minimum_required(VERSION 3.25)

# Manifest mode only works when the vcpkg toolchain is in play. Fail loudly
# instead of silently falling back to find_package() against system libraries.
if(NOT DEFINED CMAKE_TOOLCHAIN_FILE AND NOT DEFINED ENV{VCPKG_ROOT})
  message(FATAL_ERROR
    "VCPKG_ROOT is not set and no toolchain file was given.\n"
    "Set VCPKG_ROOT to your vcpkg checkout, or configure with "
    "-DCMAKE_TOOLCHAIN_FILE=<vcpkg>/scripts/buildsystems/vcpkg.cmake")
endif()

project(coro_http
  VERSION 0.1.0
  DESCRIPTION "A coroutine-driven HTTP/1.1 server demo"
  LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(CORO_HTTP_BUILD_BENCH "Build micro-benchmarks" OFF)

find_package(unofficial-picohttpparser CONFIG REQUIRED)

add_library(coro_http STATIC
  src/runtime/thread_pool.cpp
  src/net/event_loop.cpp
  src/net/wakeup_pipe.cpp
  src/net/socket.cpp
  src/net/acceptor.cpp
  src/http/parse_request_head.cpp
  src/http/response.cpp
  src/http/session.cpp
  src/http/server.cpp
  src/app/router.cpp
)

if(APPLE)
  target_sources(coro_http PRIVATE src/net/kqueue_backend.cpp)
elseif(CMAKE_SYSTEM_NAME STREQUAL "Linux")
  target_sources(coro_http PRIVATE src/net/epoll_backend.cpp)
else()
  message(FATAL_ERROR "coro-http supports macOS (kqueue) and Linux (epoll) only")
endif()

target_include_directories(coro_http PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(coro_http PUBLIC unofficial::picohttpparser::picohttpparser)
target_compile_options(coro_http PRIVATE -Wall -Wextra -Wpedantic)

add_executable(coro-http
  src/app/handlers.cpp
  src/app/legacy_sdk.cpp
  src/app/main.cpp
)
target_link_libraries(coro-http PRIVATE coro_http)
target_compile_options(coro-http PRIVATE -Wall -Wextra -Wpedantic)

include(CTest)
if(BUILD_TESTING)
  find_package(Catch2 3 CONFIG REQUIRED)
  enable_testing()
  add_subdirectory(tests)
endif()

if(CORO_HTTP_BUILD_BENCH)
  find_package(benchmark CONFIG REQUIRED)
  add_subdirectory(bench)
endif()
