add_executable(coro_http_tests
  test_task.cpp
  test_event_loop.cpp
  test_thread_pool.cpp
  test_parser.cpp
  test_router.cpp
  test_server.cpp
)

target_link_libraries(coro_http_tests PRIVATE coro_http Catch2::Catch2WithMain)
target_compile_options(coro_http_tests PRIVATE -Wall -Wextra -Wpedantic)

include(Catch)
catch_discover_tests(coro_http_tests)
