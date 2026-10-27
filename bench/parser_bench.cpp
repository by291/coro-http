#include <benchmark/benchmark.h>

#include <string>

#include "http/parse_request_head.h"
#include "http/response.h"

namespace {

std::string typical_request() {
  return "GET /hello?x=1 HTTP/1.1\r\n"
         "Host: example.com\r\n"
         "User-Agent: coro-http-bench/1.0\r\n"
         "Accept: */*\r\n"
         "Connection: keep-alive\r\n"
         "\r\n";
}

std::string large_post_head() {
  std::string text = "POST /echo HTTP/1.1\r\nHost: example.com\r\nContent-Length: 65536\r\n";
  for (int i = 0; i < 20; ++i) {
    text += "X-Header-" + std::to_string(i) + ": value\r\n";
  }
  text += "\r\n";
  return text;
}

}  // namespace

static void BM_ParseTypicalRequest(benchmark::State& state) {
  const std::string text = typical_request();
  for (auto _ : state) {
    auto parsed = coro_http::parse_request_head(text);
    benchmark::DoNotOptimize(parsed.consumed);
  }
}
BENCHMARK(BM_ParseTypicalRequest);

static void BM_ParseLargeHead(benchmark::State& state) {
  const std::string text = large_post_head();
  for (auto _ : state) {
    auto parsed = coro_http::parse_request_head(text);
    benchmark::DoNotOptimize(parsed.consumed);
  }
}
BENCHMARK(BM_ParseLargeHead);

static void BM_SerializeResponse(benchmark::State& state) {
  coro_http::response res = coro_http::response::with_text(std::string(1024, 'x'));
  for (auto _ : state) {
    auto wire = coro_http::serialize_response(res, true, false);
    benchmark::DoNotOptimize(wire.data());
  }
}
BENCHMARK(BM_SerializeResponse);

BENCHMARK_MAIN();
