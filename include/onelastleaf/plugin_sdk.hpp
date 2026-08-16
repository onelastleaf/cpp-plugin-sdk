#pragma once

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <stop_token>
#include <string>
#include <vector>

#include <oll/plugin.pb.h>

namespace onelastleaf {

inline constexpr char protocol_schema_sha256[] =
    "21c145638fbe6a1f2d9a2cb2114403d4bee4da3c0adbac09e805a98a77d0d4da";

struct ActionResult {
  std::optional<oll::protocol::ConfigValue> result;
  std::vector<oll::protocol::ArtifactDescriptor> artifacts;

  static ActionResult string(std::string value);
};

class Host;

class ActionContext {
public:
  const std::string &job_id() const noexcept { return job_id_; }
  const oll::protocol::TraceContext &trace() const noexcept { return trace_; }
  const std::optional<google::protobuf::Timestamp> &deadline() const noexcept {
    return deadline_;
  }
  std::stop_token cancellation() const noexcept { return cancellation_; }
  Host &host() const noexcept { return *host_; }
  oll::protocol::HostCallResponse
  host_call(oll::protocol::HostCallRequest request) const;
  oll::protocol::GetConfigResponse get_config(
      std::optional<oll::protocol::ConfigPath> path = std::nullopt) const;
  oll::protocol::InvokeConfigFunctionResponse invoke_config_function(
      oll::protocol::ConfigFunctionRef function,
      std::vector<oll::protocol::ConfigValue> arguments = {}) const;

private:
  friend class Plugin;
  std::string job_id_;
  oll::protocol::TraceContext trace_;
  std::optional<google::protobuf::Timestamp> deadline_;
  std::stop_token cancellation_;
  std::uint64_t parent_call_id_{};
  Host *host_{};

  oll::protocol::TraceContext nested_trace() const;
};

class Host {
public:
  oll::protocol::HostCallResponse call(const oll::protocol::TraceContext &trace,
                                       oll::protocol::HostCallRequest request);
  oll::protocol::GetConfigResponse
  get_config(const oll::protocol::TraceContext &trace,
             std::optional<oll::protocol::ConfigPath> path = std::nullopt);
  oll::protocol::InvokeConfigFunctionResponse invoke_config_function(
      const oll::protocol::TraceContext &trace,
      oll::protocol::ConfigFunctionRef function,
      std::vector<oll::protocol::ConfigValue> arguments = {});
  void log(const oll::protocol::TraceContext &trace,
           oll::protocol::LogLevel level, std::string target,
           std::string message,
           std::map<std::string, oll::protocol::ConfigValue> fields = {});
  oll::protocol::ArtifactStored
  store_artifact(const oll::protocol::TraceContext &trace, std::string job_id,
                 oll::protocol::ArtifactDescriptor descriptor,
                 std::vector<std::string> chunks);
  std::uint64_t maximum_artifact_chunk_bytes() const noexcept;
  std::uint32_t maximum_call_depth() const noexcept;

private:
  struct Impl;
  explicit Host(std::shared_ptr<Impl> impl);
  std::shared_ptr<Impl> impl_;
  friend class Plugin;
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
