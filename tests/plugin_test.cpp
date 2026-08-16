#include <onelastleaf/plugin_sdk.hpp>

#include <cassert>
#include <stdexcept>

int main() {
  assert(std::string{onelastleaf::protocol_schema_sha256} ==
         "21c145638fbe6a1f2d9a2cb2114403d4bee4da3c0adbac09e805a98a77d0d4da");
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
