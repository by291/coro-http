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

stream_socket::stream_socket(io_engine& engine, int fd) : engine_(&engine), fd_(fd) {
  if (fd_ >= 0) {
    configure(fd_);
  }
}

stream_socket::~stream_socket() { close(); }

stream_socket::stream_socket(stream_socket&& other) noexcept
    : engine_(other.engine_), fd_(std::exchange(other.fd_, -1)) {}

stream_socket& stream_socket::operator=(stream_socket&& other) noexcept {
  if (this != &other) {
    close();
    engine_ = other.engine_;
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
    // Wake whoever is parked on this fd before it disappears; their next
    // syscall/submission then fails with EBADF instead of hanging.
    engine_->cancel(fd_);
    ::close(fd_);
    fd_ = -1;
  }
}

task<std::expected<std::size_t, std::error_code>> stream_socket::read_some(
    std::span<char> buffer, io_engine::clock::time_point deadline) {
  if (fd_ < 0) {
    co_return std::unexpected(std::make_error_code(std::errc::bad_file_descriptor));
  }
  co_return co_await engine_->read_some(fd_, buffer, deadline);
}

task<std::expected<void, std::error_code>> stream_socket::write_all(
    std::string_view data, io_engine::clock::time_point deadline) {
  if (fd_ < 0) {
    co_return std::unexpected(std::make_error_code(std::errc::bad_file_descriptor));
  }
  co_return co_await engine_->write_all(fd_, data, deadline);
}

}  // namespace coro_http
