#include "parent_liveness.hpp"

#include <array>
#include <cerrno>
#include <functional>
#include <stdexcept>
#include <stop_token>
#include <utility>

#ifdef _WIN32
#define NOMINMAX
#include <io.h>
#include <windows.h>
#else
#include <fcntl.h>
#include <poll.h>
#include <unistd.h>
#endif

namespace onelastleaf::detail {

ParentLivenessWatcher::ParentLivenessWatcher(std::function<void()> on_eof,
                                             int input_fd)
    : input_fd_(input_fd), on_eof_(std::move(on_eof)) {
  if (!on_eof_) {
    throw std::invalid_argument("parent-liveness callback is required");
  }
#ifndef _WIN32
  if (::pipe(wake_fds_.data()) != 0) {
    throw std::runtime_error("could not create parent-liveness wake pipe");
  }
  for (const int fd : wake_fds_) {
    const int descriptor_flags = ::fcntl(fd, F_GETFD);
    const int status_flags = ::fcntl(fd, F_GETFL);
    if (descriptor_flags < 0 || status_flags < 0 ||
        ::fcntl(fd, F_SETFD, descriptor_flags | FD_CLOEXEC) < 0 ||
        ::fcntl(fd, F_SETFL, status_flags | O_NONBLOCK) < 0) {
      ::close(wake_fds_[0]);
      ::close(wake_fds_[1]);
      wake_fds_ = {-1, -1};
      throw std::runtime_error("could not configure parent-liveness wake pipe");
    }
  }
#endif
  try {
    thread_ = std::jthread(
        [this](std::stop_token cancellation) { watch(cancellation); });
  } catch (...) {
#ifndef _WIN32
    ::close(wake_fds_[0]);
    ::close(wake_fds_[1]);
    wake_fds_ = {-1, -1};
#endif
    throw;
  }
}

ParentLivenessWatcher::~ParentLivenessWatcher() {
  stop();
#ifndef _WIN32
  if (wake_fds_[0] >= 0) {
    ::close(wake_fds_[0]);
  }
  if (wake_fds_[1] >= 0) {
    ::close(wake_fds_[1]);
  }
#endif
}

void ParentLivenessWatcher::stop() noexcept {
  if (!thread_.joinable()) {
    return;
  }
  thread_.request_stop();
#ifdef _WIN32
  static_cast<void>(::CancelSynchronousIo(thread_.native_handle()));
#endif
  thread_.join();
}

void ParentLivenessWatcher::notify_eof() noexcept {
  try {
    on_eof_();
  } catch (...) {
    // A liveness callback must never escape its owning thread.
    return;
  }
}

void ParentLivenessWatcher::watch(std::stop_token cancellation) noexcept {
  std::array<char, 1024> buffer{};
#ifdef _WIN32
  while (!cancellation.stop_requested()) {
    const int count = ::_read(input_fd_, buffer.data(),
                              static_cast<unsigned int>(buffer.size()));
    if (count > 0) {
      continue;
    }
    if (cancellation.stop_requested()) {
      return;
    }
    if (count < 0 && errno == EINTR) {
      continue;
    }
    notify_eof();
    return;
  }
#else
  const std::stop_callback wake{cancellation, [this] {
                                  constexpr char byte = 1;
                                  while (::write(wake_fds_[1], &byte, 1) < 0 &&
                                         errno == EINTR) {
                                  }
                                }};
  std::array<pollfd, 2> descriptors{{
      {input_fd_, static_cast<short>(POLLIN | POLLHUP), 0},
      {wake_fds_[0], POLLIN, 0},
  }};
  while (!cancellation.stop_requested()) {
    const int ready = ::poll(descriptors.data(), descriptors.size(), -1);
    if (ready < 0) {
      if (errno == EINTR) {
        continue;
      }
      notify_eof();
      return;
    }
    if ((descriptors[1].revents & POLLIN) != 0 ||
        cancellation.stop_requested()) {
      return;
    }
    if ((descriptors[0].revents & (POLLIN | POLLHUP | POLLERR | POLLNVAL)) ==
        0) {
      continue;
    }
    const auto count = ::read(input_fd_, buffer.data(), buffer.size());
    if (count > 0) {
      continue;
    }
    if (count < 0 && (errno == EINTR || errno == EAGAIN)) {
      continue;
    }
    notify_eof();
    return;
  }
#endif
}

} // namespace onelastleaf::detail
