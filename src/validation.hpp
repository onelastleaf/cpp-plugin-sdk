#pragma once

#include <cstdint>
#include <string>
#include <string_view>

#include <oll/plugin.pb.h>

namespace onelastleaf::detail {

bool canonical_uuid_v4(std::string_view value);
void validate_plugin_id(std::string_view value);
std::string endpoint_target(const char *value);
void validate_envelope(const oll::protocol::PluginEnvelope &envelope,
                       std::uint64_t &last_id,
                       std::string_view expected_session_id = {},
                       std::string_view expected_instance_id = {},
                       std::uint32_t maximum_call_depth = 0,
                       std::uint32_t maximum_causal_depth = 0);

} // namespace onelastleaf::detail
