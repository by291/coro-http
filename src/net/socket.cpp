#include "net/socket.h"

#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <utility>

namespace coro_http {

namespace {

#ifdef MSG_NOSIGNAL
constexpr int kSendFlags = MSG_NOSIGNAL;
#else
constexpr int kSendFlags = 0;
#endif

bool is_would_block(int error) { return error == EAGAIN || error == EWOULDBLOCK; }

std::error_code current_error() { return std::error_code(errno, std::system_category()); }

void configure(int fd) {
  int flags = ::fcntl(fd, F_GETFL, 0);
  if (flags >= 0) {
    ::fcntl(fd, F_SETFL, flags | O_NONBLOCK);
  }
  int fd_flags = ::fcntl(fd, F_GETFD, 0);
  if (fd_flags >= 0) {
    ::fcntl(fd, F_SETFD, fd_flags | FD_CLOEXEC);
  }

  int one = 1;
  ::setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
#ifdef SO_NOSIGPIPE
  ::setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof(one));
#endif
}

}  // namespace

stream_socket::stream_socket(event_loop& loop, int fd) : loop_(&loop), fd_(fd) {
  if (fd_ >= 0) {
    configure(fd_);
  }
}

stream_socket::~stream_socket() { close(); }

stream_socket::stream_socket(stream_socket&& other) noexcept
    : loop_(other.loop_), fd_(std::exchange(other.fd_, -1)) {}

stream_socket& stream_socket::operator=(stream_socket&& other) noexcept {
  if (this != &other) {
    close();
    loop_ = other.loop_;
    fd_ = std::exchange(other.fd_, -1);
  }
  return *this;
}

void stream_socket::shutdown_read() {
  if (fd_ >= 0) {
    ::shutdown(fd_, SHUT_RD);
  }
}

void stream_socket::close() {
  if (fd_ >= 0) {
    loop_->unwatch(fd_);
    ::close(fd_);
    fd_ = -1;
  }
}

task<std::expected<std::size_t, std::error_code>> stream_socket::read_some(
    std::span<char> buffer, event_loop::clock::time_point deadline) {
  for (;;) {
    if (fd_ < 0) {
      co_return std::unexpected(std::make_error_code(std::errc::bad_file_descriptor));
    }

    ssize_t n = ::recv(fd_, buffer.data(), buffer.size(), 0);
    if (n >= 0) {
      co_return static_cast<std::size_t>(n);
    }
    if (errno == EINTR) {
      continue;
    }
    if (!is_would_block(errno)) {
      co_return std::unexpected(current_error());
    }

    bool ready = co_await loop_->readable(fd_, deadline);
    if (!ready) {
      // The deadline expired, but data can land in the same instant: try the
      // syscall once more before reporting a timeout.
      ssize_t last = ::recv(fd_, buffer.data(), buffer.size(), 0);
      if (last >= 0) {
        co_return static_cast<std::size_t>(last);
      }
      if (errno == EINTR) {
        continue;
      }
      if (is_would_block(errno)) {
        co_return std::unexpected(std::make_error_code(std::errc::timed_out));
      }
      co_return std::unexpected(current_error());
    }
  }
}

task<std::expected<void, std::error_code>> stream_socket::write_all(
    std::string_view data, event_loop::clock::time_point deadline) {
  while (!data.empty()) {
    if (fd_ < 0) {
      co_return std::unexpected(std::make_error_code(std::errc::bad_file_descriptor));
    }

    ssize_t n = ::send(fd_, data.data(), data.size(), kSendFlags);
    if (n > 0) {
      data.remove_prefix(static_cast<std::size_t>(n));
      continue;
    }
    if (n < 0 && errno == EINTR) {
      continue;
    }
    if (n < 0 && is_would_block(errno)) {
      bool ready = co_await loop_->writable(fd_, deadline);
      if (!ready) {
        co_return std::unexpected(std::make_error_code(std::errc::timed_out));
      }
      continue;
    }
    co_return std::unexpected(current_error());
  }
  co_return std::expected<void, std::error_code>{};
}

}  // namespace coro_http
