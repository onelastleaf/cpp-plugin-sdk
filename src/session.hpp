#pragma once

#include "job_registry.hpp"

#include <cstdint>
#include <exception>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <stop_token>
#include <string>
#include <vector>

#include <google/protobuf/timestamp.pb.h>
#include <grpcpp/grpcpp.h>
#include <oll/plugin.grpc.pb.h>

#include <onelastleaf/plugin_sdk.hpp>

namespace onelastleaf {

class Sender;

namespace detail {

struct RegisteredAction {
  std::string description;
  Plugin::Action handler;
};

class ParentLivenessWatcher;

grpc::ChannelArguments plugin_channel_arguments();

class PluginSession final {
public:
  PluginSession(std::string plugin_id, std::string plugin_version,
                const std::map<std::string, RegisteredAction> &actions);
  ~PluginSession();

  PluginSession(const PluginSession &) = delete;
  PluginSession &operator=(const PluginSession &) = delete;

  int run();

private:
  using Envelope = oll::protocol::PluginEnvelope;
  using Stream = grpc::ClientReaderWriter<Envelope, Envelope>;

  void connect();
  void handshake();
  int read_messages();
  void start_job(const Envelope &envelope);
  void cancel_job(const Envelope &envelope);
  int shutdown(const Envelope &envelope);
  void execute_action(Plugin::Action handler, std::string job_id,
                      oll::protocol::PluginJobId job_id_message,
                      oll::protocol::TraceContext trace,
                      std::vector<std::string> arguments,
                      std::optional<google::protobuf::Timestamp> deadline,
                      std::uint64_t parent_call_id,
                      std::stop_token cancellation) noexcept;
  void send_failure(const oll::protocol::PluginJobId &job_id,
                    const oll::protocol::TraceContext &trace,
                    std::string message, std::stop_token cancellation);
  void record_failure(std::exception_ptr failure) noexcept;
  std::exception_ptr failure() const noexcept;
  void cleanup(std::string reason, bool cancel_rpc) noexcept;

  std::string plugin_id_;
  std::string plugin_version_;
  const std::map<std::string, RegisteredAction> &actions_;
  std::shared_ptr<grpc::ClientContext> client_context_;
  std::unique_ptr<Stream> stream_;
  std::shared_ptr<Sender> sender_;
  std::shared_ptr<Host::Impl> host_impl_;
  std::unique_ptr<ParentLivenessWatcher> parent_liveness_;
  std::uint64_t last_host_id_{};
  bool stream_finished_{};
  bool cleaned_{};
  mutable std::mutex failure_mutex_;
  std::exception_ptr failure_;
  JobRegistry jobs_;
};

} // namespace detail
} // namespace onelastleaf
