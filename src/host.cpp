#include "internal.hpp"

#include <algorithm>
#include <chrono>
#include <limits>
#include <regex>
#include <stdexcept>

#include <google/protobuf/util/time_util.h>
#include <openssl/evp.h>

namespace onelastleaf {
namespace {

bool canonical_uuid_v4(const std::string &value) {
  static const std::regex uuid{
      "^[0-9a-f]{8}-[0-9a-f]{4}-4[0-9a-f]{3}-[89ab][0-9a-f]{3}-[0-9a-f]{12}$"};
  return std::regex_match(value, uuid);
}

} // namespace

ActionResult ActionResult::string(std::string value) {
  ActionResult result;
  result.result.emplace().set_string_value(std::move(value));
  return result;
}

oll::protocol::TraceContext ActionContext::nested_trace() const {
  auto nested = trace_;
  nested.set_parent_call_id(parent_call_id_);
  if (nested.call_depth() == std::numeric_limits<std::uint32_t>::max()) {
    throw std::runtime_error("host-call depth overflowed");
  }
  nested.set_call_depth(nested.call_depth() + 1);
  if (nested.call_depth() > host_->maximum_call_depth()) {
    throw std::runtime_error(
        "host call exceeds the negotiated call-depth limit");
  }
  return nested;
}

oll::protocol::HostCallResponse
ActionContext::host_call(oll::protocol::HostCallRequest request) const {
  return host_->call(nested_trace(), std::move(request));
}

oll::protocol::GetConfigResponse
ActionContext::get_config(std::optional<oll::protocol::ConfigPath> path) const {
  return host_->get_config(nested_trace(), std::move(path));
}

oll::protocol::InvokeConfigFunctionResponse
ActionContext::invoke_config_function(
    oll::protocol::ConfigFunctionRef function,
    std::vector<oll::protocol::ConfigValue> arguments) const {
  return host_->invoke_config_function(nested_trace(), std::move(function),
                                       std::move(arguments));
}

void Sender::identity(std::string session_id, std::string instance_id) {
  session_id_ = std::move(session_id);
  instance_id_ = std::move(instance_id);
}

std::uint64_t Sender::send(std::optional<std::uint64_t> reply_to,
                           const oll::protocol::TraceContext &trace,
                           oll::protocol::PluginEnvelope envelope,
                           const std::function<void(std::uint64_t)> &before_write) {
  std::lock_guard lock{write_};
  if (next_id_ == 0 || next_id_ == std::numeric_limits<std::uint64_t>::max()) {
    throw std::runtime_error("plugin exhausted message IDs");
  }
  const auto id = next_id_++;
  if (before_write)
    before_write(id);
  envelope.set_message_id(id);
  if (reply_to)
    envelope.set_reply_to(*reply_to);
  envelope.set_session_id(session_id_);
  envelope.set_plugin_instance_id(instance_id_);
  *envelope.mutable_trace() = trace;
  if (!stream_->Write(envelope))
    throw std::runtime_error("plugin output stream closed");
  return id;
}

Host::Host(std::shared_ptr<Impl> impl) : impl_(std::move(impl)) {}

oll::protocol::HostCallResponse
Host::call(const oll::protocol::TraceContext &trace,
           oll::protocol::HostCallRequest request) {
  oll::protocol::PluginEnvelope envelope;
  *envelope.mutable_host_call() = std::move(request);
  auto response = impl_->request(trace, std::move(envelope));
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
    std::vector<oll::protocol::ConfigValue> arguments) {
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
                 std::optional<oll::protocol::ConfigPath> path) {
  oll::protocol::HostCallRequest request;
  auto *get_config = request.mutable_get_config();
  if (path)
    *get_config->mutable_path() = std::move(*path);
  auto response = call(trace, std::move(request));
  if (response.result_case() != oll::protocol::HostCallResponse::kGetConfig) {
    throw std::runtime_error("GetConfig received another response kind");
  }
  return response.get_config();
}

void Host::log(const oll::protocol::TraceContext &trace,
               oll::protocol::LogLevel level, std::string target,
               std::string message,
               std::map<std::string, oll::protocol::ConfigValue> fields) {
  oll::protocol::PluginEnvelope envelope;
  auto *log = envelope.mutable_log();
  *log->mutable_timestamp() =
      google::protobuf::util::TimeUtil::GetCurrentTime();
  log->set_level(level);
  log->set_target(std::move(target));
  log->set_message(std::move(message));
  for (auto &[key, value] : fields) {
    (*log->mutable_fields())[key] = std::move(value);
  }
  impl_->sender->send(std::nullopt, trace, std::move(envelope));
}

oll::protocol::ArtifactStored
Host::store_artifact(const oll::protocol::TraceContext &trace,
                     std::string job_id,
                     oll::protocol::ArtifactDescriptor descriptor,
                     std::vector<std::string> chunks) {
  if (!descriptor.has_artifact_id() ||
      !canonical_uuid_v4(descriptor.artifact_id().value()) ||
      descriptor.file_name().empty() || descriptor.media_type().empty() ||
      descriptor.sha256().size() != 32) {
    throw std::invalid_argument("artifact descriptor is invalid");
  }
  if (chunks.empty() || std::ranges::any_of(chunks, [](const auto &chunk) {
        return chunk.empty();
      })) {
    throw std::invalid_argument("artifact chunks must be nonempty");
  }
  if (chunks.size() > std::numeric_limits<std::uint32_t>::max()) {
    throw std::invalid_argument("artifact has too many chunks");
  }
  std::uint64_t size = 0;
  auto digest = std::unique_ptr<EVP_MD_CTX, decltype(&EVP_MD_CTX_free)>{
      EVP_MD_CTX_new(), EVP_MD_CTX_free};
  if (!digest || EVP_DigestInit_ex(digest.get(), EVP_sha256(), nullptr) != 1) {
    throw std::runtime_error("could not initialize artifact SHA-256");
  }
  for (const auto &chunk : chunks) {
    if (chunk.size() > impl_->maximum_artifact_chunk_bytes) {
      throw std::invalid_argument(
          "artifact chunk exceeds the negotiated limit");
    }
    if (chunk.size() > std::numeric_limits<std::uint64_t>::max() - size) {
      throw std::invalid_argument("artifact size overflowed");
    }
    size += static_cast<std::uint64_t>(chunk.size());
    if (EVP_DigestUpdate(digest.get(), chunk.data(), chunk.size()) != 1) {
      throw std::runtime_error("could not hash artifact bytes");
    }
  }
  std::string sha256(EVP_MD_size(EVP_sha256()), '\0');
  unsigned int digest_size = 0;
  if (EVP_DigestFinal_ex(digest.get(),
                         reinterpret_cast<unsigned char *>(sha256.data()),
                         &digest_size) != 1) {
    throw std::runtime_error("could not finish artifact SHA-256");
  }
  sha256.resize(digest_size);
  if (descriptor.size_bytes() != size || descriptor.sha256() != sha256) {
    throw std::invalid_argument(
        "artifact size or SHA-256 does not match its bytes");
  }

  oll::protocol::PluginEnvelope start_envelope;
  auto *start = start_envelope.mutable_artifact_start();
  start->mutable_job_id()->set_value(std::move(job_id));
  *start->mutable_artifact() = descriptor;
  start->set_chunk_count(static_cast<std::uint32_t>(chunks.size()));
  auto accepted = impl_->request(trace, std::move(start_envelope));
  if (accepted.payload_case() !=
          oll::protocol::PluginEnvelope::kArtifactAccepted ||
      accepted.artifact_accepted().artifact_id().value() !=
          descriptor.artifact_id().value()) {
    throw std::runtime_error("host did not accept the artifact transfer");
  }
  for (std::size_t index = 0; index < chunks.size(); ++index) {
    oll::protocol::PluginEnvelope chunk_envelope;
    auto *chunk = chunk_envelope.mutable_artifact_chunk();
    *chunk->mutable_artifact_id() = descriptor.artifact_id();
    chunk->set_chunk_index(static_cast<std::uint32_t>(index));
    chunk->set_data(std::move(chunks[index]));
    impl_->sender->send(std::nullopt, trace, std::move(chunk_envelope));
  }
  oll::protocol::PluginEnvelope complete_envelope;
  *complete_envelope.mutable_artifact_complete()->mutable_artifact_id() =
      descriptor.artifact_id();
  auto stored = impl_->request(trace, std::move(complete_envelope));
  if (stored.payload_case() != oll::protocol::PluginEnvelope::kArtifactStored ||
      stored.artifact_stored().artifact_id().value() !=
          descriptor.artifact_id().value()) {
    throw std::runtime_error("host did not acknowledge the stored artifact");
  }
  return stored.artifact_stored();
}

std::uint64_t Host::maximum_artifact_chunk_bytes() const noexcept {
  return impl_->maximum_artifact_chunk_bytes;
}

std::uint32_t Host::maximum_call_depth() const noexcept {
  return impl_->maximum_call_depth;
}

oll::protocol::PluginEnvelope
Host::Impl::request(const oll::protocol::TraceContext &trace,
                    oll::protocol::PluginEnvelope envelope) {
  auto waiter = std::make_shared<Pending>();
  waiter->correlation_id = trace.correlation_id();
  std::uint64_t id = 0;
  try {
    id = sender->send(
        std::nullopt, trace, std::move(envelope), [&](std::uint64_t message_id) {
          id = message_id;
          std::lock_guard lock{mutex};
          if (!pending.emplace(message_id, waiter).second) {
            throw std::runtime_error("duplicate pending request ID");
          }
        });
    std::unique_lock lock{mutex};
    waiter->ready.wait(lock, [&] { return waiter->response.has_value(); });
    auto response = std::move(*waiter->response);
    pending.erase(id);
    return response;
  } catch (...) {
    if (id != 0) {
      std::lock_guard lock{mutex};
      pending.erase(id);
    }
    throw;
  }
}

void Host::Impl::route(const oll::protocol::PluginEnvelope &envelope) {
  std::lock_guard lock{mutex};
  const auto found = pending.find(envelope.reply_to());
  if (found == pending.end())
    throw std::runtime_error("host response names no pending request");
  if (found->second->correlation_id != envelope.trace().correlation_id()) {
    throw std::runtime_error("host response changed correlation context");
  }
  found->second->response = envelope;
  found->second->ready.notify_one();
}

} // namespace onelastleaf
