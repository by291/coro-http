add_executable(coro_http_bench parser_bench.cpp)
target_link_libraries(coro_http_bench PRIVATE coro_http benchmark::benchmark)
target_compile_options(coro_http_bench PRIVATE -Wall -Wextra -Wpedantic)
