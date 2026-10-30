#include "net/acceptor.h"

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <stdexcept>
#include <utility>

namespace coro_http {

acceptor::acceptor(event_loop& loop, std::string host, std::uint16_t port, int backlog)
    : engine_(loop.engine()) {
  sockaddr_in address{};
  address.sin_family = AF_INET;
  address.sin_port = htons(port);
  if (::inet_pton(AF_INET, host.c_str(), &address.sin_addr) != 1) {
    throw std::runtime_error("acceptor: host is not a valid IPv4 address: " + host);
  }

  fd_ = ::socket(AF_INET, SOCK_STREAM, 0);
  if (fd_ < 0) {
    throw std::runtime_error("acceptor: socket() failed");
  }

  int one = 1;
  ::setsockopt(fd_, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));

  if (::bind(fd_, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0) {
    ::close(fd_);
    fd_ = -1;
    throw std::runtime_error("acceptor: bind() failed");
  }
  if (::listen(fd_, backlog) != 0) {
    ::close(fd_);
    fd_ = -1;
    throw std::runtime_error("acceptor: listen() failed");
  }

  int flags = ::fcntl(fd_, F_GETFL, 0);
  if (flags >= 0) {
    ::fcntl(fd_, F_SETFL, flags | O_NONBLOCK);
  }
  int fd_flags = ::fcntl(fd_, F_GETFD, 0);
  if (fd_flags >= 0) {
    ::fcntl(fd_, F_SETFD, fd_flags | FD_CLOEXEC);
  }

  sockaddr_in bound{};
  socklen_t length = sizeof(bound);
  if (::getsockname(fd_, reinterpret_cast<sockaddr*>(&bound), &length) == 0) {
    port_ = ntohs(bound.sin_port);
  }
}

acceptor::~acceptor() { close(); }

task<std::expected<int, std::error_code>> acceptor::accept() {
  if (fd_ < 0) {
    co_return std::unexpected(std::make_error_code(std::errc::bad_file_descriptor));
  }
  co_return co_await engine_.accept(fd_, io_engine::clock::time_point::max());
}

void acceptor::close() {
  if (fd_ >= 0) {
    engine_.cancel(fd_);
    ::close(fd_);
    fd_ = -1;
  }
}

}  // namespace coro_http
