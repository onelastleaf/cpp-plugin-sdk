#include "session.hpp"
#include "validation.hpp"

#include <map>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>

#include <onelastleaf/plugin_sdk.hpp>

namespace onelastleaf {

struct Plugin::Impl {
  std::string id;
  std::string version;
  std::map<std::string, detail::RegisteredAction> actions;
  bool has_run{};
};

Plugin::Plugin(std::string plugin_id, std::string version)
    : impl_(std::make_unique<Impl>()) {
  detail::validate_plugin_id(plugin_id);
  if (version.empty()) {
    throw std::invalid_argument("plugin version must not be empty");
  }
  impl_->id = std::move(plugin_id);
  impl_->version = std::move(version);
}

Plugin::~Plugin() = default;
Plugin::Plugin(Plugin &&) noexcept = default;
Plugin &Plugin::operator=(Plugin &&) noexcept = default;

Plugin &Plugin::action(std::string name, std::string description,
                       Action handler) {
  if (impl_->has_run) {
    throw std::logic_error("plugin actions cannot change after run()");
  }
  if (name.empty() || !handler || impl_->actions.contains(name)) {
    throw std::invalid_argument("action names must be nonempty and unique");
  }
  impl_->actions.emplace(
      std::move(name),
      detail::RegisteredAction{std::move(description), std::move(handler)});
  return *this;
}

int Plugin::run() {
  if (impl_->has_run) {
    throw std::logic_error("Plugin::run() may only be called once");
  }
  impl_->has_run = true;
  detail::PluginSession session{impl_->id, impl_->version, impl_->actions};
  return session.run();
}

} // namespace onelastleaf
