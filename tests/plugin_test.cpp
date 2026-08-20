#include <onelastleaf/plugin_sdk.hpp>

#include <cassert>
#include <stdexcept>

int main() {
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
