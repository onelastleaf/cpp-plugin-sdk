#include "job_registry.hpp"

#include <cstddef>
#include <exception>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <stop_token>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace onelastleaf::detail {
namespace {

JobRegistry::FailureHandler
require_failure_handler(JobRegistry::FailureHandler handler) {
  if (!handler) {
    throw std::invalid_argument("job failure handler is required");
  }
  return handler;
}

} // namespace

JobRegistry::JobRegistry(FailureHandler failure_handler)
    : failure_handler_(require_failure_handler(std::move(failure_handler))),
      reaper_([this](std::stop_token cancellation) { reap(cancellation); }) {}

JobRegistry::~JobRegistry() {
  request_stop_all();
  wait();
  reaper_.request_stop();
  if (reaper_.joinable()) {
    reaper_.join();
  }
}

void JobRegistry::start(std::string job_id, Task task) {
  if (!task) {
    throw std::invalid_argument("job task is required");
  }
  auto job = std::make_shared<ActiveJob>();
  std::lock_guard lock{mutex_};
  const auto [entry, inserted] = jobs_.emplace(job_id, job);
  if (!inserted) {
    throw std::invalid_argument("job is already active");
  }
  try {
    job->task =
        std::jthread([this, job_id = std::move(job_id), task = std::move(task)](
                         std::stop_token cancellation) mutable noexcept {
          try {
            task(cancellation);
          } catch (...) {
            try {
              failure_handler_(std::current_exception());
            } catch (...) {
              // No exception may escape a worker thread.
              static_cast<void>(std::current_exception());
            }
          }
          mark_finished(job_id);
        });
  } catch (...) {
    jobs_.erase(entry);
    throw;
  }
}

bool JobRegistry::contains(std::string_view job_id) const {
  std::lock_guard lock{mutex_};
  return jobs_.contains(job_id);
}

bool JobRegistry::request_stop(std::string_view job_id) {
  std::shared_ptr<ActiveJob> job;
  {
    std::lock_guard lock{mutex_};
    const auto found = jobs_.find(job_id);
    if (found == jobs_.end()) {
      return false;
    }
    job = found->second;
  }
  std::lock_guard lifecycle_lock{job->lifecycle};
  job->task.request_stop();
  return true;
}

void JobRegistry::request_stop_all() {
  std::vector<std::shared_ptr<ActiveJob>> jobs;
  {
    std::lock_guard lock{mutex_};
    jobs.reserve(jobs_.size());
    for (const auto &[_, job] : jobs_) {
      jobs.push_back(job);
    }
  }
  for (const auto &job : jobs) {
    std::lock_guard lifecycle_lock{job->lifecycle};
    job->task.request_stop();
  }
}

void JobRegistry::wait() {
  std::unique_lock lock{mutex_};
  changed_.wait(lock, [this] { return jobs_.empty(); });
}

std::size_t JobRegistry::size() const {
  std::lock_guard lock{mutex_};
  return jobs_.size();
}

void JobRegistry::mark_finished(const std::string &job_id) noexcept {
  std::lock_guard lock{mutex_};
  const auto found = jobs_.find(job_id);
  if (found != jobs_.end()) {
    found->second->finished = true;
    changed_.notify_all();
  }
}

void JobRegistry::reap(std::stop_token cancellation) noexcept {
  while (true) {
    std::vector<std::pair<std::string, std::shared_ptr<ActiveJob>>> completed;
    {
      std::unique_lock lock{mutex_};
      changed_.wait(lock, cancellation, [this] {
        for (const auto &[_, job] : jobs_) {
          if (job->finished && !job->reaping) {
            return true;
          }
        }
        return false;
      });
      for (auto &[job_id, job] : jobs_) {
        if (job->finished && !job->reaping) {
          job->reaping = true;
          completed.emplace_back(job_id, job);
        }
      }
      if (completed.empty() && cancellation.stop_requested()) {
        return;
      }
    }

    for (const auto &[_, job] : completed) {
      // Mark the jthread token stopped even after a normal return. Host handles
      // copied out of the action then become unusable when that job ends.
      std::lock_guard lifecycle_lock{job->lifecycle};
      job->task.request_stop();
      if (job->task.joinable()) {
        job->task.join();
      }
    }
    {
      std::lock_guard lock{mutex_};
      for (const auto &[job_id, job] : completed) {
        const auto found = jobs_.find(job_id);
        if (found != jobs_.end() && found->second == job) {
          jobs_.erase(found);
        }
      }
      changed_.notify_all();
    }
  }
}

} // namespace onelastleaf::detail
