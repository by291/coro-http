#include "net/wakeup_pipe.h"

#include <fcntl.h>
#include <unistd.h>

#include <cerrno>
#include <cstdlib>

namespace coro_http {

namespace {

void set_nonblocking(int fd) {
  int flags = ::fcntl(fd, F_GETFL, 0);
  if (flags < 0) {
    std::abort();
  }
  if (::fcntl(fd, F_SETFL, flags | O_NONBLOCK) != 0) {
    std::abort();
  }
  int fd_flags = ::fcntl(fd, F_GETFD, 0);
  if (fd_flags >= 0) {
    ::fcntl(fd, F_SETFD, fd_flags | FD_CLOEXEC);
  }
}

}  // namespace

wakeup_pipe::wakeup_pipe() {
  int fds[2] = {-1, -1};
  if (::pipe(fds) != 0) {
    std::abort();
  }
  read_fd_ = fds[0];
  write_fd_ = fds[1];
  set_nonblocking(read_fd_);
  set_nonblocking(write_fd_);
}

wakeup_pipe::~wakeup_pipe() {
  if (read_fd_ >= 0) {
    ::close(read_fd_);
  }
  if (write_fd_ >= 0) {
    ::close(write_fd_);
  }
}

void wakeup_pipe::signal() noexcept {
  const char byte = 1;
  ssize_t written;
  do {
    written = ::write(write_fd_, &byte, 1);
  } while (written < 0 && errno == EINTR);
  // EAGAIN means a byte is already queued, which is exactly what we want.
}

void wakeup_pipe::drain() noexcept {
  char buffer[64];
  for (;;) {
    ssize_t n = ::read(read_fd_, buffer, sizeof(buffer));
    if (n > 0) {
      continue;
    }
    if (n < 0 && errno == EINTR) {
      continue;
    }
    return;  // EAGAIN or a closed pipe: nothing left to drain
  }
}

}  // namespace coro_http
