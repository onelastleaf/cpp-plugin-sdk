#include "internal.hpp"
#include "validation.hpp"

#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <stdexcept>
#include <stop_token>
#include <string>
#include <utility>

#include <oll/plugin.pb.h>
#include <openssl/evp.h>

#include <onelastleaf/plugin_sdk.hpp>

namespace onelastleaf {

oll::protocol::ArtifactStored Host::Impl::store_artifact(
    const oll::protocol::TraceContext &trace, std::string job_id,
    oll::protocol::ArtifactDescriptor descriptor, std::uint32_t chunk_count,
    Host::ArtifactChunkSource chunk_source, std::stop_token cancellation) {
  if (!detail::canonical_uuid_v4(job_id)) {
    throw std::invalid_argument("artifact job ID is invalid");
  }
  if (!descriptor.has_artifact_id() ||
      !detail::canonical_uuid_v4(descriptor.artifact_id().value()) ||
      descriptor.file_name().empty() || descriptor.media_type().empty() ||
      descriptor.sha256().size() != 32) {
    throw std::invalid_argument("artifact descriptor is invalid");
  }
  if (chunk_count != 0 && !chunk_source) {
    throw std::invalid_argument("artifact chunk source is required");
  }

  auto digest = std::unique_ptr<EVP_MD_CTX, decltype(&EVP_MD_CTX_free)>{
      EVP_MD_CTX_new(), EVP_MD_CTX_free};
  if (!digest || EVP_DigestInit_ex(digest.get(), EVP_sha256(), nullptr) != 1) {
    throw std::runtime_error("could not initialize artifact SHA-256");
  }

  oll::protocol::PluginEnvelope start_envelope;
  auto *start = start_envelope.mutable_artifact_start();
  start->mutable_job_id()->set_value(std::move(job_id));
  *start->mutable_artifact() = descriptor;
  start->set_chunk_count(chunk_count);
  auto accepted = request(trace, std::move(start_envelope), cancellation);
  if (accepted.payload_case() !=
          oll::protocol::PluginEnvelope::kArtifactAccepted ||
      accepted.artifact_accepted().artifact_id().value() !=
          descriptor.artifact_id().value()) {
    throw std::runtime_error("host did not accept the artifact transfer");
  }

  std::uint64_t size = 0;
  for (std::uint32_t index = 0; index < chunk_count; ++index) {
    if (cancellation.stop_requested()) {
      throw std::runtime_error("plugin job was cancelled");
    }
    auto data = chunk_source(index);
    if (data.empty()) {
      throw std::invalid_argument("artifact chunks must be nonempty");
    }
    if (data.size() > maximum_artifact_chunk_bytes) {
      throw std::invalid_argument(
          "artifact chunk exceeds the negotiated limit");
    }
    if (data.size() > std::numeric_limits<std::uint64_t>::max() - size) {
      throw std::invalid_argument("artifact size overflowed");
    }
    size += static_cast<std::uint64_t>(data.size());
    if (EVP_DigestUpdate(digest.get(), data.data(), data.size()) != 1) {
      throw std::runtime_error("could not hash artifact bytes");
    }

    oll::protocol::PluginEnvelope chunk_envelope;
    auto *chunk = chunk_envelope.mutable_artifact_chunk();
    *chunk->mutable_artifact_id() = descriptor.artifact_id();
    chunk->set_chunk_index(index);
    chunk->set_data(std::move(data));
    send(trace, std::move(chunk_envelope), cancellation);
  }

  const int expected_digest_size = EVP_MD_get_size(EVP_sha256());
  if (expected_digest_size <= 0) {
    throw std::runtime_error("could not determine artifact SHA-256 size");
  }
  std::string sha256(static_cast<std::size_t>(expected_digest_size), '\0');
  unsigned int digest_size = 0;
  if (EVP_DigestFinal_ex(digest.get(),
                         reinterpret_cast<unsigned char *>(sha256.data()),
                         &digest_size) != 1) {
    throw std::runtime_error("could not finish artifact SHA-256");
  }
  sha256.resize(digest_size);
  if (descriptor.size_bytes() != size || descriptor.sha256() != sha256) {
    throw std::invalid_argument(
        "artifact size or SHA-256 does not match its bytes");
  }

  oll::protocol::PluginEnvelope complete_envelope;
  *complete_envelope.mutable_artifact_complete()->mutable_artifact_id() =
      descriptor.artifact_id();
  auto stored = request(trace, std::move(complete_envelope), cancellation);
  if (stored.payload_case() != oll::protocol::PluginEnvelope::kArtifactStored ||
      stored.artifact_stored().artifact_id().value() !=
          descriptor.artifact_id().value()) {
    throw std::runtime_error("host did not acknowledge the stored artifact");
  }
  return stored.artifact_stored();
}

} // namespace onelastleaf
