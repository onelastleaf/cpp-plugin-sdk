#include "session.hpp"

#include "internal.hpp"
#include "parent_liveness.hpp"
#include "validation.hpp"

#include <cstdint>
#include <cstdlib>
#include <exception>
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
#include <grpcpp/grpcpp.h>
#include <oll/plugin.grpc.pb.h>

#include <onelastleaf/plugin_sdk.hpp>

namespace onelastleaf::detail {

grpc::ChannelArguments plugin_channel_arguments() {
  grpc::ChannelArguments arguments;
  // gRPC defines -1 as unlimited. Set both directions explicitly so receive
  // does not retain gRPC's smaller default.
  constexpr int unlimited_message_size = -1;
  arguments.SetMaxReceiveMessageSize(unlimited_message_size);
  arguments.SetMaxSendMessageSize(unlimited_message_size);
  return arguments;
}

PluginSession::PluginSession(
    std::string plugin_id, std::string plugin_version,
    const std::map<std::string, RegisteredAction> &actions)
    : plugin_id_(std::move(plugin_id)),
      plugin_version_(std::move(plugin_version)), actions_(actions),
      jobs_([this](std::exception_ptr failure) { record_failure(failure); }) {}

PluginSession::~PluginSession() { cleanup("plugin session ended", true); }

int PluginSession::run() {
  connect();
  handshake();
  return read_messages();
}

void PluginSession::connect() {
  auto channel_arguments = plugin_channel_arguments();
  auto channel = grpc::CreateCustomChannel(
      endpoint_target(std::getenv("OLL_PLUGIN_ENDPOINT")),
      grpc::InsecureChannelCredentials(), channel_arguments);
  auto stub = oll::protocol::PluginRuntime::NewStub(channel);
  client_context_ = std::make_shared<grpc::ClientContext>();
  stream_ = stub->Connect(client_context_.get());
  if (!stream_) {
    throw std::runtime_error("could not open plugin protocol stream");
  }
  sender_ = std::make_shared<Sender>(stream_.get());
  host_impl_ = HostAccess::make_impl();
  host_impl_->sender = sender_;
  parent_liveness_ = std::make_unique<ParentLivenessWatcher>(
      [context = client_context_] { context->TryCancel(); });
}

void PluginSession::handshake() {
  Envelope first;
  if (!stream_->Read(&first)) {
    throw std::runtime_error("host closed before HostHello");
  }
  validate_envelope(first, last_host_id_);
  if (first.has_reply_to() || first.payload_case() != Envelope::kHostHello) {
    throw std::runtime_error("HostHello must be the first host message");
  }
  if (first.session_id().empty() || first.plugin_instance_id().empty()) {
    throw std::runtime_error(
        "HostHello envelope omitted its session or instance identity");
  }
  const auto &hello = first.host_hello();
  if (!hello.has_node() || hello.plugin_id().value() != plugin_id_ ||
      hello.plugin_name().value().empty() || hello.maximum_call_depth() == 0 ||
      hello.maximum_causal_depth() == 0 ||
      hello.maximum_artifact_chunk_bytes() == 0) {
    throw std::runtime_error(
        "HostHello does not describe the expected plugin instance");
  }
  if (first.trace().call_depth() > hello.maximum_call_depth() ||
      first.trace().causal_depth() > hello.maximum_causal_depth()) {
    throw std::runtime_error(
        "HostHello exceeds a negotiated trace depth limit");
  }

  sender_->identity(first.session_id(), first.plugin_instance_id());
  host_impl_->maximum_artifact_chunk_bytes =
      hello.maximum_artifact_chunk_bytes();
  host_impl_->maximum_call_depth = hello.maximum_call_depth();
  host_impl_->maximum_causal_depth = hello.maximum_causal_depth();

  Envelope plugin_hello;
  auto *payload = plugin_hello.mutable_plugin_hello();
  payload->mutable_plugin_id()->set_value(plugin_id_);
  *payload->mutable_plugin_name() = hello.plugin_name();
  payload->set_plugin_version(plugin_version_);
  for (const auto &[name, action] : actions_) {
    auto *descriptor = payload->add_actions();
    descriptor->set_name(name);
    descriptor->set_description(action.description);
  }
  sender_->send(std::nullopt, first.trace(), std::move(plugin_hello));

  Envelope ready;
  if (!stream_->Read(&ready)) {
    throw std::runtime_error("host closed before SessionReady");
  }
  validate_envelope(ready, last_host_id_, sender_->session_id(),
                    sender_->instance_id(), host_impl_->maximum_call_depth,
                    host_impl_->maximum_causal_depth);
  if (ready.has_reply_to() || ready.payload_case() != Envelope::kReady ||
      ready.trace().correlation_id() != first.trace().correlation_id()) {
    throw std::runtime_error("host SessionReady must follow PluginHello");
  }
  Envelope plugin_ready;
  plugin_ready.mutable_ready();
  sender_->send(std::nullopt, first.trace(), std::move(plugin_ready));
}

int PluginSession::read_messages() {
  Envelope envelope;
  while (stream_->Read(&envelope)) {
    validate_envelope(envelope, last_host_id_, sender_->session_id(),
                      sender_->instance_id(), host_impl_->maximum_call_depth,
                      host_impl_->maximum_causal_depth);
    if (envelope.has_reply_to()) {
      host_impl_->route(envelope);
      continue;
    }
    switch (envelope.payload_case()) {
    case Envelope::kStartJob:
      start_job(envelope);
      break;
    case Envelope::kCancelJob:
      cancel_job(envelope);
      break;
    case Envelope::kHeartbeat: {
      Envelope heartbeat;
      *heartbeat.mutable_heartbeat() = envelope.heartbeat();
      sender_->send(envelope.message_id(), envelope.trace(),
                    std::move(heartbeat));
      break;
    }
    case Envelope::kShutdown:
      return shutdown(envelope);
    case Envelope::kProtocolError:
      throw std::runtime_error("host protocol error: " +
                               envelope.protocol_error().message());
    default:
      throw std::runtime_error("unexpected host-initiated message");
    }
  }

  cleanup("host protocol stream closed", false);
  if (const auto asynchronous_failure = failure()) {
    std::rethrow_exception(asynchronous_failure);
  }
  return 1;
}

void PluginSession::start_job(const Envelope &envelope) {
  const auto &request = envelope.start_job();
  auto job_id = request.job_id().value();
  if (!canonical_uuid_v4(job_id) || jobs_.contains(job_id) ||
      request.invocation_case() != oll::protocol::StartJobRequest::kAction ||
      !actions_.contains(request.action().action())) {
    throw std::runtime_error("invalid StartJobRequest");
  }

  Envelope accepted;
  *accepted.mutable_job_accepted()->mutable_job_id() = request.job_id();
  sender_->send(envelope.message_id(), envelope.trace(), std::move(accepted));

  auto trace = envelope.trace();
  auto arguments = std::vector<std::string>{
      request.action().arguments().begin(), request.action().arguments().end()};
  auto handler = actions_.at(request.action().action()).handler;
  auto job_id_message = request.job_id();
  const auto parent_call_id = envelope.message_id();
  auto deadline =
      request.has_deadline() ? std::optional{request.deadline()} : std::nullopt;
  jobs_.start(job_id,
              [this, handler, job_id, job_id_message, trace, arguments,
               deadline, parent_call_id](std::stop_token cancellation) mutable {
                execute_action(std::move(handler), std::move(job_id),
                               std::move(job_id_message), std::move(trace),
                               std::move(arguments), std::move(deadline),
                               parent_call_id, cancellation);
              });
}

void PluginSession::cancel_job(const Envelope &envelope) {
  const auto &job_id = envelope.cancel_job().job_id().value();
  if (!jobs_.request_stop(job_id)) {
    throw std::runtime_error("cancellation names no active job");
  }
  Envelope acknowledged;
  *acknowledged.mutable_cancel_job_acknowledged()->mutable_job_id() =
      envelope.cancel_job().job_id();
  sender_->send(envelope.message_id(), envelope.trace(),
                std::move(acknowledged));
}

int PluginSession::shutdown(const Envelope &envelope) {
  jobs_.request_stop_all();
  host_impl_->close("plugin session is shutting down");
  jobs_.wait();

  Envelope acknowledged;
  acknowledged.mutable_shutdown_acknowledged();
  sender_->send(envelope.message_id(), envelope.trace(),
                std::move(acknowledged));
  parent_liveness_.reset();
  stream_->WritesDone();
  Envelope after_shutdown;
  if (stream_->Read(&after_shutdown)) {
    throw std::runtime_error("host sent a message after ShutdownRequest");
  }
  const auto status = stream_->Finish();
  stream_finished_ = true;
  sender_->close();
  cleaned_ = true;
  return status.ok() ? 0 : 1;
}

void PluginSession::execute_action(
    Plugin::Action handler, std::string job_id,
    oll::protocol::PluginJobId job_id_message,
    oll::protocol::TraceContext trace, std::vector<std::string> arguments,
    std::optional<google::protobuf::Timestamp> deadline,
    std::uint64_t parent_call_id, std::stop_token cancellation) noexcept {
  try {
    auto host = HostAccess::make(host_impl_, cancellation);
    ActionContext action_context{std::move(job_id),   trace,
                                 std::move(deadline), cancellation,
                                 parent_call_id,      std::move(host)};
    try {
      auto result = handler(action_context, arguments);
      if (cancellation.stop_requested()) {
        return;
      }
      Envelope update_envelope;
      auto *update = update_envelope.mutable_job_update();
      *update->mutable_job_id() = job_id_message;
      update->set_state(oll::protocol::JOB_STATE_SUCCEEDED);
      update->set_progress(1.0);
      if (result.result) {
        *update->mutable_result() = *result.result;
      }
      for (auto &artifact : result.artifacts) {
        *update->add_artifacts() = std::move(artifact);
      }
      host_impl_->send(trace, std::move(update_envelope), cancellation);
    } catch (const std::exception &error) {
      if (!cancellation.stop_requested()) {
        send_failure(job_id_message, trace, error.what(), cancellation);
      }
    } catch (...) {
      if (!cancellation.stop_requested()) {
        send_failure(job_id_message, trace,
                     "action threw a non-standard exception", cancellation);
      }
    }
  } catch (...) {
    if (!cancellation.stop_requested()) {
      record_failure(std::current_exception());
    }
  }
}

void PluginSession::send_failure(const oll::protocol::PluginJobId &job_id,
                                 const oll::protocol::TraceContext &trace,
                                 std::string message,
                                 std::stop_token cancellation) {
  Envelope update_envelope;
  auto *update = update_envelope.mutable_job_update();
  *update->mutable_job_id() = job_id;
  update->set_state(oll::protocol::JOB_STATE_FAILED);
  update->set_progress(1.0);
  update->mutable_error()->set_code(oll::protocol::ERROR_CODE_INTERNAL);
  update->mutable_error()->set_message(std::move(message));
  host_impl_->send(trace, std::move(update_envelope), cancellation);
}

void PluginSession::record_failure(std::exception_ptr failure) noexcept {
  {
    std::lock_guard lock{failure_mutex_};
    if (!failure_) {
      failure_ = std::move(failure);
    }
  }
  if (client_context_) {
    client_context_->TryCancel();
  }
}

std::exception_ptr PluginSession::failure() const noexcept {
  std::lock_guard lock{failure_mutex_};
  return failure_;
}

void PluginSession::cleanup(std::string reason, bool cancel_rpc) noexcept {
  if (cleaned_) {
    return;
  }
  cleaned_ = true;
  parent_liveness_.reset();
  jobs_.request_stop_all();
  if (host_impl_) {
    host_impl_->close(std::move(reason));
  }
  if (cancel_rpc && client_context_) {
    client_context_->TryCancel();
  }
  if (sender_) {
    sender_->close();
  }
  jobs_.wait();
  if (stream_ && !stream_finished_) {
    static_cast<void>(stream_->Finish());
    stream_finished_ = true;
  }
}

} // namespace onelastleaf::detail
