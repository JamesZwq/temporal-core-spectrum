// kcs_pack.cpp -- convert a Delta-spectrum index into the binary ".kcsb" form described in
// kcs_format.hpp.  The builder (kcs.cpp) is not touched: it is frozen and heavily validated,
// and everything this converter needs is already in its outputs.
//
// TWO input forms are accepted, selected by sniffing the magic, so BOTH build pipelines end
// in the same converter and must land on the same .kcsb bytes:
//
//     kcs --dump   -> <g>.kcs_index (TEXT) ------------------------> kcs_pack -> .kcsb
//     kcs --stream -> <g>.strm -> stream_tool finalize -> KCSIDX3 -> kcs_pack -> .kcsb
//
// On a WELL-FORMED text index the text parser here is character-for-character the one in
// cc-impl/cc_index6.cpp, so "pack then build" and "build from text" see exactly the same edge
// list in exactly the same order.  That is what makes the .cc6 byte-identity test a real test
// of the converter.  It differs on a MALFORMED index: where cc_index6 silently skips, this
// refuses (see "INPUT IS NOT TRUSTED" below), because a converter that quietly drops records
// converts a broken build into a smaller index that passes every check.
//
// A KCSIDX3 file stores the staircases but NOT the (u,v,t) edge list -- only a SHA-256 over
// the builder's canonical list (ordinal,u,v,t).  The packer therefore re-derives that list
// from the GRAPH file (default: the input path minus ".kcsidx3"; override with --graph),
// replicating the builder's canonicalization (first-appearance node renumbering, u<v swap,
// sort by (t,u,v), dedup), and refuses to write anything unless its fingerprint equals the
// one stored in the index.  Any drift between that replica and kcs.cpp load_graph -- or a
// stale / foreign graph file -- is therefore loud, never a silent byte difference.
//
// usage: kcs_pack <index(.kcs_index|.kcsidx3)> [-o out.kcsb] [--graph <graph>]
//                 [--no-verify] [--stats] [--allow-malformed]
//   --graph G     the graph the index was built from; KCSIDX3 input only (the text index
//                 carries its own edge list).  Default: input path minus ".kcsidx3".
//   --no-verify   skip the built-in re-read (which walks every edge and every breakpoint
//                 through kcs_reader.hpp and compares against the parsed input)
//   --stats       print the width decision table and the section sizes
//   --allow-malformed
//                 downgrade the "unparseable line" and "no trailing newline" errors to
//                 warnings.  TEXT input only -- a binary KCSIDX3 is either valid or refused.
//                 ONLY for deliberately exercising the cc-impl skip-compatible path; a
//                 normal run must never need it.
//
// INPUT IS NOT TRUSTED.  A .kcs_index truncated by a full disk still ends in a syntactically
// valid-looking line (the parser finds `|` and `base=` and stops at the cut), so a tolerant
// parser turns a truncated build into a smaller index that verifies clean.  Every non-comment
// line must therefore parse completely, the file must end in a newline, and every id / radius
// must fit the field the format gives it.  A KCSIDX3 input is held to the full strength of
// its own format: header CRC + layout against the true file size, directory and record
// section CRCs, footer, per-edge monotone staircase, exact prefix areas, and the graph
// fingerprint -- all before a single output byte is written.  The KCSIDX3 is read with
// bounded sequential buffers, never mmap'ed, so packing does not add the whole index to the
// resident set (that would hand back the memory saving --stream exists for).
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdint>
#include <cerrno>
#include <string>
#include <vector>
#include <algorithm>
#include <array>
#include <fstream>
#include <limits>
#include <stdexcept>
#include <tuple>
#include <unordered_map>
#include <chrono>
#include <unistd.h>
#include <sys/stat.h>
#include "kcs_format.hpp"
#include "kcs_reader.hpp"
#include "kcs_stream_format.hpp"   // kcsidx:: KCSIDX3 codecs + kcsstream:: magic (refusal)
using namespace std;
using namespace kcs;
using ll = long long;

static double now_s() {
    return chrono::duration<double>(chrono::steady_clock::now().time_since_epoch()).count();
}
static u64 align8(u64 x) { return (x + 7) & ~(u64)7; }

// ===================== KCSIDX3 input path =====================
//
// ---- the builder's canonical edge list, replicated from kcs.cpp load_graph ----
// This MUST match load_graph decision for decision: which lines are skipped, the one
// optional count header, the '#'/'%' comments, the negative-t refusal, the self-loop skip,
// first-appearance node renumbering (u interned before v), the u<v swap, the (t,u,v) sort,
// the (t,u,v) dedup, and the side-id bound.  The SHA-256 fingerprint stored in the KCSIDX3
// is over exactly this list, so any divergence fails the fingerprint check loudly instead
// of packing wrong (u,v,t) bytes.
struct CanonEdge { int32_t u, v; long long t; };

static bool cg_space(char ch) {
    return ch == ' ' || ch == '\t' || ch == '\r' || ch == '\n' || ch == '\f' || ch == '\v';
}

struct CgLine { int kind; long long u, v, t; };   // kind: 0 skip, 1 count header, 2 edge

static CgLine cg_parse_line(const string& line) {
    const char* p = line.data();
    const char* const finish = p + line.size();
    auto skip_space = [&]() { while (p != finish && cg_space(*p)) ++p; };
    auto parse_i64 = [&](const char* field) -> long long {
        skip_space();
        if (p == finish || *p == '#' || *p == '%')
            throw invalid_argument(string("missing ") + field);
        char* end = nullptr;
        errno = 0;
        const long long value = strtoll(p, &end, 10);
        if (errno == ERANGE)
            throw out_of_range(string(field) + " exceeds signed 64-bit range");
        if (end == p)
            throw invalid_argument(string("invalid ") + field);
        if (end != finish && !cg_space(*end))
            throw invalid_argument(string("invalid delimiter after ") + field);
        p = end;
        return value;
    };
    skip_space();
    if (p == finish || *p == '#' || *p == '%') return {0, 0, 0, 0};
    CgLine out{2, 0, 0, 0};
    out.u = parse_i64("first column");
    skip_space();
    if (p == finish || *p == '#' || *p == '%') {
        if (out.u < 0) throw invalid_argument("leading count header must be non-negative");
        out.kind = 1;
        return out;
    }
    out.v = parse_i64("second column");
    skip_space();
    if (p == finish || *p == '#' || *p == '%')
        throw invalid_argument("edge record is missing its timestamp");
    out.t = parse_i64("timestamp");
    return out;
}

static vector<CanonEdge> load_canonical_graph(const string& filename) {
    ifstream input(filename);
    if (!input.is_open())
        throw runtime_error("cannot open graph " + filename + ": " + strerror(errno));
    vector<CanonEdge> keys;
    unordered_map<long long, int32_t> ids;
    ids.reserve(1 << 20);
    auto intern = [&](long long external_id) -> int32_t {
        auto it = ids.find(external_id);
        if (it != ids.end()) return it->second;
        if (ids.size() >= (size_t)numeric_limits<int32_t>::max())
            throw overflow_error("node count exceeds signed 32-bit encoding");
        const int32_t next = (int32_t)ids.size();
        ids.emplace(external_id, next);
        return next;
    };
    string line;
    size_t line_no = 0;
    bool saw_count_header = false, saw_edge_record = false;
    while (getline(input, line)) {
        ++line_no;
        CgLine parsed;
        try {
            parsed = cg_parse_line(line);
        } catch (const exception& ex) {
            throw invalid_argument(filename + ":" + to_string(line_no) + ": " + ex.what());
        }
        if (parsed.kind == 0) continue;
        if (parsed.kind == 1) {
            if (saw_count_header || saw_edge_record)
                throw invalid_argument(filename + ":" + to_string(line_no) +
                                       ": a count header is allowed only once, before all edges");
            saw_count_header = true;
            continue;
        }
        saw_edge_record = true;
        if (parsed.t < 0)
            throw invalid_argument(filename + ":" + to_string(line_no) +
                                   ": negative timestamps are unsupported");
        if (parsed.u == parsed.v) continue;
        int32_t u = intern(parsed.u);
        int32_t v = intern(parsed.v);
        if (u > v) swap(u, v);
        keys.push_back(CanonEdge{u, v, parsed.t});
    }
    if (input.bad()) throw runtime_error("I/O error while reading " + filename);
    sort(keys.begin(), keys.end(), [](const CanonEdge& a, const CanonEdge& b) {
        return tie(a.t, a.u, a.v) < tie(b.t, b.u, b.v);
    });
    keys.erase(unique(keys.begin(), keys.end(), [](const CanonEdge& a, const CanonEdge& b) {
        return a.t == b.t && a.u == b.u && a.v == b.v;
    }), keys.end());
    // the builder's own side-id bound: nothing beyond it can have produced an index
    constexpr uint64_t kMaxSideCount = (uint64_t)numeric_limits<int32_t>::max() + 1u;
    if (keys.size() > (size_t)(kMaxSideCount / 2u))
        throw overflow_error("edge count exceeds the builder's signed 32-bit side encoding");
    return keys;
}

// ---- streaming, fully-checked KCSIDX3 read into the same arrays the text parser fills ----
// One sequential pass with a bounded buffer: header CRC + layout against the true file size
// (kcsidx::decode_header), directory contiguity/cover + CRC, per-edge monotone staircase +
// exact prefix areas + record CRC, footer, and the graph fingerprint.  This is the same
// strength as kcsidx::BinaryIndex::verify_sections(), without mmap'ing gigabytes into RSS.
static void load_kcsidx3(const string& inpath, const string& graphpath,
                         vector<u32>& eu, vector<u32>& ev, vector<ll>& et, vector<u32>& ebase,
                         vector<u32>& curveD, vector<u32>& curveV, vector<u64>& coff,
                         ll& DFLOOR, ll& DMAX, u32& maxnode) {
    using kcsidx::checked_read;

    // 1. the canonical edge list and its fingerprint -- this binds the KCSIDX3 to (u,v,t)
    const vector<CanonEdge> g = load_canonical_graph(graphpath);
    kcsidx::Sha256 gsha;
    for (size_t e = 0; e < g.size(); ++e)
        kcsidx::hash_graph_edge(gsha, (uint64_t)e, (int64_t)g[e].u, (int64_t)g[e].v,
                                (int64_t)g[e].t);
    const array<uint8_t, 32> gfp = gsha.finish();

    // 2. header, checked against the true file size
    FILE* f = fopen(inpath.c_str(), "rb");
    if (!f) throw runtime_error("cannot open " + inpath + ": " + strerror(errno));
    struct FileCloser { FILE* fp; ~FileCloser() { if (fp) fclose(fp); } } closer{f};
    struct stat st {};
    if (fstat(fileno(f), &st) != 0 || st.st_size < 0)
        throw runtime_error("cannot stat " + inpath);
    array<uint8_t, kcsidx::kHeaderBytes> hb{};
    checked_read(f, hb.data(), hb.size(), "KCSIDX3 header");
    const kcsidx::Header H = kcsidx::decode_header(hb.data(), hb.size(), (uint64_t)st.st_size);
    if (H.edges != g.size())
        throw runtime_error("KCSIDX3 has " + to_string(H.edges) + " edges but " + graphpath +
                            " canonicalizes to " + to_string(g.size()) +
                            " -- wrong or modified graph file");
    if (H.graph_sha256 != gfp)
        throw runtime_error("graph fingerprint mismatch: " + graphpath +
                            " is not the graph this KCSIDX3 was built from");
    const u64 m = H.edges, nbp = H.records;
    DFLOOR = H.floor;
    DMAX = H.dmax;

    // 3. directory: contiguity, cover, CRC; fills base + CSR offsets
    ebase.clear(); ebase.reserve((size_t)m);
    coff.clear();  coff.reserve((size_t)m + 1); coff.push_back(0);
    kcsidx::Crc64 dcrc;
    {
        const u64 CH = 1u << 16;   // directory entries per read (1 MiB)
        vector<unsigned char> buf((size_t)(CH * kcsidx::kDirectoryEntryBytes));
        u64 done = 0, next = 0;
        while (done < m) {
            const u64 n = min(CH, m - done);
            const size_t bytes = (size_t)(n * kcsidx::kDirectoryEntryBytes);
            checked_read(f, buf.data(), bytes, "KCSIDX3 directory");
            dcrc.update(buf.data(), bytes);
            for (u64 i = 0; i < n; ++i) {
                const kcsidx::DirectoryEntry d =
                    kcsidx::decode_directory(buf.data() + i * kcsidx::kDirectoryEntryBytes);
                if (d.first != next)
                    throw runtime_error("non-contiguous KCSIDX3 directory at edge " +
                                        to_string(done + i));
                if (d.count > H.records - next)
                    throw runtime_error("KCSIDX3 directory overruns the record section");
                next += d.count;
                ebase.push_back(d.base);
                coff.push_back(next);
            }
            done += n;
        }
        if (next != H.records)
            throw runtime_error("KCSIDX3 directory does not cover the record section");
        if (dcrc.value() != H.directory_crc)
            throw runtime_error("KCSIDX3 directory checksum mismatch");
    }

    // 4. records: per-edge monotone staircase, exact prefix areas, u32 radius guard, CRC
    curveD.clear(); curveD.reserve((size_t)nbp);   // nbp <= file_size/24, so this is bounded
    curveV.clear(); curveV.reserve((size_t)nbp);
    kcsidx::Crc64 rcrc;
    {
        const u64 CH = 43690;      // record entries per read (~1 MiB, a multiple of 24 B)
        vector<unsigned char> buf((size_t)(CH * kcsidx::kRecordEntryBytes));
        u64 idx = 0, e = 0;
        ll pd = 0; u32 pv = 0; kcsidx::u128 area = 0;
        while (idx < nbp) {
            const u64 n = min(CH, nbp - idx);
            const size_t bytes = (size_t)(n * kcsidx::kRecordEntryBytes);
            checked_read(f, buf.data(), bytes, "KCSIDX3 records");
            rcrc.update(buf.data(), bytes);
            for (u64 i = 0; i < n; ++i, ++idx) {
                while (idx >= coff[(size_t)e + 1]) ++e;   // e < m: coff[m] == nbp > idx
                if (idx == coff[(size_t)e]) { pd = DFLOOR; pv = ebase[(size_t)e]; area = 0; }
                const kcsidx::RecordEntry r =
                    kcsidx::decode_record(buf.data() + i * kcsidx::kRecordEntryBytes);
                if (r.delta <= pd || r.delta > DMAX || r.value <= pv)
                    throw runtime_error("non-monotone KCSIDX3 record list at edge " +
                                        to_string(e));
                area = kcsidx::checked_area_add(area, pv, (uint64_t)(r.delta - pd));
                if (r.prefix != area)
                    throw runtime_error("bad KCSIDX3 prefix area at edge " + to_string(e));
                // same scale guard the text parser applies: .kcsb radii are u32 fields
                if ((unsigned long long)r.delta > 0xFFFFFFFFull)
                    throw runtime_error("breakpoint radius outside u32 (would wrap) at edge " +
                                        to_string(e));
                curveD.push_back((u32)r.delta);
                curveV.push_back(r.value);
                pd = r.delta;
                pv = r.value;
            }
        }
        if (rcrc.value() != H.records_crc)
            throw runtime_error("KCSIDX3 record checksum mismatch");
    }

    // 5. footer, and nothing after it
    array<uint8_t, kcsidx::kFooterBytes> fb{};
    checked_read(f, fb.data(), fb.size(), "KCSIDX3 footer");
    if (memcmp(fb.data(), kcsidx::kFooterMagic, 8) != 0 ||
        kcsidx::get_u64(fb.data() + 8) != H.directory_crc ||
        kcsidx::get_u64(fb.data() + 16) != H.records_crc ||
        kcsidx::get_u64(fb.data() + 24) != H.header_crc)
        throw runtime_error("invalid KCSIDX3 footer");
    if (fgetc(f) != EOF)
        throw runtime_error("trailing bytes after the KCSIDX3 footer");

    // 6. the edge list itself, from the fingerprint-verified canonical graph
    eu.clear(); eu.reserve((size_t)m);
    ev.clear(); ev.reserve((size_t)m);
    et.clear(); et.reserve((size_t)m);
    u32 mx = 0;
    for (const CanonEdge& ce : g) {
        eu.push_back((u32)ce.u);
        ev.push_back((u32)ce.v);
        et.push_back((ll)ce.t);
        if ((u32)ce.u > mx) mx = (u32)ce.u;
        if ((u32)ce.v > mx) mx = (u32)ce.v;
    }
    maxnode = mx;
    fprintf(stderr, "[kcs_pack] KCSIDX3 %s: %llu edges  %llu breakpoints  graph %s  "
                    "fingerprint OK  checksums OK\n",
            inpath.c_str(), (unsigned long long)m, (unsigned long long)nbp, graphpath.c_str());
}

// ---- input sniffing: one converter, every format either handled or loudly refused ----
enum class InKind { Text, Kcsidx3, Kcsb, Stream2, Kcsidx2 };

static InKind sniff_input(const string& path) {
    FILE* f = fopen(path.c_str(), "rb");
    if (!f) { fprintf(stderr, "kcs_pack: cannot open %s: %s\n", path.c_str(), strerror(errno));
              exit(1); }
    unsigned char b[8] = {0};
    const size_t got = fread(b, 1, sizeof b, f);
    fclose(f);
    if (got == sizeof b) {
        if (memcmp(b, kcsidx::kMagic, 8) == 0)              return InKind::Kcsidx3;
        if (memcmp(b, KCSB_MAGIC, 8) == 0)                  return InKind::Kcsb;
        if (memcmp(b, kcsstream::kStreamMagic, 8) == 0)     return InKind::Stream2;
        if (memcmp(b, "KCSIDX2\0", 8) == 0)                 return InKind::Kcsidx2;
    }
    return InKind::Text;
}

static bool strip_suffix(string& s, const string& suf) {
    if (s.size() > suf.size() && s.compare(s.size() - suf.size(), suf.size(), suf) == 0) {
        s.resize(s.size() - suf.size());
        return true;
    }
    return false;
}

int main(int argc, char** argv) {
    if (argc < 2) { fprintf(stderr, "usage: kcs_pack <index(.kcs_index|.kcsidx3)> [-o out.kcsb] "
                                    "[--graph <graph>] [--no-verify] [--stats] "
                                    "[--allow-malformed]\n"); return 1; }
    string inpath = argv[1], outpath, graphpath;
    bool verify = true, stats = false, allow_malformed = false;
    for (int i = 2; i < argc; ++i) {
        if (!strcmp(argv[i], "-o") && i + 1 < argc) outpath = argv[++i];
        else if (!strcmp(argv[i], "--graph") && i + 1 < argc) graphpath = argv[++i];
        else if (!strcmp(argv[i], "--no-verify")) verify = false;
        else if (!strcmp(argv[i], "--stats")) stats = true;
        else if (!strcmp(argv[i], "--allow-malformed")) allow_malformed = true;
        else { fprintf(stderr, "kcs_pack: unknown arg %s\n", argv[i]); return 1; }
    }

    const InKind kind = sniff_input(inpath);
    if (kind == InKind::Kcsb) {
        fprintf(stderr, "kcs_pack: FATAL %s is already a .kcsb -- refusing to re-pack it\n",
                inpath.c_str());
        return 2;
    }
    if (kind == InKind::Stream2) {
        fprintf(stderr, "kcs_pack: FATAL %s is a KCSSTRM2 stream, not an index -- run "
                        "`stream_tool finalize` on it first, then pack the KCSIDX3\n",
                inpath.c_str());
        return 2;
    }
    if (kind == InKind::Kcsidx2) {
        fprintf(stderr, "kcs_pack: FATAL %s is a legacy KCSIDX2 file; this build reads "
                        "KCSIDX3 -- rebuild it with the current pipeline\n", inpath.c_str());
        return 2;
    }
    if (kind == InKind::Kcsidx3) {
        if (allow_malformed) {
            fprintf(stderr, "kcs_pack: FATAL --allow-malformed has no meaning for a binary "
                            "KCSIDX3 input; the file is either valid or refused\n");
            return 2;
        }
        if (graphpath.empty()) {
            string b = inpath;
            if (!strip_suffix(b, ".kcsidx3")) {
                fprintf(stderr, "kcs_pack: FATAL %s does not end in .kcsidx3, so the graph "
                                "path cannot be derived -- pass --graph <graph>\n",
                        inpath.c_str());
                return 2;
            }
            graphpath = b;
        }
    } else if (!graphpath.empty()) {
        fprintf(stderr, "kcs_pack: FATAL --graph is only meaningful for a KCSIDX3 input; "
                        "the text index carries its own edge list\n");
        return 2;
    }
    if (outpath.empty()) {
        string b = inpath;
        strip_suffix(b, kind == InKind::Kcsidx3 ? ".kcsidx3" : ".kcs_index");
        outpath = b + ".kcsb";
    }

    // ---------------- 1. load the input into one canonical set of arrays ----------------
    // Everything below section 1 is SHARED between the two input forms: identical arrays in,
    // identical .kcsb bytes out.  That is the property the A/B pipeline gate rests on.
    double t0 = now_s();
    vector<u32> eu, ev, ebase; vector<ll> et;
    vector<u32> curveD, curveV; vector<u64> coff; coff.push_back(0);
    ll DMAX = 0, DFLOOR = 0; u32 maxnode = 0;

    if (kind == InKind::Kcsidx3) {
        try {
            load_kcsidx3(inpath, graphpath, eu, ev, et, ebase, curveD, curveV, coff,
                         DFLOOR, DMAX, maxnode);
        } catch (const exception& ex) {
            fprintf(stderr, "kcs_pack: FATAL %s\n", ex.what());
            return 2;
        }
    } else {
    // ---------------- 1t. parse the text (verbatim from cc_index6.cpp, plus refusals) ------
    // The accepted-line path below is character-for-character what cc_index6.cpp does, so a
    // well-formed index still packs to exactly the same bytes.  What is NEW is that every
    // path cc_index6 uses to *drop* something now raises a fatal parse error instead.
    FILE* f = fopen(inpath.c_str(), "r");
    if (!f) { perror("open index"); return 1; }
    size_t bufcap = 1 << 20; char* buf = (char*)malloc(bufcap); ssize_t nn;
    u64 lineno = 0, nbad = 0; bool saw_range = false, last_line_unterminated = false;
    auto bad_line = [&](const char* why, const char* extra) {
        ++nbad;
        if (nbad <= 20) fprintf(stderr, "kcs_pack: %s:%llu: %s%s%s\n", inpath.c_str(),
                                (unsigned long long)lineno, why, extra ? ": " : "",
                                extra ? extra : "");
    };
    while ((nn = getline(&buf, &bufcap, f)) > 0) {
        ++lineno;
        // A file cut mid-line (full disk, killed builder, truncated copy) ends without '\n'.
        // The line it leaves behind usually still parses, which is exactly why this is checked.
        last_line_unterminated = (buf[nn - 1] != '\n');
        if (buf[0] == '#') {
            char* p = strstr(buf, "Delta in [");
            if (p) { p += 10; DFLOOR = strtoll(p, &p, 10); while (*p == ',' || *p == ' ') ++p;
                     DMAX = strtoll(p, &p, 10); saw_range = true; }
            continue;
        }
        {   // blank lines are not records and are not tolerated silently either
            const char* q = buf; while (*q == ' ' || *q == '\t' || *q == '\r' || *q == '\n') ++q;
            if (!*q) { bad_line("blank line inside the index", nullptr); continue; }
        }
        char* p = buf; char* q0;
        q0 = p; long u = strtol(p, &p, 10); if (p == q0) { bad_line("no endpoint u", buf); continue; }
        q0 = p; long v = strtol(p, &p, 10); if (p == q0) { bad_line("no endpoint v", buf); continue; }
        q0 = p; ll   t = strtoll(p, &p, 10); if (p == q0) { bad_line("no timestamp t", buf); continue; }
        char* bar = strchr(p, '|'); if (!bar) { bad_line("record has no '|'", buf); continue; }
        char* bp = strstr(bar, "base="); if (!bp) { bad_line("record has no 'base='", buf); continue; }
        bp += 5;
        q0 = bp; u64 base64 = strtoull(bp, &bp, 10);
        if (bp == q0) { bad_line("'base=' has no value", buf); continue; }
        // ---- scale guards: the format gives u/v/base a u32 field; a wider value must not wrap
        if (u < 0 || (unsigned long long)u > 0xFFFFFFFFull) { bad_line("endpoint u outside u32", buf); continue; }
        if (v < 0 || (unsigned long long)v > 0xFFFFFFFFull) { bad_line("endpoint v outside u32", buf); continue; }
        if (base64 > 0xFFFFFFFFull) { bad_line("base outside u32", buf); continue; }
        u32 base = (u32)base64;
        const size_t bp_mark = curveD.size();          // roll back if the staircase is bad
        bool line_ok = true;
        char* sc = strchr(bp, ';');
        if (sc) { p = sc + 1;
            while (*p) { while (*p == ' ' || *p == '\n' || *p == '\r') ++p; if (!*p) break;
                q0 = p; ll d = strtoll(p, &p, 10);
                if (p == q0) { bad_line("breakpoint has no radius", buf); line_ok = false; break; }
                if (*p != ':') { bad_line("breakpoint radius not followed by ':'", buf); line_ok = false; break; }
                ++p;
                q0 = p; u64 val64 = strtoull(p, &p, 10);
                if (p == q0) { bad_line("breakpoint has no value", buf); line_ok = false; break; }
                if (d < 0 || (unsigned long long)d > 0xFFFFFFFFull) {
                    bad_line("breakpoint radius outside u32 (would wrap)", buf); line_ok = false; break; }
                if (val64 > 0xFFFFFFFFull) {
                    bad_line("breakpoint value outside u32", buf); line_ok = false; break; }
                curveD.push_back((u32)d); curveV.push_back((u32)val64); } }
        if (!line_ok) { curveD.resize(bp_mark); curveV.resize(bp_mark); continue; }
        eu.push_back((u32)u); ev.push_back((u32)v); et.push_back(t); ebase.push_back(base);
        if ((u32)u > maxnode) maxnode = (u32)u;
        if ((u32)v > maxnode) maxnode = (u32)v;
        coff.push_back(curveD.size());
    }
    if (ferror(f)) { perror("read index"); fclose(f); free(buf); return 1; }
    fclose(f); free(buf);

    if (last_line_unterminated) {
        fprintf(stderr, "kcs_pack: FATAL %s ends without a newline -- the file is TRUNCATED "
                        "(the cut line still parses, which is why this is checked).\n",
                inpath.c_str());
        if (!allow_malformed) return 2;
    }
    if (nbad) {
        fprintf(stderr, "kcs_pack: %s %llu unparseable line%s -- refusing to write a "
                        "silently smaller index%s\n",
                allow_malformed ? "WARNING:" : "FATAL:", (unsigned long long)nbad,
                nbad == 1 ? "" : "s", allow_malformed ? " (--allow-malformed)" : "");
        if (!allow_malformed) return 2;
    }
    if (!saw_range) {
        fprintf(stderr, "kcs_pack: FATAL %s has no '# ... Delta in [lo, hi]' header line; "
                        "dfloor/dmax would silently default to 0\n", inpath.c_str());
        return 2;
    }
    }   // end of the text branch

    double parse_s = now_s() - t0;
    const u64 m = eu.size(), nbp = curveD.size();
    if (m == 0) { fprintf(stderr, "kcs_pack: empty index\n"); return 1; }

    // ---------------- 2. the same staircase check the consumer makes ----------------
    for (u64 e = 0; e < m; ++e) {
        ll pd = DFLOOR; u32 pv = ebase[e];
        for (u64 c = coff[e]; c < coff[e+1]; ++c) {
            if ((ll)curveD[c] <= pd || curveV[c] <= pv || (ll)curveD[c] > DMAX) {
                fprintf(stderr, "kcs_pack: FATAL non-monotone staircase on edge %llu\n",
                        (unsigned long long)e); return 2; }
            pd = curveD[c]; pv = curveV[c];
        }
    }

    // ---------------- 3. widths, measured ----------------
    u64 maxD = 0, maxV = 0, maxBase = 0; ll maxT = 0, minT = 0; bool anyneg = false;
    for (u64 e = 0; e < m; ++e) {
        if (ebase[e] > maxBase) maxBase = ebase[e];
        if (ebase[e] > maxV)    maxV    = ebase[e];
        if (et[e] > maxT) maxT = et[e];
        if (et[e] < minT) { minT = et[e]; anyneg = true; }
    }
    for (u64 c = 0; c < nbp; ++c) { if (curveD[c] > maxD) maxD = curveD[c];
                                    if (curveV[c] > maxV) maxV = curveV[c]; }
    u32 KMAX = 0;
    for (u64 e = 0; e < m; ++e) {
        u32 fin = (coff[e+1] > coff[e]) ? curveV[coff[e+1]-1] : ebase[e];
        if (fin > KMAX) KMAX = fin;
    }
    const unsigned w_t    = (anyneg || (u64)maxT > 0xFFFFFFFFull) ? 8 : 4;
    const unsigned w_base = kcsb_width(maxBase);
    const unsigned w_off  = (nbp > 0xFFFFFFFFull) ? 8 : 4;
    const unsigned w_d    = kcsb_width(maxD);
    const unsigned w_v    = kcsb_width(maxV);
    const u32 flags = anyneg ? KHF_T_SIGNED : 0u;

    // ---------------- 4. section layout ----------------
    KcsbHeader H; memset(&H, 0, sizeof H);
    memcpy(H.magic, KCSB_MAGIC, 8);
    H.version = KCSB_VERSION; H.endian = KCSB_ENDIAN;
    H.m = m; H.nbp = nbp; H.dfloor = DFLOOR; H.dmax = DMAX;
    H.kmax = KMAX; H.maxnode = maxnode;
    H.w_t = (u8)w_t; H.w_base = (u8)w_base; H.w_off = (u8)w_off;
    H.w_d = (u8)w_d; H.w_v = (u8)w_v; H.flags = flags;
    const u64 len[KS_COUNT] = { m*4, m*4, m*w_t, m*w_base, (m+1)*w_off, nbp*w_d, nbp*w_v };
    u64 cur = sizeof(KcsbHeader);
    for (int s = 0; s < KS_COUNT; ++s) {
        cur = align8(cur); H.sec[s].off = cur; H.sec[s].len = len[s];
        cur += len[s] + KCSB_PAD;
    }
    const u64 fsize = align8(cur);

    // ---------------- 5. write ----------------
    // Written to <out>.tmp and renamed only after the last byte is on disk, so a run that
    // dies on a full disk can never leave something at `outpath` that a later step, or a
    // resume that tests `[ -f out.kcsb ]`, would mistake for a finished index.
    double t1 = now_s();
    const string tmppath = outpath + ".tmp";
    FILE* g = fopen(tmppath.c_str(), "wb");
    if (!g) { perror("open out"); return 1; }
    auto die_write = [&](const char* what) {
        int e = errno;
        fprintf(stderr, "kcs_pack: FATAL %s while writing %s: %s\n",
                what, tmppath.c_str(), strerror(e));
        fclose(g);
        if (unlink(tmppath.c_str()) != 0 && errno != ENOENT) perror("unlink partial");
        fprintf(stderr, "kcs_pack: removed the partial file; no .kcsb was produced\n");
        exit(1);
    };
    u64 pos = 0;
    auto emit = [&](const void* p, u64 n) {
        if (n && fwrite(p, 1, n, g) != n) die_write("short write"); pos += n; };
    auto padto = [&](u64 target) {
        static const unsigned char zero[64] = {0};
        while (pos < target) { u64 n = target - pos; if (n > 64) n = 64; emit(zero, n); } };
    emit(&H, sizeof H);
    vector<unsigned char> chunk;
    auto emit_packed = [&](int s, u64 n, unsigned w, auto get) {
        padto(H.sec[s].off);
        const u64 STEP = 1u << 20;
        chunk.assign(STEP * w, 0);
        for (u64 i = 0; i < n; ) {
            u64 c = n - i < STEP ? n - i : STEP;
            for (u64 j = 0; j < c; ++j) kcsb_store(&chunk[j * w], w, (u64)get(i + j));
            emit(chunk.data(), c * w);
            i += c;
        }
    };
    emit_packed(KS_EU,    m,     4,      [&](u64 i){ return (u64)eu[i]; });
    emit_packed(KS_EV,    m,     4,      [&](u64 i){ return (u64)ev[i]; });
    emit_packed(KS_ET,    m,     w_t,    [&](u64 i){ u64 r; ll t = et[i]; memcpy(&r, &t, 8); return r; });
    emit_packed(KS_BASE,  m,     w_base, [&](u64 i){ return (u64)ebase[i]; });
    emit_packed(KS_BPOFF, m + 1, w_off,  [&](u64 i){ return (u64)coff[i]; });
    emit_packed(KS_BPD,   nbp,   w_d,    [&](u64 i){ return (u64)curveD[i]; });
    emit_packed(KS_BPV,   nbp,   w_v,    [&](u64 i){ return (u64)curveV[i]; });
    padto(fsize);
    // fflush before fclose so a deferred ENOSPC is reported while the path is still known;
    // fclose frees the FILE* even when it fails, so it must not be called twice.
    if (fflush(g) != 0) die_write("flush failed");
    if (fclose(g) != 0) {
        int e = errno;
        fprintf(stderr, "kcs_pack: FATAL close failed on %s: %s\n", tmppath.c_str(), strerror(e));
        if (unlink(tmppath.c_str()) != 0 && errno != ENOENT) perror("unlink partial");
        fprintf(stderr, "kcs_pack: removed the partial file; no .kcsb was produced\n");
        return 1;
    }
    if (rename(tmppath.c_str(), outpath.c_str()) != 0) {
        perror("rename into place");
        if (unlink(tmppath.c_str()) != 0 && errno != ENOENT) perror("unlink partial");
        return 1;
    }
    double write_s = now_s() - t1;

    fprintf(stderr, "[kcs_pack] %s -> %s\n", inpath.c_str(), outpath.c_str());
    fprintf(stderr, "[kcs_pack] m=%llu nbp=%llu Dfloor=%lld Dmax=%lld Kmax=%u  "
                    "widths t=%u base=%u off=%u d=%u v=%u\n",
            (unsigned long long)m, (unsigned long long)nbp, DFLOOR, DMAX, KMAX,
            w_t, w_base, w_off, w_d, w_v);
    fprintf(stderr, "[kcs_pack] parse %.2fs  write %.2fs  out %llu B\n",
            parse_s, write_s, (unsigned long long)fsize);
    if (stats) {
        static const char* nm[KS_COUNT] = {"EU","EV","ET","BASE","BPOFF","BPD","BPV"};
        for (int s = 0; s < KS_COUNT; ++s)
            fprintf(stderr, "   %-6s off=%-12llu len=%-12llu (%.2f%%)\n", nm[s],
                    (unsigned long long)H.sec[s].off, (unsigned long long)H.sec[s].len,
                    100.0 * (double)H.sec[s].len / (double)fsize);
        fprintf(stderr, "   breakpoint bytes per bp = %u (d=%u + v=%u)\n", w_d + w_v, w_d, w_v);
    }

    // ---------------- 6. built-in re-read: EVERY edge, EVERY breakpoint ----------------
    if (verify) {
        double t2 = now_s();
        KcsIndex R;
        if (!R.open(outpath.c_str())) { fprintf(stderr, "kcs_pack: VERIFY could not open\n"); return 3; }
        u64 bad = 0;
        auto err = [&](const char* what, u64 i, long long got, long long want) {
            if (++bad <= 20) fprintf(stderr, "kcs_pack: VERIFY %s at %llu: got %lld want %lld\n",
                                     what, (unsigned long long)i, got, want); };
        if (R.m() != m)   err("m", 0, (long long)R.m(), (long long)m);
        if (R.nbp() != nbp) err("nbp", 0, (long long)R.nbp(), (long long)nbp);
        if (R.dfloor() != DFLOOR) err("dfloor", 0, (long long)R.dfloor(), DFLOOR);
        if (R.dmax()   != DMAX)   err("dmax", 0, (long long)R.dmax(), DMAX);
        if (R.kmax()   != KMAX)   err("kmax", 0, (long long)R.kmax(), (long long)KMAX);
        if (R.maxnode()!= maxnode)err("maxnode",0,(long long)R.maxnode(),(long long)maxnode);
        for (u64 e = 0; e < m; ++e) {
            if (R.eu(e) != eu[e])     err("eu", e, R.eu(e), eu[e]);
            if (R.ev(e) != ev[e])     err("ev", e, R.ev(e), ev[e]);
            if (R.et(e) != et[e])     err("et", e, (long long)R.et(e), et[e]);
            if (R.base(e) != ebase[e])err("base", e, R.base(e), ebase[e]);
            if (R.bpbegin(e) != coff[e])   err("bpbegin", e, (long long)R.bpbegin(e), (long long)coff[e]);
            if (R.bpend(e)   != coff[e+1]) err("bpend",   e, (long long)R.bpend(e),   (long long)coff[e+1]);
            u32 want_dl = (coff[e+1] > coff[e]) ? curveV[coff[e+1]-1] : ebase[e];
            if (R.deathlevel(e) != want_dl) err("deathlevel", e, R.deathlevel(e), want_dl);
        }
        for (u64 c = 0; c < nbp; ++c) {
            if (R.bpd(c) != curveD[c]) err("bpd", c, R.bpd(c), curveD[c]);
            if (R.bpv(c) != curveV[c]) err("bpv", c, R.bpv(c), curveV[c]);
        }
        R.close();
        fprintf(stderr, "[kcs_pack] verify %llu edges + %llu breakpoints in %.2fs -> %llu mismatch%s\n",
                (unsigned long long)m, (unsigned long long)nbp, now_s() - t2,
                (unsigned long long)bad, bad == 1 ? "" : "es");
        if (bad) {
            // A .kcsb that failed its own re-read must not be left where a later step, or a
            // resume that only tests for existence, could pick it up.
            if (unlink(outpath.c_str()) != 0) perror("unlink bad .kcsb");
            else fprintf(stderr, "kcs_pack: removed %s (failed its own re-read)\n", outpath.c_str());
            return 3;
        }
    }
    // The results row is the only thing a sweep keeps.  stdout is nearly always a redirect
    // into a results file, so losing this write loses the row while the run reports success.
    printf("KCSBCSV,%s,%llu,%llu,%u,%lld,%llu,%u,%u,%u,%u,%u\n", outpath.c_str(),
           (unsigned long long)m, (unsigned long long)nbp, KMAX, DMAX,
           (unsigned long long)fsize, w_t, w_base, w_off, w_d, w_v);
    if (fflush(stdout) != 0 || ferror(stdout)) {
        fprintf(stderr, "kcs_pack: FATAL could not write the KCSBCSV results row to stdout: %s\n",
                strerror(errno));
        return 1;
    }
    return 0;
}
