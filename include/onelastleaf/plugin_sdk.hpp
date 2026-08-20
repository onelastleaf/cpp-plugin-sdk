#pragma once

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <stop_token>
#include <string>
#include <string_view>
#include <vector>

#include <oll/plugin.pb.h>

namespace onelastleaf {

namespace detail {
class PluginSession;
struct HostAccess;
} // namespace detail

struct ActionResult {
  std::optional<oll::protocol::ConfigValue> result;
  std::vector<oll::protocol::ArtifactDescriptor> artifacts;

  static ActionResult string(std::string value);
};

class Host {
public:
  using ArtifactChunkSource =
      std::function<std::string(std::uint32_t chunk_index)>;

  Host(const Host &) = default;
  Host &operator=(const Host &) = default;
  Host(Host &&) noexcept = default;
  Host &operator=(Host &&) noexcept = default;

  oll::protocol::HostCallResponse
  call(const oll::protocol::TraceContext &trace,
       oll::protocol::HostCallRequest request) const;
  oll::protocol::GetConfigResponse get_config(
      const oll::protocol::TraceContext &trace,
      std::optional<oll::protocol::ConfigPath> path = std::nullopt) const;
  oll::protocol::InvokeConfigFunctionResponse invoke_config_function(
      const oll::protocol::TraceContext &trace,
      oll::protocol::ConfigFunctionRef function,
      std::vector<oll::protocol::ConfigValue> arguments = {}) const;
  void log(const oll::protocol::TraceContext &trace,
           oll::protocol::LogLevel level, std::string target,
           std::string message,
           std::map<std::string, oll::protocol::ConfigValue> fields = {}) const;
  oll::protocol::ArtifactStored
  store_artifact(const oll::protocol::TraceContext &trace, std::string job_id,
                 oll::protocol::ArtifactDescriptor descriptor,
                 std::vector<std::string> chunks) const;
  oll::protocol::ArtifactStored
  store_artifact(const oll::protocol::TraceContext &trace, std::string job_id,
                 oll::protocol::ArtifactDescriptor descriptor,
                 std::uint32_t chunk_count,
                 ArtifactChunkSource chunk_source) const;
  std::uint64_t maximum_artifact_chunk_bytes() const noexcept;
  std::uint32_t maximum_call_depth() const noexcept;

private:
  struct Impl;
  Host(std::shared_ptr<Impl> impl, std::stop_token cancellation);
  std::shared_ptr<Impl> impl_;
  std::stop_token cancellation_;
  friend class detail::PluginSession;
  friend struct detail::HostAccess;
};

class ActionContext {
public:
  const std::string &job_id() const noexcept { return job_id_; }
  const oll::protocol::TraceContext &trace() const noexcept { return trace_; }
  const std::optional<google::protobuf::Timestamp> &deadline() const noexcept {
    return deadline_;
  }
  std::stop_token cancellation() const noexcept { return cancellation_; }
  Host host() const noexcept { return host_; }
  oll::protocol::HostCallResponse
  host_call(oll::protocol::HostCallRequest request) const;
  oll::protocol::GetConfigResponse get_config(
      std::optional<oll::protocol::ConfigPath> path = std::nullopt) const;
  oll::protocol::InvokeConfigFunctionResponse invoke_config_function(
      oll::protocol::ConfigFunctionRef function,
      std::vector<oll::protocol::ConfigValue> arguments = {}) const;

private:
  ActionContext(std::string job_id, oll::protocol::TraceContext trace,
                std::optional<google::protobuf::Timestamp> deadline,
                std::stop_token cancellation, std::uint64_t parent_call_id,
                Host host);
  friend class detail::PluginSession;
  std::string job_id_;
  oll::protocol::TraceContext trace_;
  std::optional<google::protobuf::Timestamp> deadline_;
  std::stop_token cancellation_;
  std::uint64_t parent_call_id_{};
  Host host_;

  oll::protocol::TraceContext nested_trace() const;
};

class Plugin {
public:
  using Action = std::function<ActionResult(ActionContext &,
                                            const std::vector<std::string> &)>;

  Plugin(std::string plugin_id, std::string version);
  ~Plugin();
  Plugin(Plugin &&) noexcept;
  Plugin &operator=(Plugin &&) noexcept;
  Plugin(const Plugin &) = delete;
  Plugin &operator=(const Plugin &) = delete;

  Plugin &action(std::string name, std::string description, Action handler);
  int run();

private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

} // namespace onelastleaf
