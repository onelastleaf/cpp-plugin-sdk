#include <chrono>
#include <stop_token>
#include <string>
#include <thread>
#include <vector>

#include <onelastleaf/plugin_sdk.hpp>

namespace {

oll::protocol::ConfigValue string_value(std::string value) {
  oll::protocol::ConfigValue result;
  result.set_string_value(std::move(value));
  return result;
}

onelastleaf::ActionResult echo(onelastleaf::ActionContext &,
                               const std::vector<std::string> &arguments) {
  std::string output;
  for (const auto &argument : arguments) {
    if (!output.empty())
      output += ' ';
    output += argument;
  }
  return onelastleaf::ActionResult::string(std::move(output));
}

onelastleaf::ActionResult wait(onelastleaf::ActionContext &context,
                               const std::vector<std::string> &) {
  while (!context.cancellation().stop_requested()) {
    std::this_thread::sleep_for(std::chrono::milliseconds{1});
  }
  return {};
}

onelastleaf::ActionResult host_calls(onelastleaf::ActionContext &context,
                                     const std::vector<std::string> &) {
  auto configured = context.get_config();
  if (!configured.has_value() ||
      configured.value().kind_case() !=
          oll::protocol::ConfigValue::kFunctionValue) {
    throw std::runtime_error("GetConfig omitted function");
  }
  auto invoked = context.invoke_config_function(
      configured.value().function_value(), {string_value("config")});
  if (invoked.results_size() != 1 ||
      invoked.results(0).kind_case() !=
          oll::protocol::ConfigValue::kStringValue) {
    throw std::runtime_error("configuration function omitted string result");
  }
  oll::protocol::HostCallRequest request;
  auto *read = request.mutable_read_document();
  read->mutable_path()->set_value("/conformance.md");
  read->set_projection(oll::protocol::DOCUMENT_PROJECTION_CONTENT);
  auto document = context.host_call(std::move(request));
  if (document.result_case() !=
          oll::protocol::HostCallResponse::kReadDocument ||
      !document.read_document().has_document() ||
      document.read_document().document().representation_case() !=
          oll::protocol::DocumentSnapshot::kContent) {
    throw std::runtime_error("document call omitted text content");
  }
  context.host().log(context.trace(), oll::protocol::LOG_LEVEL_INFO,
                     "conformance", "host action complete");
  return onelastleaf::ActionResult::string(
      invoked.results(0).string_value() + "|" +
      document.read_document().document().content());
}

onelastleaf::ActionResult artifact(onelastleaf::ActionContext &context,
                                   const std::vector<std::string> &) {
  oll::protocol::ArtifactDescriptor descriptor;
  descriptor.mutable_artifact_id()->set_value(
      "aaaaaaaa-aaaa-4aaa-8aaa-aaaaaaaaaaaa");
  descriptor.set_file_name("conformance.txt");
  descriptor.set_media_type("text/plain");
  descriptor.set_size_bytes(16);
  descriptor.set_sha256(
      "\xa1\x1a\x40\x45\xc8\x9f\x72\x7f\xad\xb9\xae\xdd\xb0\xf2\x96\x37"
      "\xce\x5b\x50\x58\x46\xaf\xeb\xd8\x2a\xe2\xc0\x1b\x67\x33\xa6\xb5",
      32);
  context.host().store_artifact(context.trace(), context.job_id(), descriptor,
                                {"artifact ", "payload"});
  onelastleaf::ActionResult result =
      onelastleaf::ActionResult::string("artifact");
  result.artifacts.push_back(std::move(descriptor));
  return result;
}

} // namespace

int main() {
  onelastleaf::Plugin plugin{"org.onelastleaf.conformance", "0.1.0"};
  plugin.action("echo", "Echo arguments", echo)
      .action("wait", "Wait for cancellation", wait)
      .action("host", "Exercise host capabilities", host_calls)
      .action("artifact", "Exercise artifact transfer", artifact);
  return plugin.run();
}
