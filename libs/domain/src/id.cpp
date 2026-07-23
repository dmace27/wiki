#include "kc/domain/id.hpp"

#include <array>
#include <chrono>
#include <cstdint>
#include <mutex>
#include <random>
#include <string>

namespace kc::domain {
namespace {

constexpr std::string_view crockford = "0123456789ABCDEFGHJKMNPQRSTVWXYZ";

std::array<std::uint8_t, 16> make_ulid_bytes() {
  std::array<std::uint8_t, 16> bytes{};
  const auto milliseconds = static_cast<std::uint64_t>(
      std::chrono::duration_cast<std::chrono::milliseconds>(
          std::chrono::system_clock::now().time_since_epoch())
          .count());

  for (std::size_t index = 0; index < 6; ++index) {
    const auto shift = static_cast<unsigned>((5U - static_cast<unsigned>(index)) * 8U);
    bytes[index] = static_cast<std::uint8_t>((milliseconds >> shift) & 0xffU);
  }

  static std::mutex random_mutex;
  static std::random_device random_device;
  static std::mt19937_64 generator(random_device());
  static std::uniform_int_distribution<unsigned> distribution(0U, 255U);
  std::scoped_lock lock(random_mutex);
  for (std::size_t index = 6; index < bytes.size(); ++index) {
    bytes[index] = static_cast<std::uint8_t>(distribution(generator));
  }
  return bytes;
}

std::string encode_ulid(const std::array<std::uint8_t, 16>& bytes) {
  std::string encoded(26, '0');
  std::uint32_t buffer = 0;
  unsigned bits = 2;  // ULIDs encode 128 bits into 130 bits; the first two are zero.
  std::size_t output = 0;

  for (const auto byte : bytes) {
    buffer = static_cast<std::uint32_t>((buffer << 8U) | byte);
    bits += 8U;
    while (bits >= 5U) {
      bits -= 5U;
      encoded[output++] = crockford[(buffer >> bits) & 0x1fU];
    }
  }
  if (bits != 0U) {
    encoded[output++] = crockford[(buffer << (5U - bits)) & 0x1fU];
  }
  encoded.resize(output);
  return encoded;
}

}  // namespace

std::string generate_prefixed_ulid(const std::string_view prefix) {
  return std::string(prefix) + encode_ulid(make_ulid_bytes());
}

ProjectId generate_project_id() { return {generate_prefixed_ulid("prj_")}; }
SourceId generate_source_id() { return {generate_prefixed_ulid("src_")}; }
SourceVersionId generate_source_version_id() { return {generate_prefixed_ulid("ver_")}; }
PageId generate_page_id() { return {generate_prefixed_ulid("pg_")}; }
ArticleId generate_article_id() { return {generate_prefixed_ulid("art_")}; }
ProposalId generate_proposal_id() { return {generate_prefixed_ulid("prp_")}; }
RunId generate_run_id() { return {generate_prefixed_ulid("run_")}; }

}  // namespace kc::domain

