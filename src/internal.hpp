#pragma once

#include <condition_variable>
#include <functional>
#include <map>
#include <mutex>

#include <grpcpp/grpcpp.h>
#include <oll/plugin.grpc.pb.h>

#include <onelastleaf/plugin_sdk.hpp>

namespace onelastleaf {

class Sender {
 public:
  explicit Sender(grpc::ClientReaderWriter<oll::protocol::PluginEnvelope,
                                            oll::protocol::PluginEnvelope>* stream)
      : stream_(stream) {}

  void identity(std::string session_id, std::string instance_id);
  std::uint64_t send(std::optional<std::uint64_t> reply_to,
                     const oll::protocol::TraceContext& trace,
                     oll::protocol::PluginEnvelope envelope,
                     const std::function<void(std::uint64_t)>& before_write = {});
  const std::string& session_id() const noexcept { return session_id_; }
  const std::string& instance_id() const noexcept { return instance_id_; }

 private:
  grpc::ClientReaderWriter<oll::protocol::PluginEnvelope,
                           oll::protocol::PluginEnvelope>* stream_;
  std::uint64_t next_id_{1};
  std::mutex write_;
  std::string session_id_;
  std::string instance_id_;
};

struct Host::Impl {
  struct Pending {
    std::string correlation_id;
    std::condition_variable ready;
    std::optional<oll::protocol::PluginEnvelope> response;
  };

  std::shared_ptr<Sender> sender;
  std::uint64_t maximum_artifact_chunk_bytes{};
  std::uint32_t maximum_call_depth{};
  std::uint32_t maximum_causal_depth{};
  std::mutex mutex;
  std::map<std::uint64_t, std::shared_ptr<Pending>> pending;

  oll::protocol::PluginEnvelope request(
      const oll::protocol::TraceContext& trace,
      oll::protocol::PluginEnvelope envelope);
  void route(const oll::protocol::PluginEnvelope& envelope);
};

}  // namespace onelastleaf
