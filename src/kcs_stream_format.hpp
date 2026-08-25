#ifndef KCS_STREAM_FORMAT_HPP
#define KCS_STREAM_FORMAT_HPP

#include "kcs_index_format.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <stdexcept>

namespace kcsstream {

constexpr uint32_t kStreamVersion = 2;
constexpr uint64_t kStreamFlagsRadiusMajorDescending = 1;
constexpr uint64_t kStreamHeaderBytes = 192;
constexpr uint64_t kStreamFooterBytes = 48;
constexpr uint64_t kStreamSeedEntryBytes = 4;
constexpr uint64_t kStreamGroupHeaderBytes = 16;
constexpr uint64_t kStreamEventBytes = 8;
constexpr char kStreamMagic[8] = {'K', 'C', 'S', 'S', 'T', 'R', 'M', '2'};
constexpr char kStreamFooterMagic[8] = {'K', 'C', 'S', 'E', 'N', 'D', '2', 'S'};

struct StreamHeader {
  uint64_t edges{};
  uint64_t groups{};
  uint64_t records{};
  int64_t floor{};
  int64_t dmax{};
  uint64_t seed_offset{};
  uint64_t seed_bytes{};
  uint64_t body_offset{};
  uint64_t body_bytes{};
  uint64_t footer_offset{};
  uint64_t file_bytes{};
  std::array<uint8_t, 32> graph_sha256{};
  uint64_t seed_crc{};
  uint64_t body_crc{};
  uint64_t header_crc{};
};

struct StreamGroup {
  int64_t delta_below{};
  uint64_t count{};
};

struct StreamEvent {
  uint32_t edge{};
  uint32_t value_below{};
};

inline std::array<uint8_t, kStreamHeaderBytes>
encode_stream_header(const StreamHeader &h) {
  std::array<uint8_t, kStreamHeaderBytes> b{};
  std::memcpy(b.data(), kStreamMagic, 8);
  kcsidx::put_u32(b.data(), 8, kStreamVersion);
  kcsidx::put_u32(b.data(), 12, kcsidx::kEndianMarker);
  kcsidx::put_u64(b.data(), 16, kStreamHeaderBytes);
  kcsidx::put_u64(b.data(), 24, kStreamFlagsRadiusMajorDescending);
  kcsidx::put_u64(b.data(), 32, h.edges);
  kcsidx::put_u64(b.data(), 40, h.groups);
  kcsidx::put_u64(b.data(), 48, h.records);
  kcsidx::put_i64(b.data(), 56, h.floor);
  kcsidx::put_i64(b.data(), 64, h.dmax);
  kcsidx::put_u64(b.data(), 72, h.seed_offset);
  kcsidx::put_u64(b.data(), 80, h.seed_bytes);
  kcsidx::put_u64(b.data(), 88, h.body_offset);
  kcsidx::put_u64(b.data(), 96, h.body_bytes);
  kcsidx::put_u64(b.data(), 104, h.footer_offset);
  kcsidx::put_u64(b.data(), 112, h.file_bytes);
  std::copy(h.graph_sha256.begin(), h.graph_sha256.end(), b.begin() + 120);
  kcsidx::put_u64(b.data(), 152, h.seed_crc);
  kcsidx::put_u64(b.data(), 160, h.body_crc);
  kcsidx::Crc64 crc;
  crc.update(b.data(), 168);
  kcsidx::put_u64(b.data(), 168, crc.value());
  return b;
}

inline StreamHeader decode_stream_header(const uint8_t *b,
                                         uint64_t actual_size) {
  if (actual_size < kStreamHeaderBytes || std::memcmp(b, kStreamMagic, 8) != 0)
    throw std::runtime_error("not a KCSSTRM2 file");
  if (kcsidx::get_u32(b + 8) != kStreamVersion ||
      kcsidx::get_u32(b + 12) != kcsidx::kEndianMarker ||
      kcsidx::get_u64(b + 16) != kStreamHeaderBytes ||
      kcsidx::get_u64(b + 24) != kStreamFlagsRadiusMajorDescending)
    throw std::runtime_error("unsupported KCSSTRM2 version/layout");
  kcsidx::Crc64 header_crc;
  header_crc.update(b, 168);
  if (header_crc.value() != kcsidx::get_u64(b + 168))
    throw std::runtime_error("KCSSTRM2 header checksum mismatch");
  // Reserved bytes are outside the historical v2 CRC range.  Requiring their
  // canonical zero encoding detects any mutation without silently changing the
  // checksum semantics of already-written KCSSTRM2 files.
  if (std::any_of(b + 176, b + kStreamHeaderBytes,
                  [](uint8_t value) { return value != 0; }))
    throw std::runtime_error("KCSSTRM2 reserved header bytes are nonzero");

  StreamHeader h;
  h.edges = kcsidx::get_u64(b + 32);
  h.groups = kcsidx::get_u64(b + 40);
  h.records = kcsidx::get_u64(b + 48);
  h.floor = kcsidx::get_i64(b + 56);
  h.dmax = kcsidx::get_i64(b + 64);
  h.seed_offset = kcsidx::get_u64(b + 72);
  h.seed_bytes = kcsidx::get_u64(b + 80);
  h.body_offset = kcsidx::get_u64(b + 88);
  h.body_bytes = kcsidx::get_u64(b + 96);
  h.footer_offset = kcsidx::get_u64(b + 104);
  h.file_bytes = kcsidx::get_u64(b + 112);
  std::copy(b + 120, b + 152, h.graph_sha256.begin());
  h.seed_crc = kcsidx::get_u64(b + 152);
  h.body_crc = kcsidx::get_u64(b + 160);
  h.header_crc = kcsidx::get_u64(b + 168);

  if (h.edges == 0 || h.edges > UINT32_MAX || h.floor < 0 || h.dmax < h.floor ||
      h.seed_offset != kStreamHeaderBytes ||
      h.seed_bytes != kcsidx::checked_mul(h.edges, kStreamSeedEntryBytes,
                                          "stream seed bytes") ||
      h.body_offset != kcsidx::checked_add(h.seed_offset, h.seed_bytes,
                                           "stream body offset") ||
      h.footer_offset != kcsidx::checked_add(h.body_offset, h.body_bytes,
                                             "stream footer offset") ||
      h.file_bytes != kcsidx::checked_add(h.footer_offset, kStreamFooterBytes,
                                          "stream file bytes") ||
      h.file_bytes != actual_size)
    throw std::runtime_error("invalid KCSSTRM2 section layout or file size");
  return h;
}

inline std::array<uint8_t, kStreamGroupHeaderBytes>
encode_stream_group(const StreamGroup &g) {
  std::array<uint8_t, kStreamGroupHeaderBytes> b{};
  kcsidx::put_i64(b.data(), 0, g.delta_below);
  kcsidx::put_u64(b.data(), 8, g.count);
  return b;
}

inline StreamGroup decode_stream_group(const uint8_t *b) {
  return {kcsidx::get_i64(b), kcsidx::get_u64(b + 8)};
}

inline std::array<uint8_t, kStreamEventBytes>
encode_stream_event(const StreamEvent &e) {
  std::array<uint8_t, kStreamEventBytes> b{};
  kcsidx::put_u32(b.data(), 0, e.edge);
  kcsidx::put_u32(b.data(), 4, e.value_below);
  return b;
}

inline StreamEvent decode_stream_event(const uint8_t *b) {
  return {kcsidx::get_u32(b), kcsidx::get_u32(b + 4)};
}

inline std::array<uint8_t, kStreamFooterBytes>
encode_stream_footer(const StreamHeader &h) {
  std::array<uint8_t, kStreamFooterBytes> b{};
  std::memcpy(b.data(), kStreamFooterMagic, 8);
  kcsidx::put_u64(b.data(), 8, h.groups);
  kcsidx::put_u64(b.data(), 16, h.records);
  kcsidx::put_u64(b.data(), 24, h.seed_crc);
  kcsidx::put_u64(b.data(), 32, h.body_crc);
  kcsidx::put_u64(b.data(), 40, h.header_crc);
  return b;
}

inline void validate_stream_footer(const uint8_t *b, const StreamHeader &h) {
  if (std::memcmp(b, kStreamFooterMagic, 8) != 0 ||
      kcsidx::get_u64(b + 8) != h.groups ||
      kcsidx::get_u64(b + 16) != h.records ||
      kcsidx::get_u64(b + 24) != h.seed_crc ||
      kcsidx::get_u64(b + 32) != h.body_crc ||
      kcsidx::get_u64(b + 40) != h.header_crc)
    throw std::runtime_error("invalid KCSSTRM2 footer");
}

} // namespace kcsstream

#endif
