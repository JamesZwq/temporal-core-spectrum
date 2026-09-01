#include "kcs_stream_format.hpp"

#include <chrono>
#include <functional>
#include <iostream>
#include <queue>

using namespace kcsidx;
using namespace kcsstream;

namespace {

uint64_t parse_chunk_cap(const char *text) {
  const char *end = text + std::strlen(text);
  uint64_t value = 0;
  const auto parsed = std::from_chars(text, end, value);
  if (parsed.ec != std::errc{} || parsed.ptr != end)
    throw std::invalid_argument("chunk-records must be an unsigned 64-bit integer");
  if (value == 0)
    throw std::invalid_argument("chunk-records must be positive when specified");
  return value;
}

struct TextInput {
  TextHeader h{};
  std::vector<TextEdge> edges;
  std::array<uint8_t, 32> graph_sha{};
};

TextInput read_text(const std::string &path) {
  std::ifstream in(path, std::ios::binary);
  if (!in)
    throw std::runtime_error("cannot open text index");
  std::string line;
  if (!std::getline(in, line))
    throw std::runtime_error("empty text index");
  if (!line.empty() && line.back() == '\r')
    line.pop_back();
  TextInput out;
  out.h = parse_text_header(line);
  Sha256 sha;
  uint64_t ln = 1;
  while (std::getline(in, line)) {
    ++ln;
    if (!line.empty() && line.back() == '\r')
      line.pop_back();
    if (line.empty() || line[0] == '#')
      continue;
    try {
      auto e = parse_text_edge(line, out.h);
      hash_edge(sha, out.edges.size(), e);
      out.edges.push_back(std::move(e));
    } catch (const std::exception &ex) {
      throw std::runtime_error(path + ":" + std::to_string(ln) + ": " +
                               ex.what());
    }
  }
  if (in.bad())
    throw std::runtime_error("text input I/O error");
  if (out.edges.empty())
    throw std::runtime_error("empty graph");
  out.graph_sha = sha.finish();
  return out;
}

std::filesystem::path norm(const std::filesystem::path &p) {
  std::error_code ec;
  auto a = std::filesystem::absolute(p, ec);
  if (ec)
    throw std::runtime_error("bad path");
  auto parent = std::filesystem::weakly_canonical(a.parent_path(), ec);
  if (ec)
    throw std::runtime_error("bad parent path");
  return parent / a.filename();
}
void reject_alias(const std::string &input, const std::string &output) {
  auto a = norm(input), b = norm(output);
  if (a == b)
    throw std::runtime_error("output aliases input");
  std::error_code ec;
  if (std::filesystem::exists(b, ec) && !ec &&
      std::filesystem::equivalent(a, b, ec) && !ec)
    throw std::runtime_error("output aliases input through link");
}
std::string tag() {
  return ".tmp." + std::to_string(::getpid()) + "." +
         std::to_string(
             std::chrono::steady_clock::now().time_since_epoch().count());
}
FILE *openf(const std::filesystem::path &p, const char *mode) {
  FILE *f = std::fopen(p.string().c_str(), mode);
  if (!f)
    throw std::runtime_error("cannot open " + p.string() + ": " +
                             std::strerror(errno));
  return f;
}
void close_read_stream(FILE *&f, const char *what) {
  FILE *closing = f;
  f = nullptr; // fclose consumes the stream even when it reports EOF/error.
  if (closing && std::fclose(closing) != 0)
    throw std::runtime_error(std::string("close failed for ") + what);
}
FILE *openx(const std::filesystem::path &p) {
  int fd =
      ::open(p.string().c_str(), O_RDWR | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
  if (fd < 0)
    throw std::runtime_error("cannot create exclusive temporary " + p.string() +
                             ": " + std::strerror(errno));
  FILE *f = ::fdopen(fd, "wb+");
  if (!f) {
    int e = errno;
    ::close(fd);
    std::error_code ec;
    std::filesystem::remove(p, ec);
    throw std::runtime_error("cannot attach stream to temporary " + p.string() +
                             ": " + std::strerror(e));
  }
  return f;
}
void sync_parent(const std::filesystem::path &p) {
  int fd = ::open(p.parent_path().string().c_str(), O_RDONLY);
  if (fd < 0)
    throw std::runtime_error("open parent failed");
  int r = ::fsync(fd), e = errno, c = ::close(fd);
  if (r)
    throw std::runtime_error("sync parent failed: " +
                             std::string(std::strerror(e)));
  if (c)
    throw std::runtime_error("close parent failed");
}
struct Cleanup {
  std::vector<std::filesystem::path> p;
  ~Cleanup() {
    std::error_code ec;
    for (const auto &x : p)
      std::filesystem::remove(x, ec);
  }
};

class WorkDirectory {
public:
  explicit WorkDirectory(const std::filesystem::path &output) {
    for (unsigned attempt = 0; attempt < 1000; ++attempt) {
      path_ = std::filesystem::path(output.string() + tag() + ".work." +
                                    std::to_string(attempt));
      if (::mkdir(path_.string().c_str(), 0700) == 0)
        return;
      if (errno != EEXIST)
        throw std::runtime_error("cannot create finalizer work directory: " +
                                 std::string(std::strerror(errno)));
    }
    throw std::runtime_error("cannot allocate a unique finalizer work directory");
  }
  WorkDirectory(const WorkDirectory &) = delete;
  WorkDirectory &operator=(const WorkDirectory &) = delete;
  ~WorkDirectory() {
    std::error_code ec;
    std::filesystem::remove_all(path_, ec);
  }
  std::filesystem::path file(uint64_t id, const char *kind) const {
    return path_ / (std::string(kind) + "." + std::to_string(id));
  }

private:
  std::filesystem::path path_;
};

struct Evt {
  int64_t d{};
  uint32_t e{}, v{};
};

void test_from_text(const std::string &input, const std::string &output) {
  reject_alias(input, output);
  auto text = read_text(input);
  if (text.edges.size() > UINT32_MAX)
    throw std::overflow_error("edge ids exceed uint32");
  std::vector<uint32_t> seed(text.edges.size());
  std::vector<Evt> events;
  for (uint32_t e = 0; e < text.edges.size(); ++e) {
    const auto &x = text.edges[e];
    seed[e] = x.records.empty() ? x.base : x.records.back().value;
    if (seed[e] > text.edges.size())
      throw std::runtime_error("test text coreness exceeds edge-count bound");
    uint32_t below = x.base;
    for (const auto &r : x.records) {
      events.push_back(Evt{r.delta - 1, e, below});
      below = r.value;
    }
  }
  std::sort(events.begin(), events.end(), [](const Evt &a, const Evt &b) {
    return a.d != b.d ? a.d > b.d : a.e < b.e;
  });
  uint64_t groups = 0;
  for (size_t i = 0; i < events.size();) {
    ++groups;
    size_t j = i + 1;
    while (j < events.size() && events[j].d == events[i].d)
      ++j;
    i = j;
  }
  StreamHeader h;
  h.edges = seed.size();
  h.groups = groups;
  h.records = events.size();
  h.floor = text.h.floor;
  h.dmax = text.h.dmax;
  h.seed_offset = kStreamHeaderBytes;
  h.seed_bytes = checked_mul(h.edges, kStreamSeedEntryBytes, "seed bytes");
  h.body_offset = checked_add(h.seed_offset, h.seed_bytes, "body off");
  h.body_bytes = checked_add(
      checked_mul(groups, kStreamGroupHeaderBytes, "group headers"),
      checked_mul(h.records, kStreamEventBytes, "records"), "body bytes");
  h.footer_offset = checked_add(h.body_offset, h.body_bytes, "footer");
  h.file_bytes = checked_add(h.footer_offset, kStreamFooterBytes, "file");
  h.graph_sha256 = text.graph_sha;
  auto out = norm(output);
  auto tmp = std::filesystem::path(out.string() + tag());
  Cleanup clean{{tmp}};
  FILE *f = openx(tmp);
  try {
    std::array<uint8_t, kStreamHeaderBytes> zero{};
    checked_write(f, zero.data(), zero.size(), "placeholder");
    Crc64 sc, bc;
    std::array<uint8_t, 4> sb{};
    for (uint32_t x : seed) {
      put_u32(sb.data(), 0, x);
      checked_write(f, sb.data(), sb.size(), "seed", &sc);
    }
    for (size_t i = 0; i < events.size();) {
      size_t j = i + 1;
      while (j < events.size() && events[j].d == events[i].d)
        ++j;
      const auto gh =
          encode_stream_group({events[i].d, static_cast<uint64_t>(j - i)});
      checked_write(f, gh.data(), gh.size(), "group", &bc);
      for (size_t k = i; k < j; ++k) {
        const auto eb = encode_stream_event({events[k].e, events[k].v});
        checked_write(f, eb.data(), eb.size(), "event", &bc);
      }
      i = j;
    }
    h.seed_crc = sc.value();
    h.body_crc = bc.value();
    auto hb = encode_stream_header(h);
    h.header_crc = get_u64(hb.data() + 168);
    const auto fb = encode_stream_footer(h);
    checked_seek(f, 0, "header");
    checked_write(f, hb.data(), hb.size(), "header");
    checked_seek(f, h.footer_offset, "footer");
    checked_write(f, fb.data(), fb.size(), "footer");
    checked_close(f, "stream output");
    if (std::rename(tmp.string().c_str(), out.string().c_str()))
      throw std::runtime_error("rename stream failed");
    sync_parent(out);
    std::cout << "KCSSTRM2 " << out << " edges=" << h.edges
              << " groups=" << h.groups << " records=" << h.records
              << " bytes=" << h.file_bytes << "\n";
  } catch (...) {
    if (f)
      std::fclose(f);
    throw;
  }
}

class StreamReader {
public:
  explicit StreamReader(const std::string &path) {
    fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0)
      throw std::runtime_error("open stream failed");
    try {
      struct stat st{};
      if (fstat(fd, &st) || st.st_size < 0)
        throw std::runtime_error("stat stream failed");
      size = static_cast<uint64_t>(st.st_size);
      if (size < kStreamHeaderBytes)
        throw std::runtime_error("bad stream size");
      std::array<uint8_t, kStreamHeaderBytes> hb{};
      read_at(0, hb.data(), hb.size());
      h = decode_stream_header(hb.data(), size);
      std::array<uint8_t, kStreamFooterBytes> footer{};
      read_at(h.footer_offset, footer.data(), footer.size());
      validate_stream_footer(footer.data(), h);
    } catch (...) {
      ::close(fd);
      fd = -1;
      throw;
    }
  }
  ~StreamReader() {
    if (fd >= 0)
      ::close(fd);
  }
  template <class Emit>
  std::vector<uint32_t> validate(Emit &&emit) const {
    if (h.edges > SIZE_MAX / sizeof(uint32_t))
      throw std::runtime_error("edge state too large");
    int copy = ::dup(fd);
    if (copy < 0)
      throw std::runtime_error("duplicate stream descriptor failed");
    FILE *f = ::fdopen(copy, "rb");
    if (!f) {
      int e = errno;
      ::close(copy);
      throw std::runtime_error("attach stream reader failed: " +
                               std::string(std::strerror(e)));
    }
    try {
      std::vector<uint32_t> seed(h.edges), last(h.edges);
      Crc64 sc, bc;
      checked_seek(f, h.seed_offset, "seed section");
      std::array<uint8_t, 4> sb{};
      for (uint64_t e = 0; e < h.edges; ++e) {
        checked_read(f, sb.data(), sb.size(), "stream seed");
        sc.update(sb.data(), sb.size());
        seed[e] = last[e] = get_u32(sb.data());
        if (seed[e] > h.edges)
          throw std::runtime_error("seed exceeds edge-count bound");
      }
      if (sc.value() != h.seed_crc)
        throw std::runtime_error("stream seed CRC mismatch");
      checked_seek(f, h.body_offset, "body section");
      std::vector<uint64_t> seen((h.edges + 63) / 64, 0);
      std::vector<uint32_t> touched;
      uint64_t off = h.body_offset, ng = 0, nr = 0;
      int64_t prev = INT64_MAX;
      std::array<uint8_t, kStreamGroupHeaderBytes> gh{};
      std::array<uint8_t, kStreamEventBytes> eb{};
      while (off < h.footer_offset) {
        if (h.footer_offset - off < gh.size())
          throw std::runtime_error("truncated group header");
        checked_read(f, gh.data(), gh.size(), "stream group");
        bc.update(gh.data(), gh.size());
        const StreamGroup group = decode_stream_group(gh.data());
        const int64_t d = group.delta_below;
        const uint64_t k = group.count;
        off += gh.size();
        if (d >= prev || d < h.floor || d >= h.dmax || k == 0 || k > h.edges ||
            k > (h.footer_offset - off) / eb.size())
          throw std::runtime_error("invalid group");
        prev = d;
        touched.clear();
        if (k > touched.capacity())
          touched.reserve(static_cast<size_t>(k));
        for (uint64_t i = 0; i < k; ++i) {
          checked_read(f, eb.data(), eb.size(), "stream event");
          bc.update(eb.data(), eb.size());
          off += eb.size();
          const StreamEvent event = decode_stream_event(eb.data());
          const uint32_t e = event.edge, v = event.value_below;
          if (e >= h.edges)
            throw std::runtime_error("edge id out of range");
          uint64_t bit = UINT64_C(1) << (e & 63);
          if (seen[e >> 6] & bit)
            throw std::runtime_error("duplicate edge in group");
          seen[e >> 6] |= bit;
          touched.push_back(e);
          // Strict descent in uint32 already proves at most seed[e] events:
          // seed-1, ..., 0.  No separate per-edge drop counter is needed.
          if (v >= last[e])
            throw std::runtime_error("invalid per-edge decreasing stream");
          last[e] = v;
          emit(Evt{d, e, v});
        }
        for (uint32_t e : touched)
          seen[e >> 6] &= ~(UINT64_C(1) << (e & 63));
        ++ng;
        nr = checked_add(nr, k, "stream records");
      }
      if (off != h.footer_offset || ng != h.groups || nr != h.records)
        throw std::runtime_error("stream counts/length mismatch");
      if (bc.value() != h.body_crc)
        throw std::runtime_error("stream body CRC mismatch");
      close_read_stream(f, "stream reader");
      return seed;
    } catch (...) {
      if (f)
        std::fclose(f);
      throw;
    }
  }
  std::vector<uint32_t> validate() const {
    return validate([](Evt) noexcept {});
  }
  StreamHeader h{};

private:
  void read_at(uint64_t off, void *dst, size_t n) const {
    if (off > size || n > size - off || off > static_cast<uint64_t>(INT64_MAX))
      throw std::runtime_error("stream offset outside file");
    size_t done = 0;
    while (done < n) {
      ssize_t got = ::pread(fd, static_cast<uint8_t *>(dst) + done, n - done,
                            static_cast<off_t>(off + done));
      if (got < 0 && errno == EINTR)
        continue;
      if (got <= 0)
        throw std::runtime_error("short positioned stream read");
      done += static_cast<size_t>(got);
    }
  }
  int fd = -1;
  uint64_t size = 0;
};

struct RunRec {
  uint32_t e{}, v{};
  int64_t d{};
};
bool less_run(const RunRec &a, const RunRec &b) {
  return a.e != b.e ? a.e < b.e : a.d < b.d;
}
std::array<uint8_t, 16> enc_run(const RunRec &r) {
  std::array<uint8_t, 16> b{};
  put_u32(b.data(), 0, r.e);
  put_u32(b.data(), 4, r.v);
  put_i64(b.data(), 8, r.d);
  return b;
}
bool read_run(FILE *f, RunRec &r) {
  std::array<uint8_t, 16> b{};
  size_t n = std::fread(b.data(), 1, b.size(), f);
  if (n == 0) {
    if (std::ferror(f))
      throw std::runtime_error("run read error");
    return false;
  }
  if (n != b.size())
    throw std::runtime_error("truncated run");
  r = {get_u32(b.data()), get_u32(b.data() + 4), get_i64(b.data() + 8)};
  return true;
}
void write_sorted_run(std::vector<RunRec> &chunk,
                      const std::filesystem::path &p) {
  std::sort(chunk.begin(), chunk.end(), less_run);
  FILE *f = openx(p);
  try {
    for (const auto &r : chunk) {
      auto b = enc_run(r);
      checked_write(f, b.data(), b.size(), "run");
    }
    checked_close(f, "run", false);
  } catch (...) {
    if (f)
      std::fclose(f);
    throw;
  }
  chunk.clear();
}
template <class Fn>
void merge_runs(const std::vector<std::filesystem::path> &paths, Fn &&emit) {
  struct Item {
    RunRec r;
    size_t i;
  };
  auto cmp = [](const Item &a, const Item &b) { return less_run(b.r, a.r); };
  std::priority_queue<Item, std::vector<Item>, decltype(cmp)> q(cmp);
  std::vector<FILE *> fs(paths.size(), nullptr);
  try {
    for (size_t i = 0; i < paths.size(); ++i) {
      fs[i] = openf(paths[i], "rb");
      RunRec r;
      if (read_run(fs[i], r))
        q.push({r, i});
    }
    while (!q.empty()) {
      auto x = q.top();
      q.pop();
      emit(x.r);
      RunRec r;
      if (read_run(fs[x.i], r))
        q.push({r, x.i});
    }
    for (auto &f : fs) {
      close_read_stream(f, "run input");
    }
  } catch (...) {
    for (auto *f : fs)
      if (f)
        std::fclose(f);
    throw;
  }
}
std::filesystem::path merge_to_run(const std::vector<std::filesystem::path> &in,
                                   const std::filesystem::path &out) {
  FILE *f = openx(out);
  try {
    merge_runs(in, [&](const RunRec &r) {
      auto b = enc_run(r);
      checked_write(f, b.data(), b.size(), "merged run");
    });
    checked_close(f, "merged run", false);
  } catch (...) {
    if (f)
      std::fclose(f);
    throw;
  }
  return out;
}

void resizef(FILE *f, uint64_t n) {
  if (n > INT64_MAX || ftruncate(fileno(f), static_cast<off_t>(n)))
    throw std::runtime_error("resize final failed");
}

void finalize(const std::string &stream_path, const std::string &output,
              uint64_t chunk_cap) {
  reject_alias(stream_path, output);
  StreamReader s(stream_path);
  if (chunk_cap == 0)
    chunk_cap = 1000000;
  // A sorted run may split both a radius group and one edge's history.  The
  // k-way merge restores the global (edge, radius) order, so there is no
  // correctness reason to retain one record per edge in RAM.  Keeping the
  // chunk independent of m is what makes this a genuine external-memory path.
  if (chunk_cap > SIZE_MAX / sizeof(RunRec))
    throw std::runtime_error("chunk size exceeds addressable memory");
  auto out = norm(output);
  WorkDirectory work(out);
  std::vector<RunRec> chunk;
  chunk.reserve(static_cast<size_t>(chunk_cap));
  uint64_t run_id = 0;
  auto newrun = [&] {
    if (run_id == UINT64_MAX)
      throw std::overflow_error("too many external sort runs");
    return work.file(run_id++, "run");
  };
  constexpr size_t fanin = 32;
  std::vector<std::vector<std::filesystem::path>> levels;
  auto remove_runs = [](const std::vector<std::filesystem::path> &paths) {
    std::error_code ec;
    for (const auto &path : paths)
      std::filesystem::remove(path, ec);
  };
  // Base-32 online compaction: each level retains at most fanin-1 live runs.
  // Therefore run metadata and open-file fan-in are O(fanin log_fanin R), not
  // O(R), even when a caller deliberately chooses a one-record chunk.
  std::function<void(std::filesystem::path, size_t)> retain_run;
  retain_run = [&](std::filesystem::path path, size_t level) {
    if (level == levels.size())
      levels.emplace_back();
    levels[level].push_back(std::move(path));
    if (levels[level].size() != fanin)
      return;
    std::vector<std::filesystem::path> group;
    group.swap(levels[level]);
    auto merged = newrun();
    merge_to_run(group, merged);
    remove_runs(group);
    retain_run(std::move(merged), level + 1);
  };
  auto seed = s.validate([&](Evt e) {
    chunk.push_back({e.e, e.v, e.d});
    if (chunk.size() == chunk_cap) {
      auto p = newrun();
      write_sorted_run(chunk, p);
      retain_run(std::move(p), 0);
    }
  });
  if (!chunk.empty()) {
    auto p = newrun();
    write_sorted_run(chunk, p);
    retain_run(std::move(p), 0);
  }
  std::vector<std::filesystem::path> runs;
  for (auto &level : levels)
    for (auto &path : level)
      runs.push_back(std::move(path));
  // Base-32 digits leave fewer than 32 runs per level but can leave more than
  // 32 overall.  Reduce arbitrary groups of 32 until the final merge fits the
  // file-descriptor bound; all runs are globally sorted, so level is irrelevant.
  while (runs.size() > fanin) {
    using Diff = std::vector<std::filesystem::path>::difference_type;
    const size_t first = runs.size() - fanin;
    std::vector<std::filesystem::path> group(
        runs.begin() + static_cast<Diff>(first), runs.end());
    auto merged = newrun();
    merge_to_run(group, merged);
    remove_runs(group);
    runs.erase(runs.begin() + static_cast<Diff>(first), runs.end());
    runs.push_back(std::move(merged));
  }
  Header h;
  h.edges = s.h.edges;
  h.records = s.h.records;
  h.floor = s.h.floor;
  h.dmax = s.h.dmax;
  h.directory_offset = kHeaderBytes;
  h.directory_bytes = checked_mul(h.edges, kDirectoryEntryBytes, "dir bytes");
  h.records_offset =
      checked_add(h.directory_offset, h.directory_bytes, "record off");
  h.records_bytes = checked_mul(h.records, kRecordEntryBytes, "record bytes");
  h.footer_offset = checked_add(h.records_offset, h.records_bytes, "footer");
  h.file_bytes = checked_add(h.footer_offset, kFooterBytes, "file");
  h.graph_sha256 = s.h.graph_sha256;
  auto tmp = work.file(run_id, "final");
  FILE *init = openx(tmp);
  try {
    resizef(init, h.file_bytes);
    checked_close(init, "prealloc", false);
  } catch (...) {
    if (init)
      std::fclose(init);
    throw;
  }
  FILE *df = openf(tmp, "r+b"), *rf = nullptr;
  try {
    rf = openf(tmp, "r+b");
    checked_seek(df, h.directory_offset, "dir");
    checked_seek(rf, h.records_offset, "records");
    Crc64 dc, rc;
    uint64_t next_edge = 0, out_rec = 0;
    bool have = false;
    RunRec pending{};
    uint32_t base = 0, count = 0;
    u128 area = 0;
    int64_t pd = 0;
    uint32_t pv = 0;
    auto write_dir = [&](uint32_t b, uint32_t c) {
      if (out_rec < c)
        throw std::runtime_error("directory record underflow");
      auto x = encode_directory({out_rec - c, c, b});
      checked_write(df, x.data(), x.size(), "dir", &dc);
      ++next_edge;
    };
    auto emit_bp = [&](int64_t delta, uint32_t value) {
      if (delta <= pd || delta > h.dmax || value <= pv)
        throw std::runtime_error("invalid reconstructed staircase");
      area = checked_area_add(area, pv, static_cast<uint64_t>(delta - pd));
      auto x = encode_record({delta, value, area});
      checked_write(rf, x.data(), x.size(), "record", &rc);
      pd = delta;
      pv = value;
      ++count;
      ++out_rec;
    };
    auto finish_edge = [&] {
      if (!have)
        return;
      if (pending.v >= seed[pending.e])
        throw std::runtime_error("invalid final stream transition");
      emit_bp(pending.d + 1, seed[pending.e]);
      write_dir(base, count);
      have = false;
    };
    auto take = [&](const RunRec &r) {
      if (r.e >= h.edges || r.d < h.floor || r.d >= h.dmax || r.v >= seed[r.e])
        throw std::runtime_error("invalid temporary run record");
      if (!have || r.e != pending.e) {
        finish_edge();
        if (r.e < next_edge)
          throw std::runtime_error("temporary runs are not edge-sorted");
        while (next_edge < r.e)
          write_dir(seed[next_edge], 0);
        have = true;
        pending = r;
        base = r.v;
        count = 0;
        area = 0;
        pd = h.floor;
        pv = base;
      } else {
        if (r.d <= pending.d || r.v <= pending.v)
          throw std::runtime_error("temporary run order/value violation");
        emit_bp(pending.d + 1, r.v);
        pending = r;
      }
    };
    if (!runs.empty())
      merge_runs(runs, take);
    finish_edge();
    while (next_edge < h.edges)
      write_dir(seed[next_edge], 0);
    if (out_rec != h.records || next_edge != h.edges)
      throw std::runtime_error("final counts mismatch");
    checked_close(df, "dir cursor", false);
    checked_close(rf, "record cursor", false);
    h.directory_crc = dc.value();
    h.records_crc = rc.value();
    auto hb = encode_header(h);
    h.header_crc = get_u64(hb.data() + 184);
    std::array<uint8_t, kFooterBytes> fb{};
    std::memcpy(fb.data(), kFooterMagic, 8);
    put_u64(fb.data(), 8, h.directory_crc);
    put_u64(fb.data(), 16, h.records_crc);
    put_u64(fb.data(), 24, h.header_crc);
    FILE *meta = openf(tmp, "r+b");
    try {
      checked_seek(meta, 0, "header");
      checked_write(meta, hb.data(), hb.size(), "header");
      checked_seek(meta, h.footer_offset, "footer");
      checked_write(meta, fb.data(), fb.size(), "footer");
      checked_close(meta, "final index");
    } catch (...) {
      if (meta)
        std::fclose(meta);
      throw;
    }
    if (std::rename(tmp.string().c_str(), out.string().c_str()))
      throw std::runtime_error("rename final failed");
    sync_parent(out);
    std::cout << "FINAL KCSIDX3 " << out << " edges=" << h.edges
              << " records=" << h.records << " runs=" << runs.size() << "\n";
  } catch (...) {
    if (df)
      std::fclose(df);
    if (rf)
      std::fclose(rf);
    throw;
  }
}

} // namespace

int main(int argc, char **argv) {
  try {
    if (argc == 4 && std::string(argv[1]) == "test-from-text") {
      test_from_text(argv[2], argv[3]);
      return 0;
    }
    if (argc == 3 && std::string(argv[1]) == "verify") {
      StreamReader s(argv[2]);
      s.validate();
      std::cout << "STREAM VERIFY OK groups=" << s.h.groups
                << " records=" << s.h.records << "\n";
      return 0;
    }
    if ((argc == 4 || argc == 5) && std::string(argv[1]) == "finalize") {
      uint64_t cap = argc == 5 ? parse_chunk_cap(argv[4]) : 0;
      finalize(argv[2], argv[3], cap);
      return 0;
    }
    std::cerr << "usage: stream_tool verify <stream2> | finalize <stream2> "
                 "<kcsidx3> [positive chunk-records] | test-from-text "
                 "<text-index> <stream2>\n";
    return 1;
  } catch (const std::exception &ex) {
    std::cerr << "stream2 failed: " << ex.what() << "\n";
    return 1;
  }
}
