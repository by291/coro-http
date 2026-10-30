#include <pthread.h>

#include <charconv>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <string>
#include <string_view>
#include <thread>

#include "app/handlers.h"
#include "http/server.h"

namespace {

void print_usage(const char* program) {
  std::printf("usage: %s [--host H] [--port N] [--threads N] [--backend auto|readiness|uring] [--log]\n",
              program);
}

bool parse_unsigned(std::string_view text, unsigned long& out) {
  auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), out);
  return error == std::errc{} && end == text.data() + text.size();
}

}  // namespace

int main(int argc, char** argv) {
  coro_http::server_config config;

  for (int i = 1; i < argc; ++i) {
    std::string_view argument = argv[i];
    if (argument == "--host" && i + 1 < argc) {
      config.host = argv[++i];
    } else if (argument == "--port" && i + 1 < argc) {
      unsigned long value = 0;
      if (!parse_unsigned(argv[++i], value) || value > 65535) {
        print_usage(argv[0]);
        return 1;
      }
      config.port = static_cast<std::uint16_t>(value);
    } else if (argument == "--threads" && i + 1 < argc) {
      unsigned long value = 0;
      if (!parse_unsigned(argv[++i], value) || value == 0) {
        print_usage(argv[0]);
        return 1;
      }
      config.workers = static_cast<std::size_t>(value);
    } else if (argument == "--backend" && i + 1 < argc) {
      std::string_view name = argv[++i];
      if (name == "auto") {
        config.backend = coro_http::io_backend::auto_select;
      } else if (name == "readiness") {
        config.backend = coro_http::io_backend::readiness;
      } else if (name == "uring") {
        config.backend = coro_http::io_backend::uring;
      } else {
        std::fprintf(stderr, "unknown backend: %s\n", argv[i]);
        print_usage(argv[0]);
        return 1;
      }
    } else if (argument == "--log") {
      config.access_log = true;
    } else if (argument == "--help" || argument == "-h") {
      print_usage(argv[0]);
      return 0;
    } else {
      print_usage(argv[0]);
      return 1;
    }
  }

  std::signal(SIGPIPE, SIG_IGN);

  // Block the termination signals in every thread (the pool threads inherit
  // the mask) and consume them with sigwait in main. That keeps shutdown
  // independent of the IO backend: no signal handler, no self-pipe, and no
  // readiness awaiter that a completion engine cannot wake.
  sigset_t signals;
  sigemptyset(&signals);
  sigaddset(&signals, SIGINT);
  sigaddset(&signals, SIGTERM);
  if (pthread_sigmask(SIG_BLOCK, &signals, nullptr) != 0) {
    std::fprintf(stderr, "coro-http: could not block signals\n");
    return 1;
  }

  try {
    coro_http::server app(config);
    coro_http::register_demo_routes(app);

    std::printf("coro-http on http://%s:%u (backend: %s, blocking workers: %zu)\n",
                config.host.c_str(), app.port(), app.loop().engine().name(), config.workers);
    std::fflush(stdout);

    std::thread server_thread([&app] { app.run(); });

    int received = 0;
    if (sigwait(&signals, &received) == 0) {
      std::printf("\nreceived signal %d, shutting down\n", received);
      std::fflush(stdout);
    }
    app.request_stop();
    server_thread.join();

    std::printf("stopped\n");
  } catch (const std::exception& error) {
    std::fprintf(stderr, "coro-http: %s\n", error.what());
    return 1;
  }
  return 0;
}
