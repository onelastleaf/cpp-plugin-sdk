#include "internal.hpp"

#include <array>
#include <charconv>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <regex>
#include <thread>

#ifdef _WIN32
#include <io.h>
#else
#include <unistd.h>
#endif

namespace onelastleaf {
namespace {

constexpr std::size_t maximum_envelope_bytes = 64 * 1024 * 1024;

void wait_for_parent_eof() {
  std::array<char, 1024> buffer{};
#ifdef _WIN32
  while (_read(_fileno(stdin), buffer.data(),
               static_cast<unsigned int>(buffer.size())) > 0) {
  }
#else
  while (::read(STDIN_FILENO, buffer.data(), buffer.size()) > 0) {
  }
#endif
}

struct RegisteredAction {
  std::string description;
  Plugin::Action handler;
};

struct ActiveJob {
  std::stop_source cancellation;
  std::shared_ptr<std::atomic_bool> finished;
  std::jthread task;
};

bool canonical_uuid_v4(const std::string &value) {
  static const std::regex uuid{
      "^[0-9a-f]{8}-[0-9a-f]{4}-4[0-9a-f]{3}-[89ab][0-9a-f]{3}-[0-9a-f]{12}$"};
  return std::regex_match(value, uuid);
}

bool ipv4_loopback(const std::string &host) {
  std::array<unsigned int, 4> octets{};
  std::size_t start = 0;
  for (std::size_t index = 0; index < octets.size(); ++index) {
    const auto separator = host.find('.', start);
    if ((separator == std::string::npos) != (index == octets.size() - 1))
      return false;
    const auto end = separator == std::string::npos ? host.size() : separator;
    if (start == end)
      return false;
    const auto [parsed, error] =
        std::from_chars(host.data() + start, host.data() + end, octets[index]);
    if (error != std::errc{} || parsed != host.data() + end ||
        octets[index] > 255) {
      return false;
    }
    start = end + 1;
  }
  return octets[0] == 127;
}

void validate_plugin_id(const std::string &value) {
  static const std::regex label{"^[a-z0-9](?:[a-z0-9-]{0,61}[a-z0-9])?$"};
  std::size_t labels = 0;
  std::size_t start = 0;
  while (start <= value.size()) {
    const auto end = value.find('.', start);
    const auto current = value.substr(start, end - start);
    if (!std::regex_match(current, label))
      throw std::invalid_argument("invalid plugin ID");
    ++labels;
    if (end == std::string::npos)
      break;
    start = end + 1;
  }
  if (value.size() > 191 || labels < 2)
    throw std::invalid_argument("invalid plugin ID");
}

std::string endpoint_target(const char *value) {
  if (value == nullptr)
    throw std::invalid_argument("OLL_PLUGIN_ENDPOINT is required");
  std::string endpoint{value};
  if (!endpoint.starts_with("http://") ||
      endpoint.find_first_of("/?#", 7) != std::string::npos) {
    throw std::invalid_argument("OLL_PLUGIN_ENDPOINT must be an http loopback "
                                "URL with an explicit port");
  }
  auto target = endpoint.substr(7);
  const auto separator = target.rfind(':');
  const auto host = separator == std::string::npos
                        ? std::string{}
                        : target.substr(0, separator);
  const auto port_text = separator == std::string::npos
                             ? std::string{}
                             : target.substr(separator + 1);
  int port = 0;
  const auto [end, error] = std::from_chars(
      port_text.data(), port_text.data() + port_text.size(), port);
  const bool valid_port = error == std::errc{} &&
                          end == port_text.data() + port_text.size() &&
                          port > 0 && port <= 65535;
  const bool loopback =
      host == "localhost" || host == "[::1]" || ipv4_loopback(host);
  if (!loopback || !valid_port) {
    throw std::invalid_argument(
        "OLL_PLUGIN_ENDPOINT must use a loopback host and explicit port");
  }
  return target;
}

void validate_envelope(const oll::protocol::PluginEnvelope &envelope,
                       std::uint64_t &last_id, const Sender *sender = nullptr,
                       std::uint32_t maximum_call_depth = 0,
                       std::uint32_t maximum_causal_depth = 0) {
  if (envelope.message_id() == 0 || envelope.message_id() <= last_id) {
    throw std::runtime_error(
        "host message IDs must be nonzero and strictly increasing");
  }
  if (sender && (envelope.session_id() != sender->session_id() ||
                 envelope.plugin_instance_id() != sender->instance_id())) {
    throw std::runtime_error(
        "host envelope belongs to another plugin instance");
  }
  if (!envelope.has_trace() || envelope.trace().correlation_id().empty()) {
    throw std::runtime_error("host omitted correlation context");
  }
  if (maximum_call_depth != 0 &&
      envelope.trace().call_depth() > maximum_call_depth) {
    throw std::runtime_error("host envelope exceeds maximum call depth");
  }
  if (maximum_causal_depth != 0 &&
      envelope.trace().causal_depth() > maximum_causal_depth) {
    throw std::runtime_error("host envelope exceeds maximum causal depth");
  }
  last_id = envelope.message_id();
}

} // namespace

struct Plugin::Impl {
  std::string id;
  std::string version;
  std::map<std::string, RegisteredAction> actions;

  int run();
};

Plugin::Plugin(std::string plugin_id, std::string version)
    : impl_(std::make_unique<Impl>()) {
  validate_plugin_id(plugin_id);
  if (version.empty())
    throw std::invalid_argument("plugin version must not be empty");
  impl_->id = std::move(plugin_id);
  impl_->version = std::move(version);
}

Plugin::~Plugin() = default;
Plugin::Plugin(Plugin &&) noexcept = default;
Plugin &Plugin::operator=(Plugin &&) noexcept = default;

Plugin &Plugin::action(std::string name, std::string description,
                       Action handler) {
  if (name.empty() || !handler || impl_->actions.contains(name)) {
    throw std::invalid_argument("action names must be nonempty and unique");
  }
  impl_->actions.emplace(
      std::move(name),
      RegisteredAction{std::move(description), std::move(handler)});
  return *this;
}

int Plugin::run() { return impl_->run(); }

int Plugin::Impl::run() {
  grpc::ChannelArguments channel_arguments;
  channel_arguments.SetMaxReceiveMessageSize(maximum_envelope_bytes);
  channel_arguments.SetMaxSendMessageSize(maximum_envelope_bytes);
  auto channel = grpc::CreateCustomChannel(
      endpoint_target(std::getenv("OLL_PLUGIN_ENDPOINT")),
      grpc::InsecureChannelCredentials(), channel_arguments);
  auto stub = oll::protocol::PluginRuntime::NewStub(channel);
  auto client_context = std::make_shared<grpc::ClientContext>();
  auto stream = stub->Connect(client_context.get());
  auto sender = std::make_shared<Sender>(stream.get());
  auto host_impl = std::make_shared<Host::Impl>();
  host_impl->sender = sender;
  Host host{host_impl};
  std::thread([client_context] {
    wait_for_parent_eof();
    client_context->TryCancel();
  }).detach();

  std::uint64_t last_host_id = 0;
  oll::protocol::PluginEnvelope first;
  if (!stream->Read(&first))
    throw std::runtime_error("host closed before HostHello");
  validate_envelope(first, last_host_id);
  if (first.has_reply_to() ||
      first.payload_case() != oll::protocol::PluginEnvelope::kHostHello) {
    throw std::runtime_error("HostHello must be the first host message");
  }
  const auto &hello = first.host_hello();
  if (!hello.has_node() || hello.session_id().empty() ||
      hello.plugin_instance_id().empty() ||
      hello.protocol_schema_sha256() !=
          std::string{
              "\x21\xc1\x45\x63\x8f\xbe\x6a\x1f\x2d\x9a\x2c\xb2\x11\x44\x03\xd4"
              "\xbe\xe4\xda\x3c\x0a\xdb\xac\x09\xe8\x05\xa9\x8a\x77\xd0\xd4"
              "\xda",
              32} ||
      hello.plugin_id().value() != id || hello.plugin_name().value().empty() ||
      hello.maximum_call_depth() == 0 || hello.maximum_causal_depth() == 0 ||
      hello.maximum_artifact_chunk_bytes() == 0) {
    throw std::runtime_error(
        "HostHello does not describe the expected plugin instance");
  }
  if (first.trace().call_depth() > hello.maximum_call_depth() ||
      first.trace().causal_depth() > hello.maximum_causal_depth()) {
    throw std::runtime_error(
        "HostHello exceeds a negotiated trace depth limit");
  }
  sender->identity(hello.session_id(), hello.plugin_instance_id());
  host_impl->maximum_artifact_chunk_bytes =
      hello.maximum_artifact_chunk_bytes();
  host_impl->maximum_call_depth = hello.maximum_call_depth();
  host_impl->maximum_causal_depth = hello.maximum_causal_depth();
  oll::protocol::PluginEnvelope plugin_hello;
  auto *payload = plugin_hello.mutable_plugin_hello();
  payload->mutable_plugin_id()->set_value(id);
  *payload->mutable_plugin_name() = hello.plugin_name();
  payload->set_protocol_schema_sha256(hello.protocol_schema_sha256());
  payload->set_plugin_version(version);
  for (const auto &[name, action] : actions) {
    auto *descriptor = payload->add_actions();
    descriptor->set_name(name);
    descriptor->set_description(action.description);
  }
  sender->send(std::nullopt, first.trace(), std::move(plugin_hello));
  oll::protocol::PluginEnvelope ready;
  if (!stream->Read(&ready))
    throw std::runtime_error("host closed before SessionReady");
  validate_envelope(ready, last_host_id, sender.get(),
                    host_impl->maximum_call_depth,
                    host_impl->maximum_causal_depth);
  if (ready.has_reply_to() ||
      ready.payload_case() != oll::protocol::PluginEnvelope::kReady ||
      ready.trace().correlation_id() != first.trace().correlation_id()) {
    throw std::runtime_error("host SessionReady must follow PluginHello");
  }
  oll::protocol::PluginEnvelope plugin_ready;
  plugin_ready.mutable_ready();
  sender->send(std::nullopt, first.trace(), std::move(plugin_ready));

  std::map<std::string, std::unique_ptr<ActiveJob>> jobs;
  oll::protocol::PluginEnvelope envelope;
  while (stream->Read(&envelope)) {
    std::erase_if(jobs, [](const auto &entry) {
      return entry.second->finished->load(std::memory_order_acquire);
    });
    validate_envelope(envelope, last_host_id, sender.get(),
                      host_impl->maximum_call_depth,
                      host_impl->maximum_causal_depth);
    if (envelope.has_reply_to()) {
      host_impl->route(envelope);
      continue;
    }
    switch (envelope.payload_case()) {
    case oll::protocol::PluginEnvelope::kStartJob: {
      const auto &request = envelope.start_job();
      const auto job_id = request.job_id().value();
      if (!canonical_uuid_v4(job_id) || jobs.contains(job_id) ||
          request.invocation_case() !=
              oll::protocol::StartJobRequest::kAction ||
          !actions.contains(request.action().action())) {
        throw std::runtime_error("invalid StartJobRequest");
      }
      oll::protocol::PluginEnvelope accepted;
      *accepted.mutable_job_accepted()->mutable_job_id() = request.job_id();
      sender->send(envelope.message_id(), envelope.trace(),
                   std::move(accepted));
      auto job = std::make_unique<ActiveJob>();
      job->finished = std::make_shared<std::atomic_bool>(false);
      const auto finished = job->finished;
      const auto cancellation = job->cancellation;
      const auto trace = envelope.trace();
      const auto arguments =
          std::vector<std::string>{request.action().arguments().begin(),
                                   request.action().arguments().end()};
      const auto handler = actions.at(request.action().action()).handler;
      const auto job_id_message = request.job_id();
      const auto parent_call_id = envelope.message_id();
      const auto deadline = request.has_deadline()
                                ? std::optional{request.deadline()}
                                : std::nullopt;
      job->task = std::jthread([&, cancellation, finished, trace, arguments,
                                handler, job_id_message, parent_call_id,
                                deadline](std::stop_token) mutable {
        struct Completion {
          std::shared_ptr<std::atomic_bool> finished;
          ~Completion() { finished->store(true, std::memory_order_release); }
        } completion{finished};
        ActionContext action_context;
        action_context.job_id_ = job_id_message.value();
        action_context.trace_ = trace;
        action_context.deadline_ = deadline;
        action_context.cancellation_ = cancellation.get_token();
        action_context.parent_call_id_ = parent_call_id;
        action_context.host_ = &host;
        try {
          auto result = handler(action_context, arguments);
          if (cancellation.stop_requested())
            return;
          oll::protocol::PluginEnvelope update_envelope;
          auto *update = update_envelope.mutable_job_update();
          *update->mutable_job_id() = job_id_message;
          update->set_state(oll::protocol::JOB_STATE_SUCCEEDED);
          update->set_progress(1.0);
          if (result.result)
            *update->mutable_result() = *result.result;
          for (auto &artifact : result.artifacts)
            *update->add_artifacts() = std::move(artifact);
          sender->send(std::nullopt, trace, std::move(update_envelope));
        } catch (const std::exception &error) {
          if (cancellation.stop_requested())
            return;
          oll::protocol::PluginEnvelope update_envelope;
          auto *update = update_envelope.mutable_job_update();
          *update->mutable_job_id() = job_id_message;
          update->set_state(oll::protocol::JOB_STATE_FAILED);
          update->set_progress(1.0);
          update->mutable_error()->set_code(oll::protocol::ERROR_CODE_INTERNAL);
          update->mutable_error()->set_message(error.what());
          sender->send(std::nullopt, trace, std::move(update_envelope));
        }
      });
      jobs.emplace(job_id, std::move(job));
      break;
    }
    case oll::protocol::PluginEnvelope::kCancelJob: {
      const auto id = envelope.cancel_job().job_id().value();
      const auto found = jobs.find(id);
      if (found == jobs.end())
        throw std::runtime_error("cancellation names no active job");
      found->second->cancellation.request_stop();
      found->second->task.request_stop();
      jobs.erase(found);
      oll::protocol::PluginEnvelope acknowledged;
      *acknowledged.mutable_cancel_job_acknowledged()->mutable_job_id() =
          envelope.cancel_job().job_id();
      sender->send(envelope.message_id(), envelope.trace(),
                   std::move(acknowledged));
      break;
    }
    case oll::protocol::PluginEnvelope::kHeartbeat: {
      oll::protocol::PluginEnvelope heartbeat;
      *heartbeat.mutable_heartbeat() = envelope.heartbeat();
      sender->send(envelope.message_id(), envelope.trace(),
                   std::move(heartbeat));
      break;
    }
    case oll::protocol::PluginEnvelope::kShutdown: {
      for (auto &[_, job] : jobs) {
        job->cancellation.request_stop();
        job->task.request_stop();
      }
      jobs.clear();
      oll::protocol::PluginEnvelope acknowledged;
      acknowledged.mutable_shutdown_acknowledged();
      sender->send(envelope.message_id(), envelope.trace(),
                   std::move(acknowledged));
      stream->WritesDone();
      oll::protocol::PluginEnvelope after_shutdown;
      if (stream->Read(&after_shutdown)) {
        throw std::runtime_error("host sent a message after ShutdownRequest");
      }
      const auto status = stream->Finish();
      return status.ok() ? 0 : 1;
    }
    case oll::protocol::PluginEnvelope::kProtocolError:
      throw std::runtime_error("host protocol error: " +
                               envelope.protocol_error().message());
    default:
      throw std::runtime_error("unexpected host-initiated message");
    }
  }
  return 0;
}

} // namespace onelastleaf
