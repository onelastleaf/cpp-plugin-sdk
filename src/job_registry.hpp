#pragma once

#include <condition_variable>
#include <exception>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <stop_token>
#include <string>
#include <string_view>
#include <thread>

namespace onelastleaf::detail {

class JobRegistry final {
public:
  using Task = std::function<void(std::stop_token)>;
  using FailureHandler = std::function<void(std::exception_ptr)>;

  explicit JobRegistry(FailureHandler failure_handler);
  ~JobRegistry();

  JobRegistry(const JobRegistry &) = delete;
  JobRegistry &operator=(const JobRegistry &) = delete;
  JobRegistry(JobRegistry &&) = delete;
  JobRegistry &operator=(JobRegistry &&) = delete;

  void start(std::string job_id, Task task);
  bool contains(std::string_view job_id) const;
  bool request_stop(std::string_view job_id);
  void request_stop_all();
  void wait();
  std::size_t size() const;

private:
  struct ActiveJob {
    std::mutex lifecycle;
    std::jthread task;
    bool finished{};
    bool reaping{};
  };

  void mark_finished(const std::string &job_id) noexcept;
  void reap(std::stop_token cancellation) noexcept;

  FailureHandler failure_handler_;
  mutable std::mutex mutex_;
  std::condition_variable_any changed_;
  std::map<std::string, std::shared_ptr<ActiveJob>, std::less<>> jobs_;
  std::jthread reaper_;
};

} // namespace onelastleaf::detail
