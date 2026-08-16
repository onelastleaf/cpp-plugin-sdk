#include "validation.hpp"

#include <array>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <regex>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>

#include <oll/plugin.pb.h>

#include <onelastleaf/plugin_sdk.hpp>

namespace onelastleaf::detail {
namespace {

bool ipv4_loopback(std::string_view host) {
  std::array<unsigned int, 4> octets{};
  std::size_t start = 0;
  for (std::size_t index = 0; index < octets.size(); ++index) {
    const auto separator = host.find('.', start);
    if ((separator == std::string_view::npos) != (index == octets.size() - 1)) {
      return false;
    }
    const auto end =
        separator == std::string_view::npos ? host.size() : separator;
    if (start == end) {
      return false;
    }
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

unsigned char hex_nibble(char value) {
  if (value >= '0' && value <= '9') {
    return static_cast<unsigned char>(value - '0');
  }
  if (value >= 'a' && value <= 'f') {
    return static_cast<unsigned char>(value - 'a' + 10);
  }
  throw std::logic_error("protocol fingerprint is not lowercase hexadecimal");
}

} // namespace

bool canonical_uuid_v4(std::string_view value) {
  static const std::regex uuid{
      "^[0-9a-f]{8}-[0-9a-f]{4}-4[0-9a-f]{3}-[89ab][0-9a-f]{3}-[0-9a-f]{12}$"};
  return std::regex_match(value.begin(), value.end(), uuid);
}

void validate_plugin_id(std::string_view value) {
  static const std::regex label{"^[a-z0-9](?:[a-z0-9-]{0,61}[a-z0-9])?$"};
  std::size_t labels = 0;
  std::size_t start = 0;
  while (start <= value.size()) {
    const auto end = value.find('.', start);
    const auto current = value.substr(start, end - start);
    if (!std::regex_match(current.begin(), current.end(), label)) {
      throw std::invalid_argument("invalid plugin ID");
    }
    ++labels;
    if (end == std::string_view::npos) {
      break;
    }
    start = end + 1;
  }
  if (value.size() > 191 || labels < 2) {
    throw std::invalid_argument("invalid plugin ID");
  }
}

std::string endpoint_target(const char *value) {
  if (value == nullptr) {
    throw std::invalid_argument("OLL_PLUGIN_ENDPOINT is required");
  }
  std::string endpoint{value};
  if (!endpoint.starts_with("http://") ||
      endpoint.find_first_of("/?#", 7) != std::string::npos) {
    throw std::invalid_argument("OLL_PLUGIN_ENDPOINT must be an http loopback "
                                "URL with an explicit port");
  }
  auto target = endpoint.substr(7);
  const auto separator = target.rfind(':');
  const auto host = separator == std::string::npos
                        ? std::string_view{}
                        : std::string_view{target}.substr(0, separator);
  const auto port_text = separator == std::string::npos
                             ? std::string_view{}
                             : std::string_view{target}.substr(separator + 1);
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

const std::string &protocol_schema_sha256_bytes() {
  static const std::string bytes = [] {
    constexpr auto hex = protocol_schema_sha256;
    static_assert(hex.size() == 64);
    std::string result(hex.size() / 2, '\0');
    for (std::size_t index = 0; index < result.size(); ++index) {
      result[index] = static_cast<char>((hex_nibble(hex[index * 2]) << 4U) |
                                        hex_nibble(hex[index * 2 + 1]));
    }
    return result;
  }();
  return bytes;
}

void validate_envelope(const oll::protocol::PluginEnvelope &envelope,
                       std::uint64_t &last_id,
                       std::string_view expected_session_id,
                       std::string_view expected_instance_id,
                       std::uint32_t maximum_call_depth,
                       std::uint32_t maximum_causal_depth) {
  if (envelope.message_id() == 0 || envelope.message_id() <= last_id) {
    throw std::runtime_error(
        "host message IDs must be nonzero and strictly increasing");
  }
  if ((!expected_session_id.empty() &&
       envelope.session_id() != expected_session_id) ||
      (!expected_instance_id.empty() &&
       envelope.plugin_instance_id() != expected_instance_id)) {
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

} // namespace onelastleaf::detail
