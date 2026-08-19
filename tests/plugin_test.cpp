#include <onelastleaf/plugin_sdk.hpp>

#include <cassert>
#include <stdexcept>

int main() {
  assert(std::string{onelastleaf::protocol_schema_sha256} ==
         "9b236b37455965858413f5717a88e28568a459e81e87a28ff77be8845bcff75a");
  assert(onelastleaf::ActionResult::string("value").result->string_value() ==
         "value");
  try {
    onelastleaf::Plugin invalid{"invalid", "0.1.0"};
    return 1;
  } catch (const std::invalid_argument &) {
  }
  try {
    onelastleaf::Plugin invalid{"org.example.echo", ""};
    return 1;
  } catch (const std::invalid_argument &) {
  }
}
