#pragma once

#include <condition_variable>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <stop_token>
#include <string>
#include <utility>

#include <grpcpp/grpcpp.h>
#include <oll/plugin.grpc.pb.h>

#include <onelastleaf/plugin_sdk.hpp>

namespace onelastleaf {

class Sender {
public:
  using Writer =
      std::function<bool(const oll::protocol::PluginEnvelope &envelope)>;

  explicit Sender(
      grpc::ClientReaderWriter<oll::protocol::PluginEnvelope,
                               oll::protocol::PluginEnvelope> *stream)
      : Sender([stream](const oll::protocol::PluginEnvelope &envelope) {
          return stream->Write(envelope);
        }) {}
  explicit Sender(Writer writer);

  void identity(std::string session_id, std::string instance_id);
  std::uint64_t
  send(std::optional<std::uint64_t> reply_to,
       const oll::protocol::TraceContext &trace,
       oll::protocol::PluginEnvelope envelope,
       const std::function<void(std::uint64_t)> &before_write = {});
  void close() noexcept;
  const std::string &session_id() const noexcept { return session_id_; }
  const std::string &instance_id() const noexcept { return instance_id_; }

private:
  Writer writer_;
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
    bool cancelled{};
    bool abandoned{};
  };

  std::shared_ptr<Sender> sender;
  std::uint64_t maximum_artifact_chunk_bytes{};
  std::uint32_t maximum_call_depth{};
  std::uint32_t maximum_causal_depth{};
  mutable std::mutex mutex;
  std::map<std::uint64_t, std::shared_ptr<Pending>> pending;
  bool closed{};
  std::string close_reason{"plugin session is closed"};

  std::uint64_t send(const oll::protocol::TraceContext &trace,
                     oll::protocol::PluginEnvelope envelope,
                     std::stop_token cancellation);
  oll::protocol::PluginEnvelope
  request(const oll::protocol::TraceContext &trace,
          oll::protocol::PluginEnvelope envelope, std::stop_token cancellation);
  void route(const oll::protocol::PluginEnvelope &envelope);
  void close(std::string reason) noexcept;
  oll::protocol::ArtifactStored store_artifact(
      const oll::protocol::TraceContext &trace, std::string job_id,
      oll::protocol::ArtifactDescriptor descriptor, std::uint32_t chunk_count,
      Host::ArtifactChunkSource chunk_source, std::stop_token cancellation);
};

namespace detail {

struct HostAccess {
  using Impl = Host::Impl;

  static std::shared_ptr<Impl> make_impl() { return std::make_shared<Impl>(); }
  static Host make(std::shared_ptr<Impl> impl,
                   std::stop_token cancellation = {}) {
    return Host{std::move(impl), cancellation};
  }
};

} // namespace detail

} // namespace onelastleaf
