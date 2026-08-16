#include "internal.hpp"
#include "job_registry.hpp"
#include "parent_liveness.hpp"
#include "validation.hpp"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <future>
#include <memory>
#include <optional>
#include <stdexcept>
#include <stop_token>
#include <string>
#include <string_view>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

#include <oll/plugin.pb.h>
#include <openssl/evp.h>

#include <onelastleaf/plugin_sdk.hpp>

#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#else
#include <unistd.h>
#endif

namespace {

using namespace std::chrono_literals;
using onelastleaf::detail::HostAccess;

void expect(bool condition, const char *message) {
  if (!condition) {
    throw std::runtime_error(message);
  }
}

template <typename Exception, typename Function>
void expect_throws(Function &&function, const char *message) {
  try {
    std::forward<Function>(function)();
  } catch (const Exception &) {
    return;
  }
  throw std::runtime_error(message);
}

oll::protocol::TraceContext trace() {
  oll::protocol::TraceContext result;
  result.set_correlation_id("test-correlation");
  return result;
}

std::string sha256(std::string_view bytes) {
  auto digest = std::unique_ptr<EVP_MD_CTX, decltype(&EVP_MD_CTX_free)>{
      EVP_MD_CTX_new(), EVP_MD_CTX_free};
  expect(digest != nullptr, "could not allocate test digest");
  expect(EVP_DigestInit_ex(digest.get(), EVP_sha256(), nullptr) == 1,
         "could not initialize test digest");
  expect(EVP_DigestUpdate(digest.get(), bytes.data(), bytes.size()) == 1,
         "could not update test digest");
  std::string result(32, '\0');
  unsigned int size = 0;
  expect(EVP_DigestFinal_ex(digest.get(),
                            reinterpret_cast<unsigned char *>(result.data()),
                            &size) == 1,
         "could not finish test digest");
  result.resize(size);
  return result;
}

struct ArtifactFixture {
  std::shared_ptr<HostAccess::Impl> impl = HostAccess::make_impl();
  std::vector<oll::protocol::PluginEnvelope> writes;
  onelastleaf::Host host;

  ArtifactFixture()
      : host([this] {
          impl->maximum_artifact_chunk_bytes = 16;
          impl->maximum_call_depth = 8;
          impl->sender = std::make_shared<onelastleaf::Sender>(
              [this](const oll::protocol::PluginEnvelope &envelope) {
                writes.push_back(envelope);
                oll::protocol::PluginEnvelope response;
                response.set_reply_to(envelope.message_id());
                *response.mutable_trace() = envelope.trace();
                if (envelope.payload_case() ==
                    oll::protocol::PluginEnvelope::kArtifactStart) {
                  *response.mutable_artifact_accepted()->mutable_artifact_id() =
                      envelope.artifact_start().artifact().artifact_id();
                  impl->route(response);
                } else if (envelope.payload_case() ==
                           oll::protocol::PluginEnvelope::kArtifactComplete) {
                  *response.mutable_artifact_stored()->mutable_artifact_id() =
                      envelope.artifact_complete().artifact_id();
                  impl->route(response);
                }
                return true;
              });
          return HostAccess::make(impl);
        }()) {}
};

oll::protocol::ArtifactDescriptor descriptor(std::string bytes) {
  oll::protocol::ArtifactDescriptor result;
  result.mutable_artifact_id()->set_value(
      "aaaaaaaa-aaaa-4aaa-8aaa-aaaaaaaaaaaa");
  result.set_file_name("test.bin");
  result.set_media_type("application/octet-stream");
  result.set_size_bytes(bytes.size());
  result.set_sha256(sha256(bytes));
  return result;
}

void host_call_cancellation_wakes_the_waiter() {
  auto impl = HostAccess::make_impl();
  std::promise<std::uint64_t> written;
  auto written_result = written.get_future();
  impl->sender = std::make_shared<onelastleaf::Sender>(
      [&written](const oll::protocol::PluginEnvelope &envelope) {
        written.set_value(envelope.message_id());
        return true;
      });

  std::promise<std::string> result;
  auto result_value = result.get_future();
  onelastleaf::detail::JobRegistry jobs{[](std::exception_ptr) {
    throw std::runtime_error("unexpected failure");
  }};
  jobs.start("job", [impl, &result](std::stop_token cancellation) {
    try {
      oll::protocol::PluginEnvelope request;
      request.mutable_host_call()->mutable_get_config();
      static_cast<void>(
          impl->request(trace(), std::move(request), cancellation));
      result.set_value("unexpected response");
    } catch (const std::exception &error) {
      result.set_value(error.what());
    }
  });

  expect(written_result.wait_for(2s) == std::future_status::ready,
         "host request was not written");
  const auto request_id = written_result.get();
  expect(jobs.request_stop("job"), "active job did not accept cancellation");
  expect(result_value.wait_for(2s) == std::future_status::ready,
         "cancelled host request remained blocked");
  expect(result_value.get().find("cancelled") != std::string::npos,
         "cancelled host request returned the wrong error");
  jobs.wait();

  oll::protocol::PluginEnvelope late_response;
  late_response.set_reply_to(request_id);
  *late_response.mutable_trace() = trace();
  late_response.mutable_host_result()->mutable_get_config();
  impl->route(late_response);
}

void closing_a_session_wakes_host_calls() {
  auto impl = HostAccess::make_impl();
  std::promise<void> written;
  auto written_result = written.get_future();
  impl->sender = std::make_shared<onelastleaf::Sender>(
      [&written](const oll::protocol::PluginEnvelope &) {
        written.set_value();
        return true;
      });
  std::promise<std::string> result;
  auto result_value = result.get_future();
  std::jthread caller{[impl, &result] {
    try {
      oll::protocol::PluginEnvelope request;
      request.mutable_host_call()->mutable_get_config();
      static_cast<void>(impl->request(trace(), std::move(request), {}));
      result.set_value("unexpected response");
    } catch (const std::exception &error) {
      result.set_value(error.what());
    }
  }};
  expect(written_result.wait_for(2s) == std::future_status::ready,
         "host request was not written before close");
  impl->close("closed for test");
  expect(result_value.wait_for(2s) == std::future_status::ready,
         "session close left a host request blocked");
  expect(result_value.get() == "closed for test",
         "session close returned the wrong error");
}

void retained_hosts_fail_after_close() {
  static_assert(
      !std::is_reference_v<
          decltype(std::declval<const onelastleaf::ActionContext &>().host())>);
  auto impl = HostAccess::make_impl();
  std::atomic_int writes{};
  impl->sender = std::make_shared<onelastleaf::Sender>(
      [&writes](const oll::protocol::PluginEnvelope &) {
        ++writes;
        return true;
      });
  auto retained = HostAccess::make(impl);
  impl->close("retained host session is closed");
  expect_throws<std::runtime_error>(
      [&] {
        retained.log(trace(), oll::protocol::LOG_LEVEL_INFO, "test", "test");
      },
      "retained Host remained usable after close");
  expect(writes.load() == 0, "closed Host wrote to its former stream");
}

void retained_hosts_fail_after_their_job_finishes() {
  auto impl = HostAccess::make_impl();
  std::atomic_int writes{};
  impl->sender = std::make_shared<onelastleaf::Sender>(
      [&writes](const oll::protocol::PluginEnvelope &) {
        ++writes;
        return true;
      });
  std::promise<onelastleaf::Host> retained;
  auto retained_result = retained.get_future();
  onelastleaf::detail::JobRegistry jobs{[](std::exception_ptr) {
    throw std::runtime_error("unexpected failure");
  }};
  jobs.start("job", [impl, &retained](std::stop_token cancellation) {
    retained.set_value(HostAccess::make(impl, cancellation));
  });
  auto host = retained_result.get();
  jobs.wait();
  expect_throws<std::runtime_error>(
      [&] { host.log(trace(), oll::protocol::LOG_LEVEL_INFO, "test", "test"); },
      "retained Host remained usable after its job finished");
  expect(writes.load() == 0, "finished job wrote through a retained Host");
}

void jobs_reap_without_an_inbound_message_and_contain_exceptions() {
  std::promise<void> failure;
  auto failed = failure.get_future();
  onelastleaf::detail::JobRegistry jobs{[&failure](std::exception_ptr error) {
    try {
      std::rethrow_exception(error);
    } catch (int value) {
      if (value == 42) {
        failure.set_value();
      }
    }
  }};
  jobs.start("throwing", [](std::stop_token) { throw 42; });
  expect(failed.wait_for(2s) == std::future_status::ready,
         "non-standard worker exception escaped or was lost");
  jobs.wait();
  expect(jobs.size() == 0,
         "completed job was not reaped without another host message");
}

void empty_and_streamed_artifacts_are_supported() {
  ArtifactFixture empty;
  const auto stored =
      empty.host.store_artifact(trace(), "bbbbbbbb-bbbb-4bbb-8bbb-bbbbbbbbbbbb",
                                descriptor(""), std::vector<std::string>{});
  expect(stored.artifact_id().value() == "aaaaaaaa-aaaa-4aaa-8aaa-aaaaaaaaaaaa",
         "empty artifact was not stored");
  expect(empty.writes.size() == 2,
         "empty artifact should only send start and complete");
  expect(empty.writes[0].artifact_start().chunk_count() == 0,
         "empty artifact announced a chunk");

  ArtifactFixture streamed;
  std::vector<std::uint32_t> requested;
  static_cast<void>(streamed.host.store_artifact(
      trace(), "bbbbbbbb-bbbb-4bbb-8bbb-bbbbbbbbbbbb", descriptor("ab"), 2,
      [&requested](std::uint32_t index) {
        requested.push_back(index);
        return index == 0 ? std::string{"a"} : std::string{"b"};
      }));
  expect((requested == std::vector<std::uint32_t>{0, 1}),
         "artifact source was not consumed incrementally");
  expect(streamed.writes.size() == 4,
         "streamed artifact emitted the wrong message count");
  expect(streamed.writes[1].artifact_chunk().data() == "a" &&
             streamed.writes[2].artifact_chunk().data() == "b",
         "streamed artifact changed chunk order or contents");
}

void parent_liveness_watcher_is_owned_and_interruptible() {
#ifdef _WIN32
  int parent_pipe[2];
  expect(::_pipe(parent_pipe, 1024, _O_BINARY) == 0,
         "could not create parent pipe");
#else
  int parent_pipe[2];
  expect(::pipe(parent_pipe) == 0, "could not create parent pipe");
#endif
  std::atomic_int eof_count{};
  {
    onelastleaf::detail::ParentLivenessWatcher watcher{
        [&eof_count] { ++eof_count; }, parent_pipe[0]};
    watcher.stop();
  }
  expect(eof_count.load() == 0,
         "stopping the watcher was mistaken for parent EOF");

#ifdef _WIN32
  ::_close(parent_pipe[0]);
  ::_close(parent_pipe[1]);
  expect(::_pipe(parent_pipe, 1024, _O_BINARY) == 0,
         "could not recreate parent pipe");
#else
  ::close(parent_pipe[0]);
  ::close(parent_pipe[1]);
  expect(::pipe(parent_pipe) == 0, "could not recreate parent pipe");
#endif
  std::promise<void> observed;
  auto observed_result = observed.get_future();
  {
    onelastleaf::detail::ParentLivenessWatcher watcher{
        [&observed] { observed.set_value(); }, parent_pipe[0]};
#ifdef _WIN32
    ::_close(parent_pipe[1]);
#else
    ::close(parent_pipe[1]);
#endif
    expect(observed_result.wait_for(2s) == std::future_status::ready,
           "watcher did not observe parent EOF");
  }
#ifdef _WIN32
  ::_close(parent_pipe[0]);
#else
  ::close(parent_pipe[0]);
#endif
}

void validation_has_single_reusable_sources() {
  expect(onelastleaf::detail::canonical_uuid_v4(
             "aaaaaaaa-aaaa-4aaa-8aaa-aaaaaaaaaaaa"),
         "canonical UUID was rejected");
  expect(!onelastleaf::detail::canonical_uuid_v4(
             "AAAAAAAA-AAAA-4AAA-8AAA-AAAAAAAAAAAA"),
         "non-canonical UUID was accepted");
  expect(onelastleaf::detail::protocol_schema_sha256_bytes().size() == 32,
         "protocol fingerprint did not decode to SHA-256 bytes");
}

void plugin_run_is_one_shot() {
  const char *original = std::getenv("OLL_PLUGIN_ENDPOINT");
  const std::optional<std::string> saved =
      original == nullptr ? std::nullopt : std::optional{std::string{original}};
#ifdef _WIN32
  ::_putenv_s("OLL_PLUGIN_ENDPOINT", "not-an-endpoint");
#else
  ::setenv("OLL_PLUGIN_ENDPOINT", "not-an-endpoint", 1);
#endif
  onelastleaf::Plugin plugin{"org.example.once", "0.1.0"};
  expect_throws<std::invalid_argument>([&] { static_cast<void>(plugin.run()); },
                                       "invalid first run unexpectedly worked");
  expect_throws<std::logic_error>([&] { static_cast<void>(plugin.run()); },
                                  "Plugin accepted a second run");
#ifdef _WIN32
  ::_putenv_s("OLL_PLUGIN_ENDPOINT", saved ? saved->c_str() : "");
#else
  if (saved) {
    ::setenv("OLL_PLUGIN_ENDPOINT", saved->c_str(), 1);
  } else {
    ::unsetenv("OLL_PLUGIN_ENDPOINT");
  }
#endif
}

} // namespace

int main() {
  host_call_cancellation_wakes_the_waiter();
  closing_a_session_wakes_host_calls();
  retained_hosts_fail_after_close();
  retained_hosts_fail_after_their_job_finishes();
  jobs_reap_without_an_inbound_message_and_contain_exceptions();
  empty_and_streamed_artifacts_are_supported();
  parent_liveness_watcher_is_owned_and_interruptible();
  validation_has_single_reusable_sources();
  plugin_run_is_one_shot();
}
