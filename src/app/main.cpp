#include <fcntl.h>
#include <unistd.h>

#include <csignal>
#include <charconv>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <string>
#include <string_view>

#include "app/handlers.h"
#include "http/server.h"
#include "runtime/spawn.h"

namespace {

int g_signal_pipe[2] = {-1, -1};

// The only thing a signal handler is allowed to do here: write one byte.
// Everything else happens back on the event loop thread.
extern "C" void handle_signal(int) {
  const char byte = 1;
  [[maybe_unused]] ssize_t written = ::write(g_signal_pipe[1], &byte, 1);
}

void print_usage(const char* program) {
  std::printf("usage: %s [--host H] [--port N] [--threads N] [--log]\n", program);
}

bool parse_unsigned(std::string_view text, unsigned long& out) {
  auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), out);
  return error == std::errc{} && end == text.data() + text.size();
}

coro_http::task<void> watch_for_signal(coro_http::server& app) {
  co_await app.loop().readable(g_signal_pipe[0]);
  app.request_stop();
  co_return;
}

bool install_signal_pipe() {
  if (::pipe(g_signal_pipe) != 0) {
    return false;
  }
  for (int fd : g_signal_pipe) {
    int flags = ::fcntl(fd, F_GETFL, 0);
    if (flags >= 0) {
      ::fcntl(fd, F_SETFL, flags | O_NONBLOCK);
    }
  }
  struct sigaction action {};
  action.sa_handler = handle_signal;
  sigemptyset(&action.sa_mask);
  action.sa_flags = SA_RESTART;
  ::sigaction(SIGINT, &action, nullptr);
  ::sigaction(SIGTERM, &action, nullptr);
  return true;
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

  if (!install_signal_pipe()) {
    std::fprintf(stderr, "coro-http: could not install signal handling\n");
    return 1;
  }

  try {
    coro_http::server app(config);
    coro_http::register_demo_routes(app);

    std::printf("coro-http on http://%s:%u (blocking workers: %zu)\n", config.host.c_str(),
                app.port(), config.workers);
    std::fflush(stdout);

    coro_http::spawn(app.loop(), watch_for_signal(app));
    app.run();
    std::printf("stopped\n");
  } catch (const std::exception& error) {
    std::fprintf(stderr, "coro-http: %s\n", error.what());
    return 1;
  }
  return 0;
}
