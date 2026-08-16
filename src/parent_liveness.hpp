#pragma once

#include <array>
#include <functional>
#include <stop_token>
#include <thread>

namespace onelastleaf::detail {

class ParentLivenessWatcher final {
public:
  explicit ParentLivenessWatcher(std::function<void()> on_eof,
                                 int input_fd = 0);
  ~ParentLivenessWatcher();

  ParentLivenessWatcher(const ParentLivenessWatcher &) = delete;
  ParentLivenessWatcher &operator=(const ParentLivenessWatcher &) = delete;
  ParentLivenessWatcher(ParentLivenessWatcher &&) = delete;
  ParentLivenessWatcher &operator=(ParentLivenessWatcher &&) = delete;

  void stop() noexcept;

private:
  void watch(std::stop_token cancellation) noexcept;
  void notify_eof() noexcept;

  int input_fd_;
  std::function<void()> on_eof_;
  std::array<int, 2> wake_fds_{-1, -1};
  std::jthread thread_;
};

} // namespace onelastleaf::detail
