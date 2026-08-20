#include "internal.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <stop_token>
#include <string>
#include <utility>
#include <vector>

#include <google/protobuf/timestamp.pb.h>
#include <google/protobuf/util/time_util.h>
#include <oll/plugin.pb.h>

#include <onelastleaf/plugin_sdk.hpp>

namespace onelastleaf {

namespace {

google::protobuf::Timestamp current_timestamp() {
  const auto nanoseconds = std::chrono::duration_cast<std::chrono::nanoseconds>(
      std::chrono::system_clock::now().time_since_epoch());
  return google::protobuf::util::TimeUtil::NanosecondsToTimestamp(
      nanoseconds.count());
}

} // namespace

ActionResult ActionResult::string(std::string value) {
  ActionResult result;
  result.result.emplace().set_string_value(std::move(value));
  return result;
}

ActionContext::ActionContext(
    std::string job_id, oll::protocol::TraceContext trace,
    std::optional<google::protobuf::Timestamp> deadline,
    std::stop_token cancellation, std::uint64_t parent_call_id, Host host)
    : job_id_(std::move(job_id)), trace_(std::move(trace)),
      deadline_(std::move(deadline)), cancellation_(cancellation),
      parent_call_id_(parent_call_id), host_(std::move(host)) {}

oll::protocol::TraceContext ActionContext::nested_trace() const {
  auto nested = trace_;
  nested.set_parent_call_id(parent_call_id_);
  if (nested.call_depth() == std::numeric_limits<std::uint32_t>::max()) {
    throw std::runtime_error("host-call depth overflowed");
  }
  nested.set_call_depth(nested.call_depth() + 1);
  if (nested.call_depth() > host_.maximum_call_depth()) {
    throw std::runtime_error(
        "host call exceeds the negotiated call-depth limit");
  }
  return nested;
}

oll::protocol::HostCallResponse
ActionContext::host_call(oll::protocol::HostCallRequest request) const {
  return host_.call(nested_trace(), std::move(request));
}

oll::protocol::GetConfigResponse
ActionContext::get_config(std::optional<oll::protocol::ConfigPath> path) const {
  return host_.get_config(nested_trace(), std::move(path));
}

oll::protocol::InvokeConfigFunctionResponse
ActionContext::invoke_config_function(
    oll::protocol::ConfigFunctionRef function,
    std::vector<oll::protocol::ConfigValue> arguments) const {
  return host_.invoke_config_function(nested_trace(), std::move(function),
                                      std::move(arguments));
}

Sender::Sender(Writer writer) : writer_(std::move(writer)) {
  if (!writer_) {
    throw std::invalid_argument("plugin envelope writer is required");
  }
}

void Sender::identity(std::string session_id, std::string instance_id) {
  std::lock_guard lock{write_};
  session_id_ = std::move(session_id);
  instance_id_ = std::move(instance_id);
}

std::uint64_t
Sender::send(std::optional<std::uint64_t> reply_to,
             const oll::protocol::TraceContext &trace,
             oll::protocol::PluginEnvelope envelope,
             const std::function<void(std::uint64_t)> &before_write) {
  std::lock_guard lock{write_};
  if (!writer_) {
    throw std::runtime_error("plugin session is closed");
  }
  if (next_id_ == 0 || next_id_ == std::numeric_limits<std::uint64_t>::max()) {
    throw std::runtime_error("plugin exhausted message IDs");
  }
  const auto id = next_id_++;
  if (before_write) {
    before_write(id);
  }
  envelope.set_message_id(id);
  if (reply_to) {
    envelope.set_reply_to(*reply_to);
  }
  envelope.set_session_id(session_id_);
  envelope.set_plugin_instance_id(instance_id_);
  *envelope.mutable_trace() = trace;
  if (!writer_(envelope)) {
    throw std::runtime_error("plugin output stream closed");
  }
  return id;
}

void Sender::close() noexcept {
  std::lock_guard lock{write_};
  writer_ = {};
}

Host::Host(std::shared_ptr<Impl> impl, std::stop_token cancellation)
    : impl_(std::move(impl)), cancellation_(cancellation) {
  if (!impl_) {
    throw std::invalid_argument("host session is required");
  }
}

oll::protocol::HostCallResponse
Host::call(const oll::protocol::TraceContext &trace,
           oll::protocol::HostCallRequest request) const {
  oll::protocol::PluginEnvelope envelope;
  *envelope.mutable_host_call() = std::move(request);
  auto response = impl_->request(trace, std::move(envelope), cancellation_);
  if (response.payload_case() ==
      oll::protocol::PluginEnvelope::kProtocolError) {
    throw std::runtime_error("host rejected request: " +
                             response.protocol_error().message());
  }
  if (response.payload_case() != oll::protocol::PluginEnvelope::kHostResult) {
    throw std::runtime_error("host call received another response kind");
  }
  if (response.host_result().result_case() ==
      oll::protocol::HostCallResponse::kError) {
    throw std::runtime_error("host rejected request: " +
                             response.host_result().error().message());
  }
  return response.host_result();
}

oll::protocol::InvokeConfigFunctionResponse Host::invoke_config_function(
    const oll::protocol::TraceContext &trace,
    oll::protocol::ConfigFunctionRef function,
    std::vector<oll::protocol::ConfigValue> arguments) const {
  oll::protocol::HostCallRequest request;
  auto *invocation = request.mutable_invoke_config_function();
  *invocation->mutable_function() = std::move(function);
  for (auto &argument : arguments) {
    *invocation->add_arguments() = std::move(argument);
  }
  auto response = call(trace, std::move(request));
  if (response.result_case() !=
      oll::protocol::HostCallResponse::kInvokeConfigFunction) {
    throw std::runtime_error(
        "InvokeConfigFunction received another response kind");
  }
  return response.invoke_config_function();
}

oll::protocol::GetConfigResponse
Host::get_config(const oll::protocol::TraceContext &trace,
                 std::optional<oll::protocol::ConfigPath> path) const {
  oll::protocol::HostCallRequest request;
  auto *get_config = request.mutable_get_config();
  if (path) {
    *get_config->mutable_path() = std::move(*path);
  }
  auto response = call(trace, std::move(request));
  if (response.result_case() != oll::protocol::HostCallResponse::kGetConfig) {
    throw std::runtime_error("GetConfig received another response kind");
  }
  return response.get_config();
}

void Host::log(const oll::protocol::TraceContext &trace,
               oll::protocol::LogLevel level, std::string target,
               std::string message,
               std::map<std::string, oll::protocol::ConfigValue> fields) const {
  oll::protocol::PluginEnvelope envelope;
  auto *log = envelope.mutable_log();
  *log->mutable_timestamp() = current_timestamp();
  log->set_level(level);
  log->set_target(std::move(target));
  log->set_message(std::move(message));
  for (auto &[key, value] : fields) {
    (*log->mutable_fields())[key] = std::move(value);
  }
  impl_->send(trace, std::move(envelope), cancellation_);
}

oll::protocol::ArtifactStored
Host::store_artifact(const oll::protocol::TraceContext &trace,
                     std::string job_id,
                     oll::protocol::ArtifactDescriptor descriptor,
                     std::vector<std::string> chunks) const {
  if (chunks.size() > std::numeric_limits<std::uint32_t>::max()) {
    throw std::invalid_argument("artifact has too many chunks");
  }
  const auto chunk_count = static_cast<std::uint32_t>(chunks.size());
  return impl_->store_artifact(
      trace, std::move(job_id), std::move(descriptor), chunk_count,
      [&chunks](std::uint32_t index) {
        return std::move(chunks.at(static_cast<std::size_t>(index)));
      },
      cancellation_);
}

oll::protocol::ArtifactStored Host::store_artifact(
    const oll::protocol::TraceContext &trace, std::string job_id,
    oll::protocol::ArtifactDescriptor descriptor, std::uint32_t chunk_count,
    ArtifactChunkSource chunk_source) const {
  return impl_->store_artifact(trace, std::move(job_id), std::move(descriptor),
                               chunk_count, std::move(chunk_source),
                               cancellation_);
}

std::uint64_t Host::maximum_artifact_chunk_bytes() const noexcept {
  return impl_ ? impl_->maximum_artifact_chunk_bytes : 0;
}

std::uint32_t Host::maximum_call_depth() const noexcept {
  return impl_ ? impl_->maximum_call_depth : 0;
}

std::uint64_t Host::Impl::send(const oll::protocol::TraceContext &trace,
                               oll::protocol::PluginEnvelope envelope,
                               std::stop_token cancellation) {
  if (cancellation.stop_requested()) {
    throw std::runtime_error("plugin job was cancelled");
  }
  return sender->send(std::nullopt, trace, std::move(envelope),
                      [this, cancellation](auto) {
                        std::lock_guard lock{mutex};
                        if (closed) {
                          throw std::runtime_error(close_reason);
                        }
                        if (cancellation.stop_requested()) {
                          throw std::runtime_error("plugin job was cancelled");
                        }
                      });
}

oll::protocol::PluginEnvelope
Host::Impl::request(const oll::protocol::TraceContext &trace,
                    oll::protocol::PluginEnvelope envelope,
                    std::stop_token cancellation) {
  auto waiter = std::make_shared<Pending>();
  waiter->correlation_id = trace.correlation_id();
  const std::stop_callback cancel_wait{cancellation, [this, waiter] {
                                         std::lock_guard lock{mutex};
                                         waiter->cancelled = true;
                                         waiter->ready.notify_all();
                                       }};
  std::uint64_t id = 0;
  bool keep_abandoned = false;
  try {
    id = sender->send(
        std::nullopt, trace, std::move(envelope),
        [this, &id, &cancellation, &waiter](std::uint64_t message_id) {
          id = message_id;
          std::lock_guard lock{mutex};
          if (closed) {
            throw std::runtime_error(close_reason);
          }
          if (cancellation.stop_requested()) {
            throw std::runtime_error("plugin job was cancelled");
          }
          if (!pending.emplace(message_id, waiter).second) {
            throw std::runtime_error("duplicate pending request ID");
          }
        });
    std::unique_lock lock{mutex};
    waiter->ready.wait(lock, [this, &waiter] {
      return waiter->response.has_value() || waiter->cancelled || closed;
    });
    if (waiter->response) {
      auto response = std::move(*waiter->response);
      pending.erase(id);
      return response;
    }
    if (waiter->cancelled) {
      waiter->abandoned = true;
      keep_abandoned = !closed;
      throw std::runtime_error("plugin job was cancelled");
    }
    throw std::runtime_error(close_reason);
  } catch (...) {
    if (id != 0 && !keep_abandoned) {
      std::lock_guard lock{mutex};
      pending.erase(id);
    }
    throw;
  }
}

void Host::Impl::route(const oll::protocol::PluginEnvelope &envelope) {
  std::lock_guard lock{mutex};
  const auto found = pending.find(envelope.reply_to());
  if (found == pending.end()) {
    throw std::runtime_error("host response names no pending request");
  }
  if (found->second->correlation_id != envelope.trace().correlation_id()) {
    throw std::runtime_error("host response changed correlation context");
  }
  if (found->second->abandoned) {
    pending.erase(found);
    return;
  }
  found->second->response = envelope;
  found->second->ready.notify_one();
}

void Host::Impl::close(std::string reason) noexcept {
  std::lock_guard lock{mutex};
  if (closed) {
    return;
  }
  closed = true;
  if (!reason.empty()) {
    close_reason.swap(reason);
  }
  for (const auto &[_, waiter] : pending) {
    waiter->ready.notify_all();
  }
  pending.clear();
}

} // namespace onelastleaf
