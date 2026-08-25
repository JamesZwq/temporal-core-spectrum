#ifndef KCS_INDEX_FORMAT_HPP
#define KCS_INDEX_FORMAT_HPP

#include <algorithm>
#include <array>
#include <cerrno>
#include <charconv>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <limits>
#include <stdexcept>
#include <string>
#include <system_error>
#include <vector>

#if defined(_WIN32)
#include <io.h>
#else
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace kcsidx {

using u128 = unsigned __int128;

constexpr uint32_t kVersion = 3;
constexpr uint32_t kEndianMarker = 0x01020304u;
constexpr uint64_t kHeaderBytes = 192;
constexpr uint64_t kDirectoryEntryBytes = 16;
constexpr uint64_t kRecordEntryBytes = 24;
constexpr uint64_t kFooterBytes = 32;
constexpr uint64_t kFlagPrefixArea96 = 1;
constexpr uint32_t kFingerprintSha256 = 1;
constexpr uint32_t kChecksumCrc64Ecma = 1;
constexpr char kMagic[8] = {'K', 'C', 'S', 'I', 'D', 'X', '3', '\0'};
constexpr char kFooterMagic[8] = {'K', 'C', 'S', 'E', 'N', 'D', '3', '\0'};

inline uint64_t checked_add(uint64_t a, uint64_t b, const char *what) {
  if (b > std::numeric_limits<uint64_t>::max() - a) {
    throw std::overflow_error(std::string(what) + " addition overflow");
  }
  return a + b;
}

inline uint64_t checked_mul(uint64_t a, uint64_t b, const char *what) {
  if (a != 0 && b > std::numeric_limits<uint64_t>::max() / a) {
    throw std::overflow_error(std::string(what) + " multiplication overflow");
  }
  return a * b;
}

inline u128 checked_area_add(u128 area, uint32_t value, uint64_t width) {
  const u128 add = static_cast<u128>(value) * static_cast<u128>(width);
  const u128 maxv = ~static_cast<u128>(0);
  if (add > maxv - area)
    throw std::overflow_error("128-bit prefix area overflow");
  return area + add;
}

inline std::string u128_string(u128 value) {
  if (value == 0)
    return "0";
  std::string out;
  while (value != 0) {
    out.push_back(static_cast<char>('0' + static_cast<unsigned>(value % 10)));
    value /= 10;
  }
  std::reverse(out.begin(), out.end());
  return out;
}

inline long double u128_long_double(u128 value) {
  const uint64_t lo = static_cast<uint64_t>(value);
  const uint64_t hi = static_cast<uint64_t>(value >> 64);
  return static_cast<long double>(hi) * 18446744073709551616.0L +
         static_cast<long double>(lo);
}

inline void put_u32(uint8_t *b, size_t off, uint32_t v) {
  for (unsigned i = 0; i < 4; ++i)
    b[off + i] = static_cast<uint8_t>(v >> (8 * i));
}

inline void put_u64(uint8_t *b, size_t off, uint64_t v) {
  for (unsigned i = 0; i < 8; ++i)
    b[off + i] = static_cast<uint8_t>(v >> (8 * i));
}

inline uint32_t get_u32(const uint8_t *p) {
  uint32_t v = 0;
  for (unsigned i = 0; i < 4; ++i)
    v |= static_cast<uint32_t>(p[i]) << (8 * i);
  return v;
}

inline uint64_t get_u64(const uint8_t *p) {
  uint64_t v = 0;
  for (unsigned i = 0; i < 8; ++i)
    v |= static_cast<uint64_t>(p[i]) << (8 * i);
  return v;
}

inline int64_t get_i64(const uint8_t *p) {
  const uint64_t u = get_u64(p);
  int64_t v = 0;
  std::memcpy(&v, &u, sizeof(v));
  return v;
}

inline void put_i64(uint8_t *b, size_t off, int64_t v) {
  uint64_t u = 0;
  std::memcpy(&u, &v, sizeof(u));
  put_u64(b, off, u);
}

class Crc64 {
public:
  void update(const void *data, size_t n) {
    const auto *p = static_cast<const uint8_t *>(data);
    for (size_t i = 0; i < n; ++i) {
      const uint8_t index = static_cast<uint8_t>((value_ >> 56) ^ p[i]);
      value_ = table()[index] ^ (value_ << 8);
    }
  }
  uint64_t value() const { return value_; }

private:
  static const std::array<uint64_t, 256> &table() {
    static const std::array<uint64_t, 256> values = [] {
      std::array<uint64_t, 256> t{};
      for (size_t i = 0; i < t.size(); ++i) {
        uint64_t x = static_cast<uint64_t>(i) << 56;
        for (unsigned bit = 0; bit < 8; ++bit)
          x = (x & (uint64_t{1} << 63))
                  ? (x << 1) ^ UINT64_C(0x42f0e1eba9ea3693)
                  : x << 1;
        t[i] = x;
      }
      return t;
    }();
    return values;
  }
  uint64_t value_ = 0;
};

class Sha256 {
public:
  void update(const void *data, size_t n) {
    const auto *p = static_cast<const uint8_t *>(data);
    bit_count_ += static_cast<uint64_t>(n) * 8;
    while (n != 0) {
      const size_t take = std::min(n, block_.size() - used_);
      std::memcpy(block_.data() + used_, p, take);
      used_ += take;
      p += take;
      n -= take;
      if (used_ == block_.size()) {
        transform(block_.data());
        used_ = 0;
      }
    }
  }

  std::array<uint8_t, 32> finish() {
    block_[used_++] = 0x80;
    if (used_ > 56) {
      std::fill(block_.begin() + static_cast<std::ptrdiff_t>(used_),
                block_.end(), 0);
      transform(block_.data());
      used_ = 0;
    }
    std::fill(block_.begin() + static_cast<std::ptrdiff_t>(used_),
              block_.begin() + 56, 0);
    for (unsigned i = 0; i < 8; ++i)
      block_[63 - i] = static_cast<uint8_t>(bit_count_ >> (8 * i));
    transform(block_.data());
    std::array<uint8_t, 32> out{};
    for (size_t i = 0; i < state_.size(); ++i) {
      out[4 * i] = static_cast<uint8_t>(state_[i] >> 24);
      out[4 * i + 1] = static_cast<uint8_t>(state_[i] >> 16);
      out[4 * i + 2] = static_cast<uint8_t>(state_[i] >> 8);
      out[4 * i + 3] = static_cast<uint8_t>(state_[i]);
    }
    return out;
  }

private:
  static uint32_t rotr(uint32_t x, unsigned n) {
    return (x >> n) | (x << (32 - n));
  }
  void transform(const uint8_t *p) {
    static constexpr uint32_t k[64] = {
        0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1,
        0x923f82a4, 0xab1c5ed5, 0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3,
        0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174, 0xe49b69c1, 0xefbe4786,
        0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
        0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147,
        0x06ca6351, 0x14292967, 0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13,
        0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85, 0xa2bfe8a1, 0xa81a664b,
        0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
        0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a,
        0x5b9cca4f, 0x682e6ff3, 0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208,
        0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};
    uint32_t w[64]{};
    for (unsigned i = 0; i < 16; ++i) {
      const size_t off = size_t{4} * static_cast<size_t>(i);
      w[i] = (static_cast<uint32_t>(p[off]) << 24) |
             (static_cast<uint32_t>(p[off + 1]) << 16) |
             (static_cast<uint32_t>(p[off + 2]) << 8) | p[off + 3];
    }
    for (unsigned i = 16; i < 64; ++i) {
      const uint32_t s0 =
          rotr(w[i - 15], 7) ^ rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
      const uint32_t s1 =
          rotr(w[i - 2], 17) ^ rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
      w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    uint32_t a = state_[0], b = state_[1], c = state_[2], d = state_[3];
    uint32_t e = state_[4], f = state_[5], g = state_[6], h = state_[7];
    for (unsigned i = 0; i < 64; ++i) {
      const uint32_t s1 = rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25);
      const uint32_t ch = (e & f) ^ ((~e) & g);
      const uint32_t t1 = h + s1 + ch + k[i] + w[i];
      const uint32_t s0 = rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22);
      const uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
      const uint32_t t2 = s0 + maj;
      h = g;
      g = f;
      f = e;
      e = d + t1;
      d = c;
      c = b;
      b = a;
      a = t1 + t2;
    }
    state_[0] += a;
    state_[1] += b;
    state_[2] += c;
    state_[3] += d;
    state_[4] += e;
    state_[5] += f;
    state_[6] += g;
    state_[7] += h;
  }
  std::array<uint32_t, 8> state_{{0x6a09e667, 0xbb67ae85, 0x3c6ef372,
                                  0xa54ff53a, 0x510e527f, 0x9b05688c,
                                  0x1f83d9ab, 0x5be0cd19}};
  std::array<uint8_t, 64> block_{};
  size_t used_ = 0;
  uint64_t bit_count_ = 0;
};

struct TextRecord {
  int64_t delta{};
  uint32_t value{};
};
struct TextEdge {
  int64_t u{}, v{}, t{};
  uint32_t base{};
  std::vector<TextRecord> records;
};
struct TextHeader {
  int64_t floor{}, dmax{};
};

class Cursor {
public:
  explicit Cursor(const std::string &s)
      : begin_(s.data()), p_(s.data()), end_(s.data() + s.size()) {}
  void ws() {
    while (p_ != end_ && (*p_ == ' ' || *p_ == '\t' || *p_ == '\r'))
      ++p_;
  }
  bool done() {
    ws();
    return p_ == end_;
  }
  void token(const char *s) {
    ws();
    const size_t n = std::strlen(s);
    if (static_cast<size_t>(end_ - p_) < n || std::memcmp(p_, s, n) != 0)
      fail(std::string("expected '") + s + "'");
    p_ += n;
  }
  int64_t i64() {
    ws();
    int64_t v{};
    auto r = std::from_chars(p_, end_, v);
    if (r.ec != std::errc{} || r.ptr == p_)
      fail("expected signed 64-bit integer");
    p_ = r.ptr;
    return v;
  }
  uint32_t u32() {
    ws();
    uint64_t v{};
    auto r = std::from_chars(p_, end_, v);
    if (r.ec != std::errc{} || r.ptr == p_ ||
        v > std::numeric_limits<uint32_t>::max())
      fail("expected unsigned 32-bit integer");
    p_ = r.ptr;
    return static_cast<uint32_t>(v);
  }

private:
  [[noreturn]] void fail(const std::string &why) const {
    throw std::runtime_error(
        why + " at column " +
        std::to_string(static_cast<size_t>(p_ - begin_) + 1));
  }
  const char *begin_;
  const char *p_;
  const char *end_;
};

inline TextHeader parse_text_header(const std::string &line) {
  static const std::string prefix =
      "# (k,Delta)-core spectrum index  |  Delta in [";
  if (line.rfind(prefix, 0) != 0)
    throw std::runtime_error("invalid text index header");
  const size_t lb = prefix.size() - 1;
  const auto comma = line.find(',', lb), rb = line.find(']', comma);
  if (comma == std::string::npos || rb == std::string::npos)
    throw std::runtime_error("invalid text index header");
  const std::string lo_text = line.substr(lb + 1, comma - lb - 1);
  const std::string hi_text = line.substr(comma + 1, rb - comma - 1);
  Cursor lo(lo_text), hi(hi_text);
  TextHeader h{lo.i64(), hi.i64()};
  const std::string tail_text = line.substr(rb + 1);
  Cursor tail(tail_text);
  if (!lo.done() || !hi.done() || !tail.done() || h.floor < 0 ||
      h.dmax < h.floor)
    throw std::runtime_error("invalid Delta range");
  return h;
}

inline TextEdge parse_text_edge(const std::string &line, const TextHeader &h) {
  Cursor c(line);
  TextEdge e;
  e.u = c.i64();
  e.v = c.i64();
  e.t = c.i64();
  c.token("|");
  c.token("base=");
  e.base = c.u32();
  c.token(";");
  int64_t prev_d = h.floor;
  uint32_t prev_v = e.base;
  while (!c.done()) {
    const int64_t d = c.i64();
    c.token(":");
    const uint32_t v = c.u32();
    if (d <= prev_d || d > h.dmax)
      throw std::runtime_error("breakpoint Delta is outside (floor,Dmax] or "
                               "not strictly increasing");
    if (v <= prev_v)
      throw std::runtime_error("breakpoint value is not strictly increasing");
    e.records.push_back(TextRecord{d, v});
    prev_d = d;
    prev_v = v;
  }
  return e;
}

inline void hash_graph_edge(Sha256 &sha, uint64_t ordinal, int64_t u,
                            int64_t v, int64_t t) {
  std::array<uint8_t, 32> b{};
  put_u64(b.data(), 0, ordinal);
  put_i64(b.data(), 8, u);
  put_i64(b.data(), 16, v);
  put_i64(b.data(), 24, t);
  sha.update(b.data(), b.size());
}

inline void hash_edge(Sha256 &sha, uint64_t ordinal, const TextEdge &e) {
  hash_graph_edge(sha, ordinal, e.u, e.v, e.t);
}

struct Header {
  uint64_t edges{}, records{};
  int64_t floor{}, dmax{};
  uint64_t directory_offset{}, directory_bytes{}, records_offset{},
      records_bytes{}, footer_offset{}, file_bytes{};
  std::array<uint8_t, 32> graph_sha256{};
  uint64_t directory_crc{}, records_crc{}, header_crc{};
};

inline std::array<uint8_t, kHeaderBytes> encode_header(const Header &h) {
  std::array<uint8_t, kHeaderBytes> b{};
  std::memcpy(b.data(), kMagic, 8);
  put_u32(b.data(), 8, kVersion);
  put_u32(b.data(), 12, kEndianMarker);
  put_u64(b.data(), 16, kHeaderBytes);
  put_u64(b.data(), 24, kFlagPrefixArea96);
  put_u64(b.data(), 32, h.edges);
  put_u64(b.data(), 40, h.records);
  put_i64(b.data(), 48, h.floor);
  put_i64(b.data(), 56, h.dmax);
  put_u64(b.data(), 64, h.directory_offset);
  put_u64(b.data(), 72, h.directory_bytes);
  put_u64(b.data(), 80, h.records_offset);
  put_u64(b.data(), 88, h.records_bytes);
  put_u64(b.data(), 96, h.footer_offset);
  put_u64(b.data(), 104, h.file_bytes);
  put_u32(b.data(), 112, static_cast<uint32_t>(kDirectoryEntryBytes));
  put_u32(b.data(), 116, static_cast<uint32_t>(kRecordEntryBytes));
  put_u32(b.data(), 120, kFingerprintSha256);
  put_u32(b.data(), 124, kChecksumCrc64Ecma);
  std::copy(h.graph_sha256.begin(), h.graph_sha256.end(), b.begin() + 128);
  put_u64(b.data(), 160, h.directory_crc);
  put_u64(b.data(), 168, h.records_crc);
  Crc64 crc;
  crc.update(b.data(), 184);
  put_u64(b.data(), 184, crc.value());
  return b;
}

inline Header decode_header(const uint8_t *b, size_t size,
                            uint64_t actual_size) {
  if (size != kHeaderBytes || std::memcmp(b, kMagic, 8) != 0)
    throw std::runtime_error("not a KCSIDX3 file");
  if (get_u32(b + 8) != kVersion || get_u32(b + 12) != kEndianMarker ||
      get_u64(b + 16) != kHeaderBytes || get_u64(b + 24) != kFlagPrefixArea96 ||
      get_u32(b + 112) != kDirectoryEntryBytes ||
      get_u32(b + 116) != kRecordEntryBytes ||
      get_u32(b + 120) != kFingerprintSha256 ||
      get_u32(b + 124) != kChecksumCrc64Ecma)
    throw std::runtime_error("unsupported KCSIDX3 version/layout");
  Crc64 crc;
  crc.update(b, 184);
  if (crc.value() != get_u64(b + 184))
    throw std::runtime_error("KCSIDX3 header checksum mismatch");
  Header h;
  h.edges = get_u64(b + 32);
  h.records = get_u64(b + 40);
  h.floor = get_i64(b + 48);
  h.dmax = get_i64(b + 56);
  h.directory_offset = get_u64(b + 64);
  h.directory_bytes = get_u64(b + 72);
  h.records_offset = get_u64(b + 80);
  h.records_bytes = get_u64(b + 88);
  h.footer_offset = get_u64(b + 96);
  h.file_bytes = get_u64(b + 104);
  std::copy(b + 128, b + 160, h.graph_sha256.begin());
  h.directory_crc = get_u64(b + 160);
  h.records_crc = get_u64(b + 168);
  h.header_crc = get_u64(b + 184);
  if (h.floor < 0 || h.dmax < h.floor || h.directory_offset != kHeaderBytes ||
      h.directory_bytes !=
          checked_mul(h.edges, kDirectoryEntryBytes, "directory bytes") ||
      h.records_offset != checked_add(h.directory_offset, h.directory_bytes,
                                      "records offset") ||
      h.records_bytes !=
          checked_mul(h.records, kRecordEntryBytes, "record bytes") ||
      h.footer_offset !=
          checked_add(h.records_offset, h.records_bytes, "footer offset") ||
      h.file_bytes !=
          checked_add(h.footer_offset, kFooterBytes, "file bytes") ||
      h.file_bytes != actual_size)
    throw std::runtime_error("invalid KCSIDX3 section layout or file size");
  return h;
}

struct DirectoryEntry {
  uint64_t first{};
  uint32_t count{}, base{};
};
struct RecordEntry {
  int64_t delta{};
  uint32_t value{};
  u128 prefix{};
};

inline std::array<uint8_t, kDirectoryEntryBytes>
encode_directory(const DirectoryEntry &d) {
  std::array<uint8_t, kDirectoryEntryBytes> b{};
  put_u64(b.data(), 0, d.first);
  put_u32(b.data(), 8, d.count);
  put_u32(b.data(), 12, d.base);
  return b;
}
inline DirectoryEntry decode_directory(const uint8_t *b) {
  return {get_u64(b), get_u32(b + 8), get_u32(b + 12)};
}
inline std::array<uint8_t, kRecordEntryBytes>
encode_record(const RecordEntry &r) {
  // Valid disjoint segment widths sum to at most 2^63-1, hence even a
  // uint32 value gives prefix < (2^32)(2^63) = 2^95.  We still check the
  // serialized uint96 boundary so malformed callers cannot truncate u128.
  if ((r.prefix >> 96) != 0)
    throw std::overflow_error("KCSIDX3 prefix area exceeds uint96");
  std::array<uint8_t, kRecordEntryBytes> b{};
  put_i64(b.data(), 0, r.delta);
  put_u32(b.data(), 8, r.value);
  put_u64(b.data(), 12, static_cast<uint64_t>(r.prefix));
  put_u32(b.data(), 20, static_cast<uint32_t>(r.prefix >> 64));
  return b;
}
inline RecordEntry decode_record(const uint8_t *b) {
  return {get_i64(b), get_u32(b + 8),
          static_cast<u128>(get_u64(b + 12)) |
              (static_cast<u128>(get_u32(b + 20)) << 64)};
}

inline void checked_write(FILE *f, const void *data, size_t n, const char *what,
                          Crc64 *crc = nullptr) {
  if (n != 0 && std::fwrite(data, 1, n, f) != n)
    throw std::runtime_error(std::string("write failed for ") + what + ": " +
                             std::strerror(errno));
  if (crc)
    crc->update(data, n);
}
inline void checked_read(FILE *f, void *data, size_t n, const char *what) {
  if (n != 0 && std::fread(data, 1, n, f) != n)
    throw std::runtime_error(std::string("short read for ") + what);
}
inline void checked_seek(FILE *f, uint64_t off, const char *what) {
  if (off > static_cast<uint64_t>(std::numeric_limits<int64_t>::max()) ||
#if defined(_WIN32)
      _fseeki64(f, static_cast<int64_t>(off), SEEK_SET) != 0)
#else
      fseeko(f, static_cast<off_t>(off), SEEK_SET) != 0)
#endif
    throw std::runtime_error(std::string("seek failed for ") + what);
}
inline void checked_close(FILE *&f, const char *what, bool sync = true) {
  if (!f)
    return;
  if (std::fflush(f) != 0)
    throw std::runtime_error(std::string("flush failed for ") + what);
  if (sync) {
#if defined(_WIN32)
    if (_commit(_fileno(f)) != 0)
      throw std::runtime_error(std::string("sync failed for ") + what);
#else
    if (::fsync(fileno(f)) != 0)
      throw std::runtime_error(std::string("sync failed for ") + what);
#endif
  }
  if (std::fclose(f) != 0) {
    f = nullptr;
    throw std::runtime_error(std::string("close failed for ") + what);
  }
  f = nullptr;
}

class BinaryIndex {
public:
  explicit BinaryIndex(const std::string &path) {
#if defined(_WIN32)
    std::error_code ec;
    const auto sz = std::filesystem::file_size(path, ec);
    if (ec)
      throw std::runtime_error("cannot stat index: " + ec.message());
    f_ = std::fopen(path.c_str(), "rb");
    if (!f_)
      throw std::runtime_error("cannot open index: " +
                               std::string(std::strerror(errno)));
    try {
      std::array<uint8_t, kHeaderBytes> b{};
      checked_read(f_, b.data(), b.size(), "header");
      h_ = decode_header(b.data(), b.size(), sz);
      std::array<uint8_t, kFooterBytes> footer{};
      checked_seek(f_, h_.footer_offset, "footer");
      checked_read(f_, footer.data(), footer.size(), "footer");
      validate_footer(footer.data());
    } catch (...) {
      std::fclose(f_);
      f_ = nullptr;
      throw;
    }
#else
    fd_ = ::open(path.c_str(), O_RDONLY);
    if (fd_ < 0)
      throw std::runtime_error("cannot open index: " +
                               std::string(std::strerror(errno)));
    try {
      struct stat st{};
      if (fstat(fd_, &st) != 0 || st.st_size < 0)
        throw std::runtime_error("cannot stat index");
      file_size_ = static_cast<uint64_t>(st.st_size);
      if (file_size_ < kHeaderBytes ||
          file_size_ >
              static_cast<uint64_t>(std::numeric_limits<size_t>::max()))
        throw std::runtime_error(
            "index size is not addressable on this platform");
      void *mapped = mmap(nullptr, static_cast<size_t>(file_size_), PROT_READ,
                          MAP_PRIVATE, fd_, 0);
      if (mapped == MAP_FAILED)
        throw std::runtime_error("cannot mmap index: " +
                                 std::string(std::strerror(errno)));
      map_ = static_cast<const uint8_t *>(mapped);
      h_ = decode_header(map_, kHeaderBytes, file_size_);
      validate_footer(at(h_.footer_offset, kFooterBytes, "footer"));
    } catch (...) {
      if (map_)
        munmap(const_cast<uint8_t *>(map_), static_cast<size_t>(file_size_));
      ::close(fd_);
      map_ = nullptr;
      fd_ = -1;
      throw;
    }
#endif
  }
  ~BinaryIndex() {
#if defined(_WIN32)
    if (f_)
      std::fclose(f_);
#else
    if (map_)
      munmap(const_cast<uint8_t *>(map_), static_cast<size_t>(file_size_));
    if (fd_ >= 0)
      ::close(fd_);
#endif
  }
  BinaryIndex(const BinaryIndex &) = delete;
  BinaryIndex &operator=(const BinaryIndex &) = delete;
  const Header &header() const { return h_; }
  DirectoryEntry directory(uint64_t edge) const {
    if (edge >= h_.edges)
      throw std::out_of_range("edge id out of range");
    const uint64_t off =
        checked_add(h_.directory_offset,
                    checked_mul(edge, kDirectoryEntryBytes, "directory offset"),
                    "directory offset");
#if defined(_WIN32)
    std::array<uint8_t, kDirectoryEntryBytes> b{};
    checked_seek(f_, off, "directory entry");
    checked_read(f_, b.data(), b.size(), "directory entry");
    auto d = decode_directory(b.data());
#else
    auto d = decode_directory(at(off, kDirectoryEntryBytes, "directory entry"));
#endif
    if (d.first > h_.records || d.count > h_.records - d.first)
      throw std::runtime_error("directory range outside record section");
    return d;
  }
  RecordEntry record(uint64_t index) const {
    if (index >= h_.records)
      throw std::out_of_range("record index out of range");
    const uint64_t off =
        checked_add(h_.records_offset,
                    checked_mul(index, kRecordEntryBytes, "record offset"),
                    "record offset");
#if defined(_WIN32)
    std::array<uint8_t, kRecordEntryBytes> b{};
    checked_seek(f_, off, "record");
    checked_read(f_, b.data(), b.size(), "record");
    return decode_record(b.data());
#else
    return decode_record(at(off, kRecordEntryBytes, "record"));
#endif
  }
  uint32_t point(uint64_t edge, int64_t delta) const {
    if (delta < h_.floor || delta > h_.dmax)
      throw std::out_of_range("query Delta outside index domain");
    const auto d = directory(edge);
    uint64_t lo = 0, hi = d.count;
    while (lo < hi) {
      const uint64_t mid = lo + (hi - lo) / 2;
      if (record(d.first + mid).delta <= delta)
        lo = mid + 1;
      else
        hi = mid;
    }
    return lo == 0 ? d.base : record(d.first + lo - 1).value;
  }
  u128 area_to(uint64_t edge, int64_t delta) const {
    if (delta < h_.floor || delta > h_.dmax)
      throw std::out_of_range("integral bound outside index domain");
    const auto d = directory(edge);
    uint64_t lo = 0, hi = d.count;
    while (lo < hi) {
      const uint64_t mid = lo + (hi - lo) / 2;
      if (record(d.first + mid).delta <= delta)
        lo = mid + 1;
      else
        hi = mid;
    }
    if (lo == 0)
      return checked_area_add(0, d.base,
                              static_cast<uint64_t>(delta - h_.floor));
    const auto r = record(d.first + lo - 1);
    return checked_area_add(r.prefix, r.value,
                            static_cast<uint64_t>(delta - r.delta));
  }
  u128 integral(uint64_t edge, int64_t a, int64_t b) const {
    if (a > b)
      throw std::invalid_argument("integral requires a <= b");
    const u128 aa = area_to(edge, a), bb = area_to(edge, b);
    if (bb < aa)
      throw std::runtime_error("corrupt decreasing prefix area");
    return bb - aa;
  }
  void verify_sections() const {
    Crc64 dc, rc;
#if defined(_WIN32)
    std::array<uint8_t, 1 << 20> buf{};
    auto scan = [&](uint64_t off, uint64_t n, Crc64 &crc, const char *what) {
      checked_seek(f_, off, what);
      while (n) {
        size_t take = static_cast<size_t>(std::min<uint64_t>(n, buf.size()));
        checked_read(f_, buf.data(), take, what);
        crc.update(buf.data(), take);
        n -= take;
      }
    };
    scan(h_.directory_offset, h_.directory_bytes, dc, "directory section");
    scan(h_.records_offset, h_.records_bytes, rc, "record section");
#else
    dc.update(at(h_.directory_offset, h_.directory_bytes, "directory section"),
              static_cast<size_t>(h_.directory_bytes));
    rc.update(at(h_.records_offset, h_.records_bytes, "record section"),
              static_cast<size_t>(h_.records_bytes));
#endif
    if (dc.value() != h_.directory_crc || rc.value() != h_.records_crc)
      throw std::runtime_error("KCSIDX3 section checksum mismatch");
    uint64_t next = 0;
    for (uint64_t e = 0; e < h_.edges; ++e) {
      auto d = directory(e);
      if (d.first != next)
        throw std::runtime_error("non-contiguous directory");
      int64_t pd = h_.floor;
      uint32_t pv = d.base;
      u128 area = 0;
      for (uint32_t i = 0; i < d.count; ++i) {
        auto r = record(d.first + i);
        if (r.delta <= pd || r.delta > h_.dmax || r.value <= pv)
          throw std::runtime_error("non-monotone record list");
        area = checked_area_add(area, pv, static_cast<uint64_t>(r.delta - pd));
        if (r.prefix != area)
          throw std::runtime_error("bad prefix area");
        pd = r.delta;
        pv = r.value;
      }
      next += d.count;
    }
    if (next != h_.records)
      throw std::runtime_error("directory does not cover record section");
  }

private:
  void validate_footer(const uint8_t *footer) const {
    if (std::memcmp(footer, kFooterMagic, 8) != 0 ||
        get_u64(footer + 8) != h_.directory_crc ||
        get_u64(footer + 16) != h_.records_crc ||
        get_u64(footer + 24) != h_.header_crc)
      throw std::runtime_error("invalid KCSIDX3 footer");
  }
#if defined(_WIN32)
  mutable FILE *f_ = nullptr;
#else
  const uint8_t *at(uint64_t off, uint64_t n, const char *what) const {
    if (off > file_size_ || n > file_size_ - off)
      throw std::runtime_error(std::string(what) + " outside mapped file");
    return map_ + static_cast<size_t>(off);
  }
  int fd_ = -1;
  const uint8_t *map_ = nullptr;
  uint64_t file_size_ = 0;
#endif
  Header h_{};
};

inline bool is_binary_index(const std::string &path) {
  FILE *f = std::fopen(path.c_str(), "rb");
  if (!f)
    return false;
  char b[8]{};
  const size_t n = std::fread(b, 1, 8, f);
  std::fclose(f);
  return n == 8 && std::memcmp(b, kMagic, 8) == 0;
}

inline bool is_legacy_kcsidx2(const std::string &path) {
  static constexpr char legacy_magic[8] = {'K', 'C', 'S', 'I',
                                           'D', 'X', '2', '\0'};
  FILE *f = std::fopen(path.c_str(), "rb");
  if (!f)
    return false;
  char b[8]{};
  const size_t n = std::fread(b, 1, sizeof(b), f);
  std::fclose(f);
  return n == sizeof(b) && std::memcmp(b, legacy_magic, sizeof(b)) == 0;
}

} // namespace kcsidx
#endif
