add_executable(coro_http_tests
  test_task.cpp
  test_event_loop.cpp
  test_thread_pool.cpp
  test_parser.cpp
  test_router.cpp
  test_server.cpp
)

# GCC 13 reports a false -Wmaybe-uninitialized inside the
# std::optional<std::expected<std::string, error_code>> that blocking_call's
# coroutine promise stores at -O3. The value is always initialised; silence it
# for this translation unit only, leaving the warning on everywhere else.
if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
  set_source_files_properties(test_server.cpp PROPERTIES COMPILE_OPTIONS "-Wno-maybe-uninitialized")
endif()

target_link_libraries(coro_http_tests PRIVATE coro_http Catch2::Catch2WithMain)
target_compile_options(coro_http_tests PRIVATE -Wall -Wextra -Wpedantic)

include(Catch)
catch_discover_tests(coro_http_tests)
