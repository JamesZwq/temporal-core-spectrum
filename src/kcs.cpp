// KCS — Kinetic Certificate Sweep for the complete temporal (k,Δ)-core coreness spectrum.
//
//   - seed x = c_inf (Lemma 1.1 static multigraph core when Dmax >= Delta_core*)
//   - top-down sweep drives the chaotic iteration of Thm 5.5; Cor 5.6 exactness
//   - Stage 0 (--hist): histogram scheduling (Thm 6.2 engine, ground-truth anchor)
//   - --nofilter: the same histogram sweep, but every causally affected edge is
//     recomputed; no support-change test filters the work set.
//   - Stage 1 (default): kinetic certificates — per-side order-statistic timers +
//     two-trigger pessimistic certificates (adaptive eighths split a+b=r),
//     occurrence structures O_{x,h}, budget forests T_{x,h},
//     batch-and-boundary discipline (spectrum-theory Sec 6.2).
//   - --relax: exact conservative certificates with a square-root drop-loss margin, raw
//     flank timers, and direct per-side drop counters (no budget-forest AVL tree and
//     no live rank-space timer selection).
//
// The loader, CSR, exact peeling oracle, and outputs keep engine_current's
// mathematical semantics while adding checked bounds and production I/O guards.
// Outputs are an atomic text index (--dump) and/or a self-contained KCSSTRM2 log
// (--stream), which stream_tool finalizes into query-ready KCSIDX3.
//
// Usage: ./kcs <graph> [Dmax] [floor] [--dump] [--stream <KCSSTRM2>]
//        [--nofilter|--hist|--relax|--recompute] [--seedcheck] [--qcheck] [--novalidate]
//
// Instrumentation (kcs: lines): the certificate scheduler's total structure
// touches are WORK = arms + wakes + h_drop_units + occ_counts + occ_deletes +
// range_adds + timer_probes.  Occurrence/budget touches are O(log m); the indexed
// radix timer contributes O(W) amortized per installation for W-bit timestamps.
// Compare against engine_current's
// removals (= 2E departure events, the Theta(Sigma deg^2) stream) plus
// member_updates (= Sigma b_e * xi_e drop fan-out): KCS replaces the first with
// order-statistic timer wakes and the second with lazy budget-forest range
// decrements.  Its change-sensitive bound is parameterized by E_evt + B + C_tot
// + H_tot; it does not claim that E_evt is universally below Sigma deg^2.
// In --relax, WORK additionally includes direct drop candidate checks (each O(1)).
//
// Build profiles:
//   release (default):  clang++ -std=c++17 -O3 -DNDEBUG kcs.cpp -o kcs
//   paper profiling:    clang++ -std=c++17 -O3 -DNDEBUG -DKCS_PROFILE kcs.cpp -o kcs-profile
// KCS_PROFILE enables the fine-grained phase clock.  Stable work counters remain
// available in both builds because the artifact scripts consume them.

#include "kcs_stream_format.hpp"

#include <algorithm>
#include <cassert>
#include <cerrno>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <new>
#include <queue>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <sys/stat.h>
#include <sys/types.h>
#include <type_traits>
#include <unordered_map>
#include <unistd.h>
#include <utility>
#include <vector>

using NodeId = int32_t;
// The loader admits at most (INT32_MAX+1)/2 edges because a signed int32 side id
// encodes 2*edge+side.  Edge ids therefore fit uint32_t with ample headroom.
using EdgeId = uint32_t;
using Time = int64_t;
using TreeNode = int32_t;

// Convert an algorithmically non-negative signed rank/index only at the point
// where it crosses into a container index.  The assertion documents and checks
// the proof obligation in debug builds; release builds compile to the cast alone.
static inline size_t nonnegative_index(int value) noexcept {
    assert(value >= 0);
    return static_cast<size_t>(value);
}

// Budget-forest nodes are side ids with -1 reserved as the null sentinel.  The
// loader's edge-count guard proves every real side id fits TreeNode; keep the one
// narrowing conversion here rather than duplicating casts throughout the AVL.
static inline TreeNode side_tree_node(uint32_t side_id) noexcept {
    assert(side_id <= static_cast<uint32_t>(std::numeric_limits<TreeNode>::max()));
    return static_cast<TreeNode>(side_id);
}

// ---- KCS_ALLOCLOG: env-gated allocation-size logger (stderr only; no effect on
// the computed index).  Prints one line per major builder allocation so a run on a
// huge graph reveals exactly which allocation requests how many elems/bytes, and
// whether that count is a plausible O(m)/O(C0) value or an absurd (overflowed) one.
// Bytes are computed in 128-bit so an overflowed elem count shows its true scale
// instead of silently wrapping in the print itself. ----
static bool env_enabled(const char* name) noexcept { return std::getenv(name) != nullptr; }
static const bool g_alloclog = env_enabled("KCS_ALLOCLOG");
static inline void alloc_log(const char* name, size_t elems, size_t elem_size) {
    if (!g_alloclog) return;
    unsigned __int128 bytes = static_cast<unsigned __int128>(elems) * elem_size;
    double gb = static_cast<double>(bytes) / 1073741824.0;
    std::cerr << "ALLOC " << name << " elems=" << elems
              << " elem_size=" << elem_size << " GB=" << gb << "\n";
}

// ---- KCS_CENSUS: env-gated xi-binned wake census (pure instrumentation; no effect
// on the computed index, arming, wake order or the fixpoint).  Buckets every
// verify_side wake by xi_s = the side's window size at Dmax and accumulates counts,
// false alarms and cycles (verify entry through the nested re-arm), plus a
// same-(side,radius) repeat counter.  Decides whether small-window sides dominate
// the wake cost (per-side eager/lazy hybrid lever) and sizes wake coalescing. ----
static const bool g_census = env_enabled("KCS_CENSUS");
// ---- KCS_CORNER: opt into the exact-timer split corner ssplit=0 (and its
// symmetric ssplit=8 counterpart).  It changes certificate wake timing only;
// the a+b=r never-late invariant and the output staircase are unchanged. ----
static const bool g_corner = env_enabled("KCS_CORNER");

// ---- KCS_PROPSPLIT: opt into a one-step, observed-spend split update after a
// false alarm.  Off preserves the historical +/-1 trigger-direction drift. ----
static const bool g_propsplit = env_enabled("KCS_PROPSPLIT");

// ---- KCS_FADIAG: strict-sweep false-alarm accounting.  All storage updates are
// guarded so an unset flag performs no diagnostic allocation or counting. ----
static const bool g_fadiag = env_enabled("KCS_FADIAG");
static inline uint32_t eager_xi_limit() {
    const char* s = std::getenv("KCS_EAGER_XI");
    if (!s || !*s) return 0;
    char* end = nullptr;
    long long v = std::strtoll(s, &end, 10);
    if (*end || v < 0) {
        throw std::invalid_argument("KCS_EAGER_XI must be a non-negative integer");
    }
    return v > static_cast<long long>(std::numeric_limits<uint32_t>::max())
               ? std::numeric_limits<uint32_t>::max() : static_cast<uint32_t>(v);
}
static inline uint64_t census_now() {   // cheap cycle counter; ratios matter, not ns
#if defined(__aarch64__)
    uint64_t v;
    __asm__ __volatile__("mrs %0, cntvct_el0" : "=r"(v));
    return v;
#elif defined(__x86_64__) || defined(__i386__)
    return __builtin_ia32_rdtsc();
#else
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count());
#endif
}

struct Timer {
    void start() {
        time_point = std::chrono::steady_clock::now();
    }

    double stop() const {
        auto finish = std::chrono::steady_clock::now();
        std::chrono::duration<double> elapsed = finish - time_point;
        return elapsed.count();
    }

private:
    std::chrono::steady_clock::time_point time_point{};
};

struct Edge {
    NodeId u{};
    NodeId v{};
    Time t{};
};
static_assert(sizeof(Edge) == 16, "Packed Edge must stay 16 bytes");

struct Incidence {
    Time t{};
    uint32_t edge{};
    uint8_t side{};   // 0 = incidence at edges[edge].u, 1 = at edges[edge].v
};
static_assert(sizeof(Incidence) <= 16, "Incidence must stay <= 16 bytes");

enum class GraphLineKind : uint8_t { Skip, CountHeader, Edge };

struct ParsedGraphLine {
    GraphLineKind kind = GraphLineKind::Skip;
    int64_t u = 0;
    int64_t v = 0;
    int64_t t = 0;
};

bool ascii_space(char ch) {
    return ch == ' ' || ch == '\t' || ch == '\r' || ch == '\n' ||
           ch == '\f' || ch == '\v';
}

ParsedGraphLine parse_graph_line(const std::string& line) {
    const char* p = line.data();
    const char* const finish = p + line.size();
    auto skip_space = [&]() {
        while (p != finish && ascii_space(*p)) ++p;
    };
    auto parse_i64 = [&](const char* field) {
        skip_space();
        if (p == finish || *p == '#' || *p == '%')
            throw std::invalid_argument(std::string("missing ") + field);
        char* end = nullptr;
        errno = 0;
        const int64_t value = std::strtoll(p, &end, 10);
        if (errno == ERANGE)
            throw std::out_of_range(std::string(field) + " exceeds signed 64-bit range");
        if (end == p)
            throw std::invalid_argument(std::string("invalid ") + field);
        if (end != finish && !ascii_space(*end))
            throw std::invalid_argument(std::string("invalid delimiter after ") + field);
        p = end;
        return value;
    };

    skip_space();
    if (p == finish || *p == '#' || *p == '%') return {};

    ParsedGraphLine parsed;
    parsed.u = parse_i64("first column");
    skip_space();
    if (p == finish || *p == '#' || *p == '%') {
        if (parsed.u < 0)
            throw std::invalid_argument("leading count header must be non-negative");
        parsed.kind = GraphLineKind::CountHeader;
        return parsed;
    }
    parsed.v = parse_i64("second column");
    skip_space();
    if (p == finish || *p == '#' || *p == '%')
        throw std::invalid_argument("edge record is missing its timestamp");
    parsed.t = parse_i64("timestamp");
    // Preserve the established dataset convention that optional columns after
    // (u,v,t), such as Enron's fourth integer, are ignored.  Requiring whitespace
    // after t still rejects accidental tokens such as "123x".
    parsed.kind = GraphLineKind::Edge;
    return parsed;
}

bool parse_time_arg(const std::string& text, Time& value) {
    try {
        size_t used = 0;
        value = static_cast<Time>(std::stoll(text, &used, 10));
        return used == text.size();
    } catch (const std::exception&) {
        return false;
    }
}

struct NodeIdManager {
    NodeId get(int64_t external_id) {
        auto it = ids.find(external_id);
        if (it != ids.end()) {
            return it->second;
        }
        // node_count itself is a NodeId and several loops increment a NodeId up to
        // (but not through) node_count, so leave INT32_MAX as the exclusive bound.
        if (ids.size() >= static_cast<size_t>(std::numeric_limits<NodeId>::max())) {
            throw std::overflow_error("Node count exceeds signed 32-bit encoding");
        }
        NodeId next = static_cast<NodeId>(ids.size());
        ids.emplace(external_id, next);
        return next;
    }

    size_t size() const {
        return ids.size();
    }

    std::unordered_map<int64_t, NodeId> ids;
};

struct CsrIncidence {
    std::vector<Incidence> incidences;
    std::vector<size_t> offsets;
    std::vector<int> bit;

    size_t node_begin(NodeId node) const {
        return offsets[static_cast<size_t>(node)];
    }

    size_t node_end(NodeId node) const {
        return offsets[static_cast<size_t>(node) + 1];
    }

    int node_size(NodeId node) const {
        return static_cast<int>(node_end(node) - node_begin(node));
    }

    Time time_at(NodeId node, int local) const {
        return incidences[node_begin(node) + static_cast<size_t>(local)].t;
    }

    int lower_time(NodeId node, Time value) const {
        int lo = 0;
        int hi = node_size(node);
        while (lo < hi) {
            int mid = lo + (hi - lo) / 2;
            if (time_at(node, mid) < value) {
                lo = mid + 1;
            } else {
                hi = mid;
            }
        }
        return lo;
    }

    int upper_time(NodeId node, Time value) const {
        int lo = 0;
        int hi = node_size(node);
        while (lo < hi) {
            int mid = lo + (hi - lo) / 2;
            if (time_at(node, mid) <= value) {
                lo = mid + 1;
            } else {
                hi = mid;
            }
        }
        return lo;
    }

    void build_fenwick() {
        bit.resize(incidences.size());
        for (size_t node = 0; node + 1 < offsets.size(); ++node) {
            size_t base = offsets[node];
            int len = static_cast<int>(offsets[node + 1] - base);
            for (int i = 1; i <= len; ++i) {
                // A Fenwick tree over an all-one array has the closed form
                // tree[i] = lowbit(i); fill it directly instead of first writing
                // all ones and then performing a second parent-accumulation pass.
                bit[base + static_cast<size_t>(i - 1)] = i & -i;
            }
        }
    }

    int prefix_sum(NodeId node, int count) const {
        size_t base = node_begin(node);
        int sum = 0;
        for (int i = count; i > 0; i -= i & -i) {
            sum += bit[base + static_cast<size_t>(i - 1)];
        }
        return sum;
    }

    int range_sum(NodeId node, int lo, int hi) const {
        return prefix_sum(node, hi) - prefix_sum(node, lo);
    }

    void add(NodeId node, int local, int delta) {
        size_t base = node_begin(node);
        int len = node_size(node);
        for (int i = local + 1; i <= len; i += i & -i) {
            bit[base + static_cast<size_t>(i - 1)] += delta;
        }
    }

    int select_active(NodeId node, int target_rank) const {
        size_t base = node_begin(node);
        int len = node_size(node);
        int idx = 0;
        int acc = 0;
        int step = 1;
        while ((step << 1) <= len) {
            step <<= 1;
        }
        for (; step > 0; step >>= 1) {
            int next = idx + step;
            if (next <= len && acc + bit[base + static_cast<size_t>(next - 1)] < target_rank) {
                idx = next;
                acc += bit[base + static_cast<size_t>(next - 1)];
            }
        }
        return idx;
    }
};

struct Graph {
    NodeId node_count{};
    std::vector<Edge> edges;
    CsrIncidence csr;
    // Interleaved by side id: [pos(e0,u), pos(e0,v), pos(e1,u), ...].
    // The side-id guard below also guarantees that every CSR position fits uint32_t.
    std::vector<uint32_t> incidence_pos;
};

Graph load_graph(const std::string& filename) {
    std::ifstream input(filename);
    if (!input.is_open()) {
        throw std::runtime_error("Could not open file " + filename);
    }

    std::vector<Edge> keys;
    NodeIdManager node_ids;
    node_ids.ids.reserve(1 << 20);

    std::string line;
    size_t line_no = 0;
    bool saw_count_header = false;
    bool saw_edge_record = false;
    while (std::getline(input, line)) {
        ++line_no;
        ParsedGraphLine parsed;
        try {
            parsed = parse_graph_line(line);
        } catch (const std::exception& ex) {
            throw std::invalid_argument(filename + ":" + std::to_string(line_no) + ": " + ex.what());
        }
        if (parsed.kind == GraphLineKind::Skip) continue;
        if (parsed.kind == GraphLineKind::CountHeader) {
            if (saw_count_header || saw_edge_record)
                throw std::invalid_argument(filename + ":" + std::to_string(line_no) +
                                            ": a count header is allowed only once, before all edges");
            saw_count_header = true;
            continue;
        }
        saw_edge_record = true;
        const int64_t raw_u = parsed.u;
        const int64_t raw_v = parsed.v;
        const int64_t raw_t = parsed.t;
        if (raw_t < 0) {
            throw std::invalid_argument(filename + ":" + std::to_string(line_no) +
                                        ": negative timestamps are unsupported; shift the input time origin to zero");
        }
        if (raw_u == raw_v) {
            continue;
        }

        NodeId u = node_ids.get(raw_u);
        NodeId v = node_ids.get(raw_v);
        if (u > v) {
            std::swap(u, v);
        }
        keys.push_back(Edge{u, v, static_cast<Time>(raw_t)});
    }
    if (input.bad()) throw std::runtime_error("I/O error while reading " + filename);

    NodeId node_count = static_cast<NodeId>(node_ids.size());
    std::unordered_map<int64_t, NodeId>().swap(node_ids.ids);

    std::sort(keys.begin(), keys.end(), [](const Edge& a, const Edge& b) {
        return std::tie(a.t, a.u, a.v) < std::tie(b.t, b.u, b.v);
    });
    keys.erase(std::unique(keys.begin(), keys.end(), [](const Edge& a, const Edge& b) {
        return a.t == b.t && a.u == b.u && a.v == b.v;
    }), keys.end());

    // A side id is 2*edge+side and budget-tree nodes store that id in int32_t.
    // Guard before narrowing edge ids or CSR positions.
    constexpr uint64_t kMaxSideCount =
        static_cast<uint64_t>(std::numeric_limits<int32_t>::max()) + 1u;
    if (keys.size() > static_cast<size_t>(kMaxSideCount / 2u)) {
        throw std::overflow_error("Edge count exceeds signed 32-bit side/tree encoding");
    }

    Graph graph;
    graph.node_count = node_count;
    graph.edges = std::move(keys);

    alloc_log("edges", graph.edges.capacity(), sizeof(Edge));
    alloc_log("csr.offsets", static_cast<size_t>(graph.node_count) + 1, sizeof(size_t));
    graph.csr.offsets.assign(static_cast<size_t>(graph.node_count) + 1, 0);
    for (const Edge& edge : graph.edges) {
        ++graph.csr.offsets[static_cast<size_t>(edge.u) + 1];
        ++graph.csr.offsets[static_cast<size_t>(edge.v) + 1];
    }
    for (size_t i = 1; i < graph.csr.offsets.size(); ++i) {
        graph.csr.offsets[i] += graph.csr.offsets[i - 1];
    }
    alloc_log("csr.incidences", graph.edges.size() * 2, sizeof(Incidence));
    graph.csr.incidences.resize(graph.edges.size() * 2);
    {
        std::vector<size_t> cursor(graph.csr.offsets.begin(), graph.csr.offsets.end() - 1);
        for (size_t edge_id = 0; edge_id < graph.edges.size(); ++edge_id) {
            const Edge& edge = graph.edges[edge_id];
            graph.csr.incidences[cursor[static_cast<size_t>(edge.u)]++] =
                Incidence{edge.t, static_cast<uint32_t>(edge_id), 0};
            graph.csr.incidences[cursor[static_cast<size_t>(edge.v)]++] =
                Incidence{edge.t, static_cast<uint32_t>(edge_id), 1};
        }
    }

    alloc_log("incidence_pos", graph.edges.size() * 2, sizeof(uint32_t));
    graph.incidence_pos.assign(graph.edges.size() * 2, std::numeric_limits<uint32_t>::max());
    for (size_t pos = 0; pos < graph.csr.incidences.size(); ++pos) {
        const Incidence& incidence = graph.csr.incidences[pos];
        size_t sid = 2 * static_cast<size_t>(incidence.edge) + incidence.side;
        graph.incidence_pos[sid] = static_cast<uint32_t>(pos);
    }

    return graph;
}

Time lower_window(Time t, Time delta) {
    return delta > t ? Time{0} : t - delta;
}

// Timestamps and radii are non-negative.  Saturating the upper endpoint preserves
// the mathematical [t-Delta,t+Delta] window at the edge of the Time domain and
// avoids signed-overflow UB in every upper_bound call.
Time upper_window(Time t, Time delta) {
    const Time hi = std::numeric_limits<Time>::max();
    return delta > hi - t ? hi : t + delta;
}

// ===== exact peeling at a fixed Delta (verbatim from engine_current; used as the
// seed fallback when Dmax < Delta_core*, and as the floor-oracle sanity check) =====
std::vector<uint32_t> compute_fast_kdelta_core(Graph& graph, Time delta) {
    size_t m = graph.edges.size();
    std::vector<int> endpoint_count_u(m, 0);
    std::vector<int> endpoint_count_v(m, 0);
    std::vector<uint32_t> degree(m, 0);
    std::vector<uint8_t> active_edge(m, 1);

    for (NodeId node = 0; node < graph.node_count; ++node) {
        size_t begin = graph.csr.node_begin(node);
        size_t end = graph.csr.node_end(node);
        int len = static_cast<int>(end - begin);
        int left = 0;
        int right = 0;

        for (int local = 0; local < len; ++local) {
            const Incidence& incidence = graph.csr.incidences[begin + static_cast<size_t>(local)];
            Time lo = lower_window(incidence.t, delta);
            Time hi = upper_window(incidence.t, delta);
            while (left < len && graph.csr.incidences[begin + static_cast<size_t>(left)].t < lo) {
                ++left;
            }
            while (right < len && graph.csr.incidences[begin + static_cast<size_t>(right)].t <= hi) {
                ++right;
            }
            int count = right - left;
            if (incidence.side == 0) {
                endpoint_count_u[static_cast<size_t>(incidence.edge)] = count;
            } else {
                endpoint_count_v[static_cast<size_t>(incidence.edge)] = count;
            }
        }
    }

    uint32_t max_degree = 0;
    for (size_t edge = 0; edge < m; ++edge) {
        degree[edge] = static_cast<uint32_t>(std::min(endpoint_count_u[edge], endpoint_count_v[edge]));
        max_degree = std::max(max_degree, degree[edge]);
    }

    std::vector<size_t> bins(static_cast<size_t>(max_degree) + 1, 0);
    for (uint32_t value : degree) {
        ++bins[value];
    }
    size_t start = 0;
    for (size_t deg = 0; deg < bins.size(); ++deg) {
        size_t count = bins[deg];
        bins[deg] = start;
        start += count;
    }

    std::vector<EdgeId> sorted_edges(m, 0);
    std::vector<size_t> position(m, 0);
    for (size_t edge = 0; edge < m; ++edge) {
        position[edge] = bins[degree[edge]];
        sorted_edges[position[edge]] = static_cast<EdgeId>(edge);
        ++bins[degree[edge]];
    }
    for (size_t deg = bins.size() - 1; deg > 0; --deg) {
        bins[deg] = bins[deg - 1];
    }
    bins[0] = 0;

    auto move_to_lower_bucket = [&](EdgeId edge, uint32_t old_degree) {
        size_t edge_pos = position[static_cast<size_t>(edge)];
        size_t swap_pos = bins[old_degree];
        EdgeId other = sorted_edges[swap_pos];
        if (other != edge) {
            sorted_edges[edge_pos] = other;
            sorted_edges[swap_pos] = edge;
            position[static_cast<size_t>(other)] = edge_pos;
            position[static_cast<size_t>(edge)] = swap_pos;
        }
        ++bins[old_degree];
    };

    auto process_endpoint = [&](NodeId node, Time t, uint32_t current_degree) {
        Time lo_time = lower_window(t, delta);
        Time hi_time = upper_window(t, delta);
        int lo = graph.csr.lower_time(node, lo_time);
        int hi = graph.csr.upper_time(node, hi_time);

        auto update_incidence = [&](const Incidence& incidence) {
            EdgeId affected = incidence.edge;
            size_t affected_index = static_cast<size_t>(affected);
            if (!active_edge[affected_index]) {
                return;
            }
            if (degree[affected_index] <= current_degree) {
                return;
            }

            uint32_t old_degree = degree[affected_index];
            if (incidence.side == 0) {
                --endpoint_count_u[affected_index];
            } else {
                --endpoint_count_v[affected_index];
            }

            uint32_t new_degree = static_cast<uint32_t>(std::min(endpoint_count_u[affected_index], endpoint_count_v[affected_index]));
            degree[affected_index] = new_degree;
            if (new_degree < old_degree) {
                move_to_lower_bucket(affected, old_degree);
            }
        };

        int raw_count = hi - lo;
        if (raw_count <= 128) {
            size_t begin = graph.csr.node_begin(node);
            for (int local = lo; local < hi; ++local) {
                update_incidence(graph.csr.incidences[begin + static_cast<size_t>(local)]);
            }
            return;
        }

        int prefix_before = graph.csr.prefix_sum(node, lo);
        int active_count = graph.csr.range_sum(node, lo, hi);
        if (raw_count <= active_count * 6 + 32) {
            size_t begin = graph.csr.node_begin(node);
            for (int local = lo; local < hi; ++local) {
                update_incidence(graph.csr.incidences[begin + static_cast<size_t>(local)]);
            }
        } else {
            for (int offset = 1; offset <= active_count; ++offset) {
                int local = graph.csr.select_active(node, prefix_before + offset);
                update_incidence(graph.csr.incidences[graph.csr.node_begin(node) + static_cast<size_t>(local)]);
            }
        }
    };

    for (size_t i = 0; i < m; ++i) {
        EdgeId edge_id = sorted_edges[i];
        const Edge& edge = graph.edges[static_cast<size_t>(edge_id)];
        uint32_t current_degree = degree[static_cast<size_t>(edge_id)];

        process_endpoint(edge.u, edge.t, current_degree);
        process_endpoint(edge.v, edge.t, current_degree);

        size_t sid = 2 * static_cast<size_t>(edge_id);
        size_t pos_u = graph.incidence_pos[sid];
        size_t pos_v = graph.incidence_pos[sid + 1];
        active_edge[static_cast<size_t>(edge_id)] = 0;
        graph.csr.add(edge.u, static_cast<int>(pos_u - graph.csr.node_begin(edge.u)), -1);
        graph.csr.add(edge.v, static_cast<int>(pos_v - graph.csr.node_begin(edge.v)), -1);
    }

    return degree;
}

// ===== Lemma 1.1 seed: c_inf(e) = min(kappa(u), kappa(v)) where kappa = static
// multigraph core numbers (parallel temporal edges count in degrees). O(m+n). =====
std::vector<uint32_t> static_core_seed(const Graph& graph) {
    size_t n = static_cast<size_t>(graph.node_count);
    size_t m = graph.edges.size();
    alloc_log("seed.deg/vert/pos", n, sizeof(uint32_t) + sizeof(NodeId) + sizeof(size_t));
    alloc_log("seed.c", m, sizeof(uint32_t));
    std::vector<uint32_t> deg(n, 0);
    for (size_t v = 0; v < n; ++v) {
        deg[v] = static_cast<uint32_t>(graph.csr.node_size(static_cast<NodeId>(v)));
    }
    uint32_t maxdeg = 0;
    for (uint32_t d : deg) maxdeg = std::max(maxdeg, d);

    // Batagelj–Zaversnik bucket peeling on the multigraph.
    std::vector<size_t> bin(static_cast<size_t>(maxdeg) + 2, 0);
    for (uint32_t d : deg) ++bin[d];
    size_t start = 0;
    for (size_t d = 0; d < bin.size(); ++d) {
        size_t cnt = bin[d];
        bin[d] = start;
        start += cnt;
    }
    std::vector<NodeId> vert(n);
    std::vector<size_t> pos(n);
    for (size_t v = 0; v < n; ++v) {
        pos[v] = bin[deg[v]];
        vert[pos[v]] = static_cast<NodeId>(v);
        ++bin[deg[v]];
    }
    for (size_t d = bin.size() - 1; d > 0; --d) bin[d] = bin[d - 1];
    bin[0] = 0;

    for (size_t i = 0; i < n; ++i) {
        NodeId v = vert[i];
        size_t begin = graph.csr.node_begin(v);
        size_t end = graph.csr.node_end(v);
        for (size_t p = begin; p < end; ++p) {
            const Incidence& inc = graph.csr.incidences[p];
            const Edge& e = graph.edges[static_cast<size_t>(inc.edge)];
            NodeId w = inc.side == 0 ? e.v : e.u;
            size_t wi = static_cast<size_t>(w);
            if (deg[wi] > deg[static_cast<size_t>(v)]) {
                uint32_t dw = deg[wi];
                size_t pw = pos[wi];
                size_t pfirst = bin[dw];
                NodeId first = vert[pfirst];
                if (first != w) {
                    vert[pw] = first;
                    vert[pfirst] = w;
                    pos[static_cast<size_t>(first)] = pw;
                    pos[wi] = pfirst;
                }
                ++bin[dw];
                --deg[wi];
            }
        }
    }
    // deg[] now holds kappa(v)
    std::vector<uint32_t> c(m, 0);
    for (size_t e = 0; e < m; ++e) {
        const Edge& ed = graph.edges[e];
        c[e] = std::min(deg[static_cast<size_t>(ed.u)], deg[static_cast<size_t>(ed.v)]);
    }
    return c;
}

// ===== monotone radix max-heap (verbatim from engine_current) =====
struct RadixHeapMax {
    struct Entry { Time g; uint32_t pi; uint32_t pj; };
    std::vector<Entry> buckets[64];
    Time last;            // upper bound on every stored key; keys are non-negative
    size_t count = 0;

    explicit RadixHeapMax(Time upper) : last(upper) {}

    int bucket_of(Time g) const {
        uint64_t x = static_cast<uint64_t>(g) ^ static_cast<uint64_t>(last);
        return x == 0 ? 0 : 64 - __builtin_clzll(x);   // <= 63 for non-negative keys
    }
    void push(Time g, uint32_t pi, uint32_t pj) {
        buckets[bucket_of(g)].push_back(Entry{g, pi, pj});
        ++count;
    }
    bool empty() const { return count == 0; }

    Time pop_batch(std::vector<Entry>& out) {
        if (buckets[0].empty()) {
            // Bounded scan (as in BucketQueueMax): if count says entries exist but
            // every bucket is empty, fail loudly instead of reading past buckets[63].
            int i = 1;
            while (i < 64 && buckets[i].empty()) ++i;
            if (i == 64) throw std::logic_error("Radix heap count is inconsistent");
            Time mx = buckets[i][0].g;
            for (const Entry& e : buckets[i]) mx = std::max(mx, e.g);
            last = mx;                                  // monotone: keys only shrink
            std::vector<Entry> moved;
            moved.swap(buckets[i]);
            for (const Entry& e : moved) buckets[bucket_of(e.g)].push_back(e);
        }
        out.clear();
        out.swap(buckets[0]);
        count -= out.size();
        return last;
    }
};

struct Drop {
    Time delta{};
    union { uint32_t edge; uint32_t next; };
    uint32_t value{};
    Drop(EdgeId edge_id, Time delta_up, uint32_t value_above) noexcept
        : delta(delta_up), edge(edge_id), value(value_above) {}
};
static_assert(sizeof(Drop) == 16, "Drop must stay 16 bytes");
static_assert(std::is_trivially_copyable<Drop>::value, "Drop must remain trivially copyable");

// Payload blocks never move: allocated records are in [size,size+16383].
// Only the tiny block-pointer directory grows geometrically.
template <class T, size_t BlockShift = 14>
class ExactChunkLog {
public:
    static constexpr size_t BLOCK_SIZE = size_t{1} << BlockShift;
    static constexpr size_t BLOCK_MASK = BLOCK_SIZE - 1;
    static_assert(BlockShift < std::numeric_limits<size_t>::digits);
    static_assert(std::is_trivially_destructible<T>::value);
    ExactChunkLog() = default;
    ExactChunkLog(const ExactChunkLog&) = delete;
    ExactChunkLog& operator=(const ExactChunkLog&) = delete;
    ~ExactChunkLog() { for (T* block : blocks_) ::operator delete[](block); }

    void reserve_records(size_t target) {
        if (target <= capacity_records()) return;
        if (target > std::numeric_limits<size_t>::max() - BLOCK_MASK)
            throw std::overflow_error("chunk-log capacity overflow");
        const size_t wanted_blocks = (target + BLOCK_MASK) >> BlockShift;
        while (blocks_.size() < wanted_blocks) allocate_block();
    }
    void reserve_for_one() {
        if (size_ == std::numeric_limits<size_t>::max())
            throw std::overflow_error("chunk-log size overflow");
        reserve_records(size_ + 1);
    }

    template <class... Args>
    size_t emplace_back(Args&&... args) {
        reserve_for_one();
        return emplace_back_reserved(std::forward<Args>(args)...);
    }
    template <class... Args>
    size_t emplace_back_reserved(Args&&... args) {
        assert(size_ < capacity_records());
        const size_t offset = size_ & BLOCK_MASK;
        T* slot = blocks_[size_ >> BlockShift] + offset;
        ::new (static_cast<void*>(slot)) T(std::forward<Args>(args)...);
        return size_++;
    }
    T& operator[](size_t i) { return blocks_[i >> BlockShift][i & BLOCK_MASK]; }
    const T& operator[](size_t i) const { return blocks_[i >> BlockShift][i & BLOCK_MASK]; }
    size_t size() const { return size_; }
    size_t capacity_records() const {
        assert(blocks_.size() <= std::numeric_limits<size_t>::max() / BLOCK_SIZE);
        return blocks_.size() * BLOCK_SIZE;
    }
    size_t allocated_bytes() const {
        return capacity_records() * sizeof(T) + blocks_.capacity() * sizeof(T*);
    }
    void swap(ExactChunkLog& other) noexcept {
        blocks_.swap(other.blocks_);
        std::swap(size_, other.size_);
    }

private:
    void allocate_block() {
        T* block = static_cast<T*>(::operator new[](BLOCK_SIZE * sizeof(T)));
        try { blocks_.push_back(block); }
        catch (...) { ::operator delete[](block); throw; }
    }
    std::vector<T*> blocks_;
    size_t size_ = 0;
};

enum class DropOrder { DescendingRadius, AscendingRadius };

// Normal records reuse edge as a 32-bit next link.  The narrow domain holds
// exactly UINT32_MAX records (indices 0..UINT32_MAX-1); before creating index
// UINT32_MAX, old links migrate once to an exact-chunk size_t sidecar.
class DropLog {
public:
    DropLog(size_t edge_count, DropOrder order) : edge_count_(edge_count), order_(order) {
#if defined(KCS_TEST_WIDE_DROP_LINKS)
        wide_ = true;
#endif
    }
    DropLog(const DropLog&) = delete;
    DropLog& operator=(const DropLog&) = delete;

    void emplace_back(EdgeId edge, Time delta, uint32_t value) {
        const size_t edge_index = static_cast<size_t>(edge);
        if (edge_index >= edge_count_)
            throw std::logic_error("breakpoint edge id out of range");
        if (has_last_ &&
            ((order_ == DropOrder::DescendingRadius && delta > last_delta_) ||
             (order_ == DropOrder::AscendingRadius && delta < last_delta_)))
            throw std::logic_error("breakpoint producer violated monotone radius order");
        ensure_initialized();

        // A radius can trigger several internal changes for the same edge.
        // Because the producer is globally radius-monotone, that edge's newest
        // record is the only possible equal-radius record.  Coalesce it before
        // the narrow-link limit check so duplicate events neither consume B
        // space nor force a pointless 32-bit-to-size_t link migration.
        const size_t newest = wide_
            ? (order_ == DropOrder::DescendingRadius ? head_wide_[edge_index]
                                                     : tail_wide_[edge_index])
            : static_cast<size_t>(order_ == DropOrder::DescendingRadius
                                      ? head32_[edge_index]
                                      : tail32_[edge_index]);
        if (newest != sentinel() && records_[newest].delta == delta) {
            records_[newest].value = std::max(records_[newest].value, value);
            has_last_ = true;
            last_delta_ = delta;
            return;
        }

        if (!wide_ && records_.size() == NARROW_LIMIT) upgrade_to_wide();
        if (wide_) {
            // Reserve both logs before mutating either logical size.  If either
            // allocation fails, retrying the append cannot desynchronize them.
            records_.reserve_for_one();
            next_wide_.reserve_for_one();
            const size_t next = order_ == DropOrder::DescendingRadius
                                    ? head_wide_[edge_index]
                                    : WIDE_END;
            const size_t index = records_.emplace_back_reserved(edge, delta, value);
            if (next_wide_.emplace_back_reserved(next) != index)
                throw std::logic_error("wide breakpoint sidecar desynchronized");
            if (order_ == DropOrder::DescendingRadius) head_wide_[edge_index] = index;
            else {
                if (tail_wide_[edge_index] == WIDE_END) head_wide_[edge_index] = index;
                else next_wide_[tail_wide_[edge_index]] = index;
                tail_wide_[edge_index] = index;
            }
        } else {
            records_.reserve_for_one();
            const size_t index = records_.emplace_back_reserved(edge, delta, value);
            if (order_ == DropOrder::DescendingRadius) {
                records_[index].next = head32_[edge_index];
                head32_[edge_index] = static_cast<uint32_t>(index);
            } else {
                records_[index].next = END32;
                if (tail32_[edge_index] == END32)
                    head32_[edge_index] = static_cast<uint32_t>(index);
                else
                    records_[tail32_[edge_index]].next = static_cast<uint32_t>(index);
                tail32_[edge_index] = static_cast<uint32_t>(index);
            }
        }
        has_last_ = true;
        last_delta_ = delta;
    }

    size_t size() const { return records_.size(); }
    const Drop& operator[](size_t i) const { return records_[i]; }
    size_t first(size_t edge) const {
        if (!initialized_) return sentinel();
        return wide_ ? head_wide_[edge] : static_cast<size_t>(head32_[edge]);
    }
    size_t sentinel() const { return wide_ ? WIDE_END : static_cast<size_t>(END32); }
    size_t following(size_t i) const {
        return wide_ ? next_wide_[i] : static_cast<size_t>(records_[i].next);
    }
    bool wide() const { return wide_; }
    size_t allocated_bytes() const {
        return records_.allocated_bytes() + next_wide_.allocated_bytes() +
               (head32_.capacity() + tail32_.capacity()) * sizeof(uint32_t) +
               (head_wide_.capacity() + tail_wide_.capacity()) * sizeof(size_t);
    }

private:
    static constexpr uint32_t END32 = std::numeric_limits<uint32_t>::max();
    static constexpr size_t WIDE_END = std::numeric_limits<size_t>::max();
#if defined(KCS_TEST_WIDE_AFTER)
    static constexpr size_t NARROW_LIMIT = KCS_TEST_WIDE_AFTER;
    static_assert(NARROW_LIMIT > 0 && NARROW_LIMIT <= static_cast<size_t>(END32));
#else
    static constexpr size_t NARROW_LIMIT = static_cast<size_t>(END32);
#endif
    void ensure_initialized() {
        if (initialized_) return;
        if (wide_) {
            head_wide_.assign(edge_count_, WIDE_END);
            if (order_ == DropOrder::AscendingRadius) tail_wide_.assign(edge_count_, WIDE_END);
        } else {
            head32_.assign(edge_count_, END32);
            if (order_ == DropOrder::AscendingRadius) tail32_.assign(edge_count_, END32);
        }
        initialized_ = true;
    }
    void upgrade_to_wide() {
        // Construct the entire wide representation off to the side.  Every
        // allocation can fail without changing the live narrow links; once all
        // storage exists, filling and the final swaps are non-throwing.
        std::vector<size_t> new_head(edge_count_, WIDE_END);
        for (size_t e = 0; e < edge_count_; ++e)
            new_head[e] = head32_[e] == END32 ? WIDE_END : head32_[e];
        std::vector<size_t> new_tail;
        if (order_ == DropOrder::AscendingRadius) {
            new_tail.resize(edge_count_);
            for (size_t e = 0; e < edge_count_; ++e)
                new_tail[e] = tail32_[e] == END32 ? WIDE_END : tail32_[e];
        }
        ExactChunkLog<size_t> new_next;
        new_next.reserve_records(records_.size());
        for (size_t i = 0; i < records_.size(); ++i) {
            const uint32_t next = records_[i].next;
            new_next.emplace_back_reserved(next == END32 ? WIDE_END
                                                          : static_cast<size_t>(next));
        }
        head_wide_.swap(new_head);
        tail_wide_.swap(new_tail);
        next_wide_.swap(new_next);
        std::vector<uint32_t>().swap(head32_);
        std::vector<uint32_t>().swap(tail32_);
        wide_ = true;
    }

    size_t edge_count_;
    DropOrder order_;
    ExactChunkLog<Drop> records_;
    ExactChunkLog<size_t> next_wide_;
    std::vector<uint32_t> head32_, tail32_;
    std::vector<size_t> head_wide_, tail_wide_;
    bool wide_ = false, initialized_ = false, has_last_ = false;
    Time last_delta_ = 0;
};

static std::filesystem::path normalized_path(const std::string& path) {
    std::error_code ec;
    std::filesystem::path absolute = std::filesystem::absolute(path, ec);
    if (ec) throw std::runtime_error("cannot resolve path '" + path + "': " + ec.message());
    std::filesystem::path parent = std::filesystem::weakly_canonical(absolute.parent_path(), ec);
    if (ec) throw std::runtime_error("cannot resolve parent of '" + path + "': " + ec.message());
    return parent / absolute.filename();
}

// Two nonexistent leaf names can still denote the same future file on a
// case-folding or Unicode-normalizing filesystem.  Ask the filesystem itself:
// create a unique suffixed sibling of the first name, then stat the identically
// suffixed second name.  Final output names are never touched or made visible.
static bool nonexistent_leaves_alias(const std::filesystem::path& first,
                                     const std::filesystem::path& second) {
    if (first.parent_path() != second.parent_path()) {
        std::error_code ec;
        const bool same_parent = std::filesystem::equivalent(
            first.parent_path(), second.parent_path(), ec);
        if (ec) throw std::runtime_error("cannot compare output parent directories: " +
                                         ec.message());
        if (!same_parent) return false;
    }
    for (unsigned attempt = 0; attempt < 32; ++attempt) {
        const std::string suffix = ".p" +
            std::to_string(static_cast<long long>(::getpid())) + "." +
            std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) +
            "." + std::to_string(attempt);
        const std::string probe_first = first.string() + suffix;
        const std::string probe_second = second.string() + suffix;
        const int fd = ::open(probe_first.c_str(),
                              O_RDWR | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
        if (fd < 0) {
            if (errno == EEXIST) continue;
            throw std::runtime_error("cannot probe output-path aliasing: " +
                                     std::string(std::strerror(errno)));
        }
        struct stat created{};
        const int first_stat = ::fstat(fd, &created);
        const int first_stat_error = errno;
        struct stat observed{};
        const int second_stat = ::stat(probe_second.c_str(), &observed);
        const int second_stat_error = errno;
        const int close_result = ::close(fd);
        const int close_error = errno;
        const int unlink_result = ::unlink(probe_first.c_str());
        const int unlink_error = errno;
        if (first_stat != 0)
            throw std::runtime_error("cannot stat alias probe: " +
                                     std::string(std::strerror(first_stat_error)));
        if (close_result != 0)
            throw std::runtime_error("cannot close alias probe: " +
                                     std::string(std::strerror(close_error)));
        if (unlink_result != 0)
            throw std::runtime_error("cannot remove alias probe: " +
                                     std::string(std::strerror(unlink_error)));
        if (second_stat == 0) {
            if (created.st_dev == observed.st_dev && created.st_ino == observed.st_ino)
                return true;
            // A pre-existing unrelated second probe made this attempt ambiguous.
            continue;
        }
        if (second_stat_error == ENOENT) return false;
        throw std::runtime_error("cannot inspect second alias probe: " +
                                 std::string(std::strerror(second_stat_error)));
    }
    throw std::runtime_error("cannot allocate an unambiguous output-path alias probe");
}

static bool paths_alias(const std::string& lhs, const std::string& rhs) {
    const std::filesystem::path a = normalized_path(lhs);
    const std::filesystem::path b = normalized_path(rhs);
    if (a == b) return true;
    std::error_code ec;
    const bool a_exists = std::filesystem::exists(a, ec);
    if (ec) throw std::runtime_error("cannot inspect path '" + lhs + "': " + ec.message());
    const bool b_exists = std::filesystem::exists(b, ec);
    if (ec) throw std::runtime_error("cannot inspect path '" + rhs + "': " + ec.message());
    if (!a_exists && !b_exists) return nonexistent_leaves_alias(a, b);
    if (!a_exists || !b_exists) return false;
    const bool equivalent = std::filesystem::equivalent(a, b, ec);
    if (ec) throw std::runtime_error("cannot compare output paths: " + ec.message());
    return equivalent;
}

static void sync_parent_directory(const std::string& path) {
    const std::string parent = normalized_path(path).parent_path().string();
    const int fd = ::open(parent.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0)
        throw std::runtime_error("cannot open output directory for sync: " +
                                 std::string(std::strerror(errno)));
    const int sync_result = ::fsync(fd);
    const int sync_error = errno;
    const int close_result = ::close(fd);
    if (sync_result != 0)
        throw std::runtime_error("cannot sync output directory: " +
                                 std::string(std::strerror(sync_error)));
    if (close_result != 0)
        throw std::runtime_error("cannot close output directory after sync");
}

// ===== --stream: self-contained, fixed-endian KCSSTRM2 in bounded RAM =====
// The final pathname is untouched until the entire sweep and its requested exact
// validation have succeeded.  A sibling O_EXCL temporary carries a placeholder
// header, the seed, and descending radius groups; finish() patches checksums and
// counts, fsyncs, and atomically renames it.  Peak working RAM is a dirty bitmap,
// at most one edge id per changed edge at the current radius, and a fixed 64 KiB
// encoder buffer.  No O(B) staircase is accumulated by this path.
class StreamSink {
public:
    StreamSink() = default;
    StreamSink(const StreamSink&) = delete;
    StreamSink& operator=(const StreamSink&) = delete;
    ~StreamSink() {
        if (file_) std::fclose(file_);
        if (!committed_ && !temp_path_.empty()) ::unlink(temp_path_.c_str());
    }

    void open(const std::string& path, const std::string& input_path,
              const Graph& graph, const std::vector<uint32_t>& seed,
              Time dmax, Time floor) {
        if (file_ || committed_ || !temp_path_.empty())
            throw std::logic_error("stream sink may be opened only once");
        if (graph.edges.size() != seed.size() || seed.empty())
            throw std::logic_error("stream seed does not match the graph");
        if (paths_alias(input_path, path))
            throw std::runtime_error("refusing to overwrite graph input through --stream");

        final_path_ = normalized_path(path).string();
        for (unsigned attempt = 0; attempt < 1000; ++attempt) {
            temp_path_ = final_path_ + ".tmp." +
                         std::to_string(static_cast<long long>(::getpid())) + "." +
                         std::to_string(attempt);
            const int fd = ::open(temp_path_.c_str(),
                                  O_RDWR | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
            if (fd >= 0) {
                file_ = ::fdopen(fd, "wb+");
                if (file_) break;
                const int saved_error = errno;
                ::close(fd);
                ::unlink(temp_path_.c_str());
                temp_path_.clear();
                throw std::runtime_error("cannot attach KCSSTRM2 temporary: " +
                                         std::string(std::strerror(saved_error)));
            }
            if (errno != EEXIST)
                throw std::runtime_error("cannot create KCSSTRM2 temporary: " +
                                         std::string(std::strerror(errno)));
        }
        if (!file_) throw std::runtime_error("cannot allocate a unique KCSSTRM2 temporary");

        header_.edges = static_cast<uint64_t>(seed.size());
        header_.floor = floor;
        header_.dmax = dmax;
        header_.seed_offset = kcsstream::kStreamHeaderBytes;
        header_.seed_bytes = kcsidx::checked_mul(
            header_.edges, kcsstream::kStreamSeedEntryBytes, "stream seed bytes");
        header_.body_offset = kcsidx::checked_add(
            header_.seed_offset, header_.seed_bytes, "stream body offset");

        kcsidx::Sha256 graph_hash;
        for (size_t edge = 0; edge < graph.edges.size(); ++edge) {
            const Edge& record = graph.edges[edge];
            kcsidx::hash_graph_edge(graph_hash, static_cast<uint64_t>(edge),
                                    static_cast<int64_t>(record.u),
                                    static_cast<int64_t>(record.v), record.t);
        }
        header_.graph_sha256 = graph_hash.finish();

        std::array<uint8_t, kcsstream::kStreamHeaderBytes> placeholder{};
        kcsidx::checked_write(file_, placeholder.data(), placeholder.size(),
                              "KCSSTRM2 placeholder header");
        std::array<uint8_t, kEncodeBufferBytes> buffer{};
        size_t used = 0;
        for (uint32_t value : seed) {
            if (value > header_.edges)
                throw std::logic_error("stream seed exceeds the edge-count bound");
            kcsidx::put_u32(buffer.data(), used, value);
            used += static_cast<size_t>(kcsstream::kStreamSeedEntryBytes);
            if (used == buffer.size()) {
                kcsidx::checked_write(file_, buffer.data(), used, "KCSSTRM2 seed", &seed_crc_);
                used = 0;
            }
        }
        if (used != 0)
            kcsidx::checked_write(file_, buffer.data(), used, "KCSSTRM2 seed", &seed_crc_);

        const size_t bitmap_words = (seed.size() + 63u) / 64u;
        alloc_log("stream.dirty_bitmap", bitmap_words, sizeof(uint64_t));
        dirty_.assign(bitmap_words, 0);
    }

    inline void on_drop(EdgeId edge) {
        const size_t index = static_cast<size_t>(edge);
        if (index >= header_.edges) throw std::logic_error("stream edge id out of range");
        const size_t word = index >> 6;
        const uint64_t bit = uint64_t{1} << (index & 63u);
        if ((dirty_[word] & bit) == 0) {
            dirty_[word] |= bit;
            spool_.push_back(edge);
        }
    }

    // The radius fixed point has been reached: emit one final post-cascade value
    // per changed edge, regardless of how many internal drops it took this radius.
    void flush_radius(Time radius, const std::vector<uint32_t>& coreness) {
        if (spool_.empty()) return;
        if (!file_) throw std::logic_error("stream sink is not open");
        if (radius >= last_radius_ || radius < header_.floor || radius >= header_.dmax)
            throw std::logic_error("KCSSTRM2 radius group is outside the descending domain");
        if (coreness.size() != header_.edges || spool_.size() > header_.edges)
            throw std::logic_error("KCSSTRM2 group does not match edge state");

        const uint64_t count = static_cast<uint64_t>(spool_.size());
        const auto group = kcsstream::encode_stream_group({radius, count});
        kcsidx::checked_write(file_, group.data(), group.size(), "KCSSTRM2 group", &body_crc_);
        body_bytes_ = kcsidx::checked_add(body_bytes_, group.size(), "stream body bytes");

        std::array<uint8_t, kEncodeBufferBytes> buffer{};
        size_t used = 0;
        for (EdgeId edge : spool_) {
            const size_t index = static_cast<size_t>(edge);
            kcsidx::put_u32(buffer.data(), used, edge);
            kcsidx::put_u32(buffer.data(), used + 4, coreness[index]);
            used += static_cast<size_t>(kcsstream::kStreamEventBytes);
            dirty_[index >> 6] &= ~(uint64_t{1} << (index & 63u));
            if (used == buffer.size()) {
                kcsidx::checked_write(file_, buffer.data(), used, "KCSSTRM2 events", &body_crc_);
                body_bytes_ = kcsidx::checked_add(body_bytes_, used, "stream body bytes");
                used = 0;
            }
        }
        if (used != 0) {
            kcsidx::checked_write(file_, buffer.data(), used, "KCSSTRM2 events", &body_crc_);
            body_bytes_ = kcsidx::checked_add(body_bytes_, used, "stream body bytes");
        }

        header_.groups = kcsidx::checked_add(header_.groups, 1, "stream group count");
        header_.records = kcsidx::checked_add(header_.records, count, "stream record count");
        last_radius_ = radius;
        spool_peak_ = std::max(spool_peak_, spool_.size());
        spool_.clear();
    }

    void finish() {
        if (!file_) throw std::logic_error("stream sink is not open or is already finished");
        if (!spool_.empty()) throw std::logic_error("unflushed KCSSTRM2 radius group");
        header_.body_bytes = body_bytes_;
        header_.footer_offset = kcsidx::checked_add(
            header_.body_offset, header_.body_bytes, "stream footer offset");
        header_.file_bytes = kcsidx::checked_add(
            header_.footer_offset, kcsstream::kStreamFooterBytes, "stream file bytes");
        header_.seed_crc = seed_crc_.value();
        header_.body_crc = body_crc_.value();
        const auto encoded_header = kcsstream::encode_stream_header(header_);
        header_.header_crc = kcsidx::get_u64(encoded_header.data() + 168);
        const auto footer = kcsstream::encode_stream_footer(header_);

        kcsidx::checked_seek(file_, 0, "KCSSTRM2 header");
        kcsidx::checked_write(file_, encoded_header.data(), encoded_header.size(),
                              "KCSSTRM2 header");
        kcsidx::checked_seek(file_, header_.footer_offset, "KCSSTRM2 footer");
        kcsidx::checked_write(file_, footer.data(), footer.size(), "KCSSTRM2 footer");
        kcsidx::checked_close(file_, "KCSSTRM2 output");
#if defined(KCS_TEST_STREAM_FAIL_BEFORE_RENAME)
        throw std::runtime_error("injected KCSSTRM2 failure before rename");
#endif
        if (::rename(temp_path_.c_str(), final_path_.c_str()) != 0)
            throw std::runtime_error("atomic KCSSTRM2 rename failed: " +
                                     std::string(std::strerror(errno)));
        committed_ = true;
        sync_parent_directory(final_path_);
    }

    uint64_t groups() const { return header_.groups; }
    uint64_t records() const { return header_.records; }
    uint64_t file_bytes() const { return header_.file_bytes; }
    size_t spool_peak() const { return spool_peak_; }
    size_t ram_bytes() const {
        return dirty_.capacity() * sizeof(uint64_t) +
               spool_.capacity() * sizeof(EdgeId) + kEncodeBufferBytes;
    }

private:
    static constexpr size_t kEncodeBufferBytes = size_t{1} << 16;
    std::string final_path_;
    std::string temp_path_;
    std::FILE* file_ = nullptr;
    std::vector<uint64_t> dirty_;
    std::vector<EdgeId> spool_;
    kcsstream::StreamHeader header_{};
    kcsidx::Crc64 seed_crc_;
    kcsidx::Crc64 body_crc_;
    uint64_t body_bytes_ = 0;
    Time last_radius_ = std::numeric_limits<Time>::max();
    size_t spool_peak_ = 0;
    bool committed_ = false;
};

// ===== Stage 0 scheduler: histogram sweep (Thm 6.2 engine, exact semantics) =====
// Drives the chaotic iteration of Thm 5.5 downward from the c_inf seed.  Departure
// events are processed one by one — Theta(E) = Theta(Sigma deg^2) event work; this is
// the correctness anchor for the certificate scheduler.
struct HistStats {
    long long removals = 0, reevals = 0, real_reevals = 0, false_reevals = 0;
    long long breakpoints = 0, member_updates = 0, ev_count = 0;
};

HistStats hist_sweep(Graph& graph, std::vector<uint32_t>& c, Time Dmax, Time floorv,
                     bool dodump, DropLog& drops, StreamSink* sink = nullptr,
                     bool filter_work = true) {
    size_t m = graph.edges.size();
    HistStats st;
    uint32_t maxc = 0;
    for (size_t e = 0; e < m; ++e) maxc = std::max(maxc, c[e]);

    size_t S = 2 * m;
    std::vector<NodeId> snode(S);
    std::vector<int> wlo(S), whi(S), cap(S);
    for (size_t e = 0; e < m; ++e) {
        const Edge& ed = graph.edges[e];
        for (size_t sd = 0; sd < 2; ++sd) {
            NodeId w = sd == 0 ? ed.u : ed.v;
            size_t sid = 2 * e + sd;
            snode[sid] = w;
            wlo[sid] = graph.csr.lower_time(w, lower_window(ed.t, Dmax));
            whi[sid] = graph.csr.upper_time(w, upper_window(ed.t, Dmax));
        }
    }
    // per-side cap = H_init via scratch histogram
    {
        std::vector<int> scratch((size_t)maxc + 2, 0);
        std::vector<int> touched; touched.reserve(256);
        for (size_t sid = 0; sid < S; ++sid) {
            NodeId w = snode[sid]; size_t begin = graph.csr.node_begin(w);
            int top = std::min(whi[sid] - wlo[sid], (int)maxc);
            for (int i = wlo[sid]; i < whi[sid]; ++i) {
                uint32_t cv = c[(size_t)graph.csr.incidences[begin + (size_t)i].edge];
                int v = (int)std::min<uint32_t>(cv, (uint32_t)top);
                if (scratch[nonnegative_index(v)]++ == 0) touched.push_back(v);
            }
            int acc = 0, h = 0;
            for (int v = top; v >= 1; --v) {
                acc += scratch[nonnegative_index(v)];
                if (acc >= v) { h = v; break; }
            }
            cap[sid] = h;
            for (int v : touched) scratch[nonnegative_index(v)] = 0;
            touched.clear();
        }
    }
    std::vector<size_t> histoff(S + 1, 0);
    for (size_t sid = 0; sid < S; ++sid) histoff[sid + 1] = histoff[sid] + (size_t)cap[sid] + 1;
    std::vector<int> hist(histoff[S], 0);
    std::vector<uint32_t> Hval(S, 0);
    std::vector<int> Sgeq(S, 0);

    auto capval = [&](size_t sid, uint32_t v) -> int { return (int)std::min<uint32_t>(v, (uint32_t)cap[sid]); };

    for (size_t sid = 0; sid < S; ++sid) {
        NodeId w = snode[sid]; size_t begin = graph.csr.node_begin(w);
        for (int i = wlo[sid]; i < whi[sid]; ++i) {
            uint32_t cv = c[(size_t)graph.csr.incidences[begin + (size_t)i].edge];
            ++hist[histoff[sid] + (size_t)capval(sid, cv)];
        }
        Hval[sid] = (uint32_t)cap[sid];
        Sgeq[sid] = hist[histoff[sid] + (size_t)cap[sid]];
    }
    std::vector<int>().swap(wlo);
    std::vector<int>().swap(whi);
    std::vector<NodeId>().swap(snode);
    { size_t bad = 0; for (size_t e = 0; e < m; ++e) if (c[e] != std::min(Hval[2 * e], Hval[2 * e + 1])) ++bad;
      if (bad) throw std::logic_error("histogram initialization disagrees with the seed on " +
                                      std::to_string(bad) + " edges"); }

    std::vector<uint8_t> inq(m, 0);
    std::vector<EdgeId> work;

    auto rebalance = [&](size_t sid) -> bool {
        uint32_t oldH = Hval[sid];
        while (Hval[sid] > 0 && Sgeq[sid] < (int)Hval[sid]) {
            Hval[sid]--; Sgeq[sid] += hist[histoff[sid] + (size_t)Hval[sid]];
        }
        return Hval[sid] != oldH;
    };
    auto dec_value = [&](size_t sid, int ov, int nv) -> bool {
        hist[histoff[sid] + (size_t)ov]--; hist[histoff[sid] + (size_t)nv]++;
        if (ov >= (int)Hval[sid] && nv < (int)Hval[sid]) { Sgeq[sid]--; return rebalance(sid); }
        return false;
    };
    auto rem_value = [&](size_t sid, int v) -> bool {
        hist[histoff[sid] + (size_t)v]--;
        if (v >= (int)Hval[sid]) { Sgeq[sid]--; return rebalance(sid); }
        return false;
    };

    auto push = [&](EdgeId e) { if (!inq[(size_t)e]) { inq[(size_t)e] = 1; work.push_back(e); } };
    auto edge_due = [&](EdgeId e) {
        size_t ei = static_cast<size_t>(e);
        return std::min(Hval[2 * ei], Hval[2 * ei + 1]) < c[ei];
    };

    auto reeval = [&](EdgeId e, Time delta) {
        uint32_t nv = std::min(Hval[2 * (size_t)e], Hval[2 * (size_t)e + 1]);
        if (nv >= c[(size_t)e]) { ++st.false_reevals; return; }
        ++st.real_reevals;
        uint32_t old = c[(size_t)e]; c[(size_t)e] = nv; ++st.breakpoints;
        if (dodump) drops.emplace_back(e, delta + 1, old);
        if (sink) sink->on_drop(e);
        const Edge& ed = graph.edges[(size_t)e];
        for (int sd = 0; sd < 2; ++sd) {
            NodeId w = sd == 0 ? ed.u : ed.v;
            int lo = graph.csr.lower_time(w, lower_window(ed.t, delta));
            int hi = graph.csr.upper_time(w, upper_window(ed.t, delta));
            size_t begin = graph.csr.node_begin(w);
            for (int i = lo; i < hi; ++i) {
                const Incidence& in = graph.csr.incidences[begin + (size_t)i];
                size_t sf = 2 * (size_t)in.edge + in.side;
                int ov = capval(sf, old), nvv = capval(sf, nv);
                if (ov != nvv) {
                    ++st.member_updates;
                    dec_value(sf, ov, nvv);
                }
                if (!filter_work || edge_due(in.edge)) push(in.edge);
            }
        }
    };

    RadixHeapMax rh(Dmax);
    for (NodeId node = 0; node < graph.node_count; ++node) {
        size_t b = graph.csr.node_begin(node), e = graph.csr.node_end(node);
        size_t j = b;
        for (size_t i = b; i < e; ++i) {
            if (j <= i) j = i + 1;
            Time ti = graph.csr.incidences[i].t;
            while (j < e && graph.csr.incidences[j].t - ti <= Dmax) ++j;
            size_t jmax = j - 1;
            if (jmax > i) {
                Time g = graph.csr.incidences[jmax].t - ti;
                if (g > floorv) rh.push(g, (uint32_t)i, (uint32_t)jmax);
            }
        }
    }

    std::vector<RadixHeapMax::Entry> batch;
    while (!rh.empty()) {
        Time g = rh.pop_batch(batch); Time delta = g - 1;
        std::sort(batch.begin(), batch.end(),
                  [](const RadixHeapMax::Entry& a, const RadixHeapMax::Entry& b) { return a.pi > b.pi; });
        for (const RadixHeapMax::Entry& en : batch) {
            uint32_t pi = en.pi, pj = en.pj;
            for (;;) {
                const Incidence& Ii = graph.csr.incidences[pi];
                const Incidence& Ij = graph.csr.incidences[pj];
                uint32_t sa = 2 * (uint32_t)Ii.edge + Ii.side, sb = 2 * (uint32_t)Ij.edge + Ij.side;
                EdgeId ea = Ii.edge, eb = Ij.edge;
                ++st.removals;
                rem_value(sa, capval(sa, c[(size_t)eb]));
                if (!filter_work || edge_due(ea)) push(ea);
                ++st.removals;
                rem_value(sb, capval(sb, c[(size_t)ea]));
                if (!filter_work || edge_due(eb)) push(eb);
                ++st.ev_count;
                if (pj > pi + 1) {
                    Time ng = graph.csr.incidences[pj - 1].t - Ii.t;
                    if (ng > floorv) {
                        if (ng == g) { --pj; continue; }
                        rh.push(ng, pi, pj - 1);
                    }
                }
                break;
            }
        }
        while (!work.empty()) { EdgeId e = work.back(); work.pop_back(); inq[(size_t)e] = 0; ++st.reevals; reeval(e, delta); }
        if (sink) sink->flush_radius(delta, c);   // radius-delta fixed point: emit
    }
    if (sink) {
        std::cout << "MEMBK hist: hist=" << (hist.capacity() * 4) / (1 << 20)
                  << "MB histoff=" << (histoff.capacity() * 8) / (1 << 20)
                  << "MB side(Hval,Sgeq,cap)=" << ((Hval.capacity() + (size_t)Sgeq.capacity() + cap.capacity()) * 4) / (1 << 20)
                  << "MB\n";
    }
    return st;
}

// ===================================================================================
// ===== Stage 1: the kinetic certificate scheduler (KCS proper) =====================
// ===================================================================================
//
// Per-side certificate at level h = H[sid] with slack r = count(v>=h in window) - h:
//   - departure trigger: order-statistic timer at the (a+1)-st largest RELEVANT gap
//     (a = ceil(r/2)), computed from O_{x,h} by binary search on the radius;
//   - drop trigger: budget b = floor(r/2) in the budget forest T_{x,h}; each value
//     drop of a member crossing level h range-decrements budgets over the affected
//     contiguous position range; underflow (< 0) wakes the side.
// Never-late: a violation consumes r+1 units, so >= a+1 departures (timer fires at
// or before) or >= b+1 drops (budget underflows at or before).  Every wake re-verifies
// LIVE against O_{x,h} at the current radius; false alarms re-arm with fresh slack.
// Correctness: quiescent state at each Delta is the exact greatest fixpoint (Thm 5.3/
// 5.5, Cor 5.6); breakpoints recorded at Delta_up = g when processing batch gap g at
// radius delta = g-1, identical to the histogram engine's discipline.

// ===== Hybrid-only multi-record radix max-queue over integer radii =================
// The default non-hybrid sweep uses IndexedTimerMax below, matching the paper's
// O(m)-space indexed dictionary.  Hybrid eager sides can own many exact departure
// records at once, so they use this fixed 65-bucket multi-record radix queue.  It has
// no Dmax-sized directory; keys are monotone because an arm at radius delta pushes a
// gap <= delta < the current batch g.
struct BucketQueueMax {
    struct Entry { Time g; uint32_t pi; uint32_t pj; uint8_t eager; };
    std::vector<Entry> buckets[65];
    Time last = 0;                          // upper bound on every stored key
    size_t count = 0;
    size_t peak = 0;                        // max entries ever resident (incl. stale)
    size_t batch_peak = 0;
    size_t bucket_capacity_entries = 0;
    size_t batch_capacity_current = 0;
    size_t capacity_peak_entries = 0;       // bucket + live local-batch capacities

    void init(Time upper) {                 // keys in [0, upper]
        if (upper < 0) throw std::invalid_argument("BucketQueueMax upper bound must be non-negative");
        last = upper;
        count = 0;
        peak = 0;
        batch_peak = 0;
        batch_capacity_current = 0;
        bucket_capacity_entries = 0;
        for (auto& b : buckets) {
            b.clear();
            bucket_capacity_entries += b.capacity();
        }
        capacity_peak_entries = bucket_capacity_entries;
    }

    int bucket_of(Time g) const {
        uint64_t diff = static_cast<uint64_t>(g) ^ static_cast<uint64_t>(last);
        return diff == 0 ? 0 : 64 - __builtin_clzll(diff);
    }

    void bucket_push(int b, const Entry& e, size_t transient_entries = 0) {
        size_t old_capacity = buckets[b].capacity();
        buckets[b].push_back(e);
        bucket_capacity_entries += buckets[b].capacity() - old_capacity;
        capacity_peak_entries = std::max(capacity_peak_entries,
            bucket_capacity_entries + batch_capacity_current + transient_entries);
    }

    void push(Time g, uint32_t pi, uint32_t pj) {
#ifndef NDEBUG
        if (g > last) throw std::logic_error("Hybrid timer key exceeds the monotone frontier");
#endif
        bucket_push(bucket_of(g), Entry{g, pi, pj, 0});
        ++count;
        if (count > peak) peak = count;
    }

    void push_eager(Time g, uint32_t sid, uint32_t member_pos) {
#ifndef NDEBUG
        if (g > last) throw std::logic_error("Hybrid eager key exceeds the monotone frontier");
#endif
        bucket_push(bucket_of(g), Entry{g, sid, member_pos, 1});
        ++count;
        if (count > peak) peak = count;
    }

    bool empty() const { return count == 0; }

    Time pop_batch(std::vector<Entry>& out) {
        if (buckets[0].empty()) {
            int i = 1;
            while (i < 65 && buckets[i].empty()) ++i;
            if (i == 65) throw std::logic_error("Hybrid radix queue count is inconsistent");
            Time mx = buckets[i].front().g;
            for (const Entry& e : buckets[i]) mx = std::max(mx, e.g);
            last = mx;
            std::vector<Entry> moved;
            size_t source_capacity = buckets[i].capacity();
            moved.swap(buckets[i]);
            bucket_capacity_entries -= source_capacity;
            capacity_peak_entries = std::max(capacity_peak_entries,
                bucket_capacity_entries + batch_capacity_current + moved.capacity());
            for (const Entry& e : moved)
                bucket_push(bucket_of(e.g), e, moved.capacity());
        }

        out.clear();
        size_t old_out_capacity = out.capacity();
        size_t zero_capacity = buckets[0].capacity();
        out.swap(buckets[0]);
        bucket_capacity_entries = bucket_capacity_entries - zero_capacity + old_out_capacity;
        batch_capacity_current = out.capacity();
        batch_peak = std::max(batch_peak, out.size());
        capacity_peak_entries = std::max(capacity_peak_entries,
            bucket_capacity_entries + batch_capacity_current);
        count -= out.size();
        return last;
    }
};

// Indexed monotone max-dictionary for the non-hybrid certificate sweep.
// A side has at most one logical departure timer.  LIVE means that timer is in
// the radix dictionary; POPPED means its old item is in the one synchronous gap batch
// being processed.  Re-arming a POPPED side installs a new LIVE timer and thereby
// invalidates the old batch item without a generation counter.
struct IndexedTimerMax {
    enum : uint8_t { NONE = 0, LIVE = 1, POPPED = 2 };
    static constexpr uint32_t NPOS = std::numeric_limits<uint32_t>::max();

    // Intrusive monotone radix dictionary: one list node per side and 65 heads.
    // In the 64-bit word-RAM model, update/delete are O(1) and redistribution is
    // O(W) amortized per logical installation (W=64; generically O(log U)).
    Time frontier = 0;
    Time batch_key = 0;
    size_t count = 0;
    size_t peak = 0;
    size_t batch_peak = 0;
    size_t batch_capacity_peak = 0;
    uint64_t occupied = 0;             // bit i-1 denotes nonempty bucket i, 1..64
    uint32_t head[65] = {};
    std::vector<uint32_t> next, prev;
    std::vector<Time> key;
    std::vector<uint8_t> bucket, state;

    void init(size_t sides, Time upper) {
        if (sides > static_cast<size_t>(NPOS))
            throw std::overflow_error("Indexed timer side count exceeds uint32 positions");
        if (upper < 0)
            throw std::invalid_argument("Indexed timer frontier must be non-negative");
        frontier = upper;
        batch_key = 0;
        count = 0;
        occupied = 0;
        std::fill(head, head + 65, NPOS);
        alloc_log("iq.links", sides, 2 * sizeof(uint32_t));
        alloc_log("iq.key", sides, sizeof(Time));
        alloc_log("iq.tags", sides, 2 * sizeof(uint8_t));
        next.assign(sides, NPOS);
        prev.assign(sides, NPOS);
        key.assign(sides, 0);
        bucket.assign(sides, 0);
        state.assign(sides, NONE);
        peak = 0;
        batch_peak = 0;
        batch_capacity_peak = 0;
    }

    bool empty() const { return count == 0; }

    uint8_t bucket_for(Time g) const {
        uint64_t diff = static_cast<uint64_t>(frontier) ^ static_cast<uint64_t>(g);
        return diff == 0 ? 0 : static_cast<uint8_t>(64 - __builtin_clzll(diff));
    }

    void link(uint32_t sid, uint8_t b) {
        uint32_t old_head = head[b];
        prev[sid] = NPOS;
        next[sid] = old_head;
        if (old_head != NPOS) prev[old_head] = sid;
        head[b] = sid;
        bucket[sid] = b;
        if (b != 0) occupied |= uint64_t(1) << (b - 1);
    }

    void unlink(uint32_t sid) {
        uint8_t b = bucket[sid];
        uint32_t p = prev[sid], n = next[sid];
        if (p == NPOS) head[b] = n;
        else next[p] = n;
        if (n != NPOS) prev[n] = p;
        prev[sid] = next[sid] = NPOS;
        if (b != 0 && head[b] == NPOS)
            occupied &= ~(uint64_t(1) << (b - 1));
    }

    void schedule(uint32_t sid, Time g) {
        if (g < 1) {
            cancel(sid);
            return;
        }
#ifndef NDEBUG
        if (g > frontier)
            throw std::logic_error("Indexed timer key exceeds the monotone frontier");
        if (state[sid] == POPPED && g >= batch_key)
            throw std::logic_error("POPPED timer re-armed outside the monotone gap frontier");
#endif
        if (state[sid] == LIVE) {
            unlink(sid);
        } else {
            ++count;
            peak = std::max(peak, count);
        }
        key[sid] = g;
        state[sid] = LIVE;
        link(sid, bucket_for(g));
    }

    void cancel(uint32_t sid) {
        if (state[sid] == LIVE) {
            unlink(sid);
            --count;
        }
        state[sid] = NONE;
        key[sid] = 0;
        prev[sid] = next[sid] = NPOS;
    }

    void refill_zero() {
        if (head[0] != NPOS || occupied == 0) return;
        uint8_t b = static_cast<uint8_t>(1 + __builtin_ctzll(occupied));
        Time new_frontier = 0;
        for (uint32_t sid = head[b]; sid != NPOS; sid = next[sid])
            new_frontier = std::max(new_frontier, key[sid]);
        frontier = new_frontier;

        uint32_t sid = head[b];
        head[b] = NPOS;
        occupied &= ~(uint64_t(1) << (b - 1));
        while (sid != NPOS) {
            uint32_t following = next[sid];
            prev[sid] = next[sid] = NPOS;
            link(sid, bucket_for(key[sid]));
            sid = following;
        }
    }

    Time pop_batch(std::vector<uint32_t>& out) {
        refill_zero();
        Time g = frontier;
        batch_key = g;
        out.clear();
        uint32_t sid = head[0];
        head[0] = NPOS;
        while (sid != NPOS) {
            uint32_t following = next[sid];
            prev[sid] = next[sid] = NPOS;
            state[sid] = POPPED;
            out.push_back(sid);
            --count;
            sid = following;
        }
        batch_peak = std::max(batch_peak, out.size());
        batch_capacity_peak = std::max(batch_capacity_peak, out.capacity());
        return g;
    }

    bool consume(uint32_t sid) {
        if (state[sid] != POPPED) return false;
        state[sid] = NONE;
        key[sid] = 0;
        prev[sid] = next[sid] = NPOS;
        return true;
    }
};

struct KcsStats {
    long long arms = 0;              // certificate (re-)arms
    long long recomputes = 0;        // exact verify_side calls (the common wake metric)
    long long timer_wakes = 0;       // valid timer pops
    long long stale_pops = 0;        // hybrid: stale epoch record; indexed: same-gap
                                     // batch item invalidated by an earlier cascade
    long long budget_wakes = 0;      // budget underflow wakes
    long long false_alarms = 0;      // wakes that re-verified with no H drop
    long long real_alarms = 0;       // wakes where H dropped
    long long h_drop_units = 0;      // total unit descents of side h-indices
    long long breakpoints = 0;       // recorded value drops (incl. multi-hop)
    long long occ_counts = 0;        // order-statistic count queries
    long long occ_deletes = 0;       // O_{x,h} deletions
    long long range_adds = 0;        // budget-forest range decrements
    long long timer_probes = 0;      // binary-search probes inside timer arming
    long long cheap_rearms = 0;      // budget-false-alarm re-arms that kept the timer
    long long relax_drop_checks = 0; // --relax: O(1) candidate-side tests on value drops
    long long relax_drop_hits = 0;   // --relax: relevant support crossings observed
    long long work() const {         // stable logical-touch counter across builds
        return arms + timer_wakes + stale_pops + budget_wakes + h_drop_units +
               occ_counts + occ_deletes + range_adds + timer_probes + cheap_rearms +
               relax_drop_checks;
    }
};

struct Kcs {
    Graph& graph;
    std::vector<uint32_t>& c;
    Time Dmax, floorv;
    bool dodump;
    bool relaxed;
    DropLog& drops;
    StreamSink* sink = nullptr;
    KcsStats st;

    size_t m, n;
    uint32_t maxc = 0;
    uint32_t eager_limit = 0;
    bool hybrid_active = false;

    // ---- occurrence structures O_{x,h}: per node x, levels 1..KO[x];
    // block = initial positions with c >= h (sorted), Fenwick of 0/1 over the block.
    std::vector<int> KO;                 // per node: max level
    std::vector<size_t> lvl_off;         // per node: offset into blk_off ("level directory")
    std::vector<size_t> blk_off;         // per (x,h): offset into occ arrays
    std::vector<uint32_t> occ_pos;       // concatenated sorted positions (global CSR pos)
    std::vector<int32_t> occ_bit;        // Fenwick per block

    // ---- budget forest AVL trees T_{x,h}: one node per side, keyed by position.
    std::vector<TreeNode> troot;         // per (x,h): AVL root (side id) or -1
    struct alignas(32) TN { TreeNode l = -1, r = -1; int32_t bud = 0, mn = 0, lz = 0;
                            uint32_t key = 0; uint8_t h = 1; uint8_t pad[7] = {}; };
    std::vector<TN> tn;                  // packed AVL node (AoS experiment)
    std::vector<uint8_t> in_tree;
    std::vector<uint32_t> Hs;            // per side: current h-index
    std::vector<uint32_t> epoch;         // per side: certificate epoch
    const uint32_t* side_pos = nullptr;   // non-owning view of Graph::incidence_pos
    std::vector<int32_t> sa;             // KCS_PROPSPLIT only: armed departure budget a
    std::vector<int32_t> armed_b;        // per side: armed drop budget b, for FA spend accounting
    std::vector<uint8_t> ssplit;         // per side: adaptive eighths (1..7; 0..8 with KCS_CORNER)
    std::vector<int32_t> timer_split;    // per side: last exact two-run partition index
    std::vector<uint8_t> in_wake;
    std::vector<uint8_t> tfired;         // per side: queued wake must be treated as timer-fired
    std::vector<int32_t> rbud;           // --relax: direct per-side drop-loss budget
    std::vector<uint32_t> wakeq;
    std::vector<size_t> pend_lvls;       // (x,h) directory slots with pending underflow
    uint32_t active_sid = std::numeric_limits<uint32_t>::max();

    // TreeNode is signed solely to encode the -1 null sentinel.  Every array
    // access passes through this checked boundary, so a sentinel can never be
    // silently converted to a huge size_t index.  All accessors inline away in
    // NDEBUG builds and preserve the original structure-of-arrays layout.
    static inline size_t tree_index(TreeNode node) noexcept {
        assert(node >= 0);
        return static_cast<size_t>(node);
    }
    inline TreeNode& t_left(TreeNode node) noexcept { return tn[tree_index(node)].l; }
    inline const TreeNode& t_left(TreeNode node) const noexcept { return tn[tree_index(node)].l; }
    inline TreeNode& t_right(TreeNode node) noexcept { return tn[tree_index(node)].r; }
    inline const TreeNode& t_right(TreeNode node) const noexcept { return tn[tree_index(node)].r; }
    inline int32_t& t_budget(TreeNode node) noexcept { return tn[tree_index(node)].bud; }
    inline const int32_t& t_budget(TreeNode node) const noexcept { return tn[tree_index(node)].bud; }
    inline int32_t& t_subtree_min(TreeNode node) noexcept { return tn[tree_index(node)].mn; }
    inline const int32_t& t_subtree_min(TreeNode node) const noexcept { return tn[tree_index(node)].mn; }
    inline int32_t& t_lazy(TreeNode node) noexcept { return tn[tree_index(node)].lz; }
    inline const int32_t& t_lazy(TreeNode node) const noexcept { return tn[tree_index(node)].lz; }
    inline uint8_t& t_stored_height(TreeNode node) noexcept { return tn[tree_index(node)].h; }
    inline const uint8_t& t_stored_height(TreeNode node) const noexcept {
        return tn[tree_index(node)].h;
    }

    // ---- exact eager-side state (allocated only for KCS_EAGER_XI > 0) ----
    std::vector<uint8_t> eager;
    std::vector<int> eager_cap, eager_Sgeq;
    std::vector<size_t> eager_histoff;
    std::vector<int> eager_hist;
    std::vector<uint32_t> eager_H;
    std::vector<uint8_t> eager_inq;
    std::vector<EdgeId> eager_work;

    // ---- KCS_CENSUS state (allocated only when the env var is set) ----
    static constexpr int CENSUS_B = 32;  // xi < 2^31 always (uint32 CSR positions)
    long long cs_cnt[CENSUS_B] = {};     // verify_side entries per bucket
    long long cs_false[CENSUS_B] = {};   // no-h-drop verifies per bucket
    unsigned long long cs_cyc[CENSUS_B] = {};  // cycles: verify entry -> exit (incl. re-arm)
    std::vector<uint8_t> cs_bucket;      // per side: floor(log2(xi_at_Dmax))
    std::vector<uint32_t> cs_last_batch; // per side: last Delta batch id with a verify
    uint32_t cs_batch = 0;               // current Delta batch id (== one pop_batch radius)
    long long cs_repeats = 0;            // verifies of a side already verified this batch

    // ---- KCS_FADIAG state (strict sweep only; allocated only when enabled) ----
    struct Fadiag {
        long long trigger_timer = 0;
        long long trigger_budget = 0;
        long long timer_unspent_zero = 0;
        long long timer_unspent_log2[32] = {};
        long long timer_unspent_underflow = 0;
        long long timer_no_drops = 0;
        std::map<int, long long> fresh_slack;
        long long ssplit[9] = {};
    };
    std::unique_ptr<Fadiag> fd;

    Kcs(Graph& g_, std::vector<uint32_t>& c_, Time D, Time f, bool dd,
        DropLog& dr, bool relax = false)
        : graph(g_), c(c_), Dmax(D), floorv(f), dodump(dd), relaxed(relax), drops(dr) {
        m = graph.edges.size();
        n = static_cast<size_t>(graph.node_count);
        side_pos = graph.incidence_pos.data();
        eager_limit = relaxed ? 0 : eager_xi_limit();
        for (size_t e = 0; e < m; ++e) maxc = std::max(maxc, c[e]);
    }

    // ---- P5: optional phase breakdown timers (additive instrumentation) ----
    // The paper/profile build attributes wall time to exactly one phase at a time.
    // In the default release build ph_enter/ph_exit are compile-time no-ops: the hot
    // path performs no clock reads, while stable work counters remain available to
    // existing artifact parsers.
    enum : uint8_t { PH_DEP = 0, PH_DROP = 1, PH_CASC = 2, PH_ARM = 3 };
    double phw[4] = {0, 0, 0, 0};
    double init_s = 0, loop_s = 0;
#if defined(KCS_PROFILE)
    using PClk = std::chrono::steady_clock;
    int ph_cur = -1;
    PClk::time_point ph_mark{};
    inline int ph_enter(int p) {
        PClk::time_point now = PClk::now();
        if (ph_cur >= 0) phw[ph_cur] += std::chrono::duration<double>(now - ph_mark).count();
        int prev = ph_cur; ph_cur = p; ph_mark = now;
        return prev;
    }
    inline void ph_exit(int prev) {
        PClk::time_point now = PClk::now();
        phw[ph_cur] += std::chrono::duration<double>(now - ph_mark).count();
        ph_cur = prev; ph_mark = now;
    }
#else
    inline int ph_enter(int) const noexcept { return -1; }
    inline void ph_exit(int) const noexcept {}
#endif

    // ---------- occurrence structure helpers ----------
    inline size_t dir_slot(NodeId x, int h) const {      // valid for 1 <= h <= KO[x]
        return lvl_off[static_cast<size_t>(x)] + static_cast<size_t>(h - 1);
    }
    inline std::vector<uint32_t>::iterator occ_iterator(size_t offset) noexcept {
        using Difference = std::vector<uint32_t>::difference_type;
        assert(offset <= occ_pos.size());
        assert(offset <= static_cast<size_t>(std::numeric_limits<Difference>::max()));
        return occ_pos.begin() + static_cast<Difference>(offset);
    }
    // Fenwick over block [bs, be): standard 1-indexed within block
    inline void fen_add(size_t bs, size_t be, int idx, int d) {   // idx: 0-based in block
        int len = static_cast<int>(be - bs);
        for (int i = idx + 1; i <= len; i += i & -i) occ_bit[bs + static_cast<size_t>(i - 1)] += d;
    }
    inline int fen_pref(size_t bs, int cnt) const {               // sum of first cnt
        int s = 0;
        for (int i = cnt; i > 0; i -= i & -i) s += occ_bit[bs + static_cast<size_t>(i - 1)];
        return s;
    }
    // The exact live-rank prefix computed by occ_count is also the first part of
    // timer_gap's setup.  A verify-side re-arm can borrow it while the occurrence
    // structure is unchanged, avoiding a duplicate lower_bound + Fenwick prefix.
    struct TimerWindow {
        NodeId x = 0;
        int h = 0;
        Time t = 0, delta = 0;
        uint32_t pl = 0, pr = 0;
        size_t bs = 0;
        int len = 0, rank_before = 0, cnt = 0;
        bool valid = false;

        void bind(NodeId x_, int h_, Time t_, Time delta_, uint32_t pl_, uint32_t pr_, int cnt_) {
            x = x_; h = h_; t = t_; delta = delta_; pl = pl_; pr = pr_; cnt = cnt_; valid = true;
        }
        bool matches(NodeId x_, int h_, Time t_, Time delta_, int cnt_) const {
            return valid && x == x_ && h == h_ && t == t_ && delta == delta_ && cnt == cnt_;
        }
    };

    // count of live positions with value >= h in GLOBAL position range [pl, pr]
    int occ_count(NodeId x, int h, uint32_t pl, uint32_t pr, TimerWindow* timer_window = nullptr) {
        ++st.occ_counts;
        size_t slot = dir_slot(x, h);
        size_t bs = blk_off[slot], be = blk_off[slot + 1];
        if (bs == be || pl > pr) {
            if (timer_window) timer_window->valid = false;
            return 0;
        }
        auto block_begin = occ_iterator(bs);
        auto block_end = occ_iterator(be);
        auto first = std::lower_bound(block_begin, block_end, pl);
        auto last = std::upper_bound(block_begin, block_end, pr);
        int a = static_cast<int>(first - block_begin);
        int b = static_cast<int>(last - block_begin);
        int rank_before = fen_pref(bs, a);
        int cnt = (a >= b) ? 0 : fen_pref(bs, b) - rank_before;
        if (timer_window) {
            timer_window->bs = bs;
            timer_window->len = static_cast<int>(be - bs);
            timer_window->rank_before = rank_before;
            timer_window->cnt = cnt;
            timer_window->pl = pl;
            timer_window->pr = pr;
            timer_window->valid = true;
        }
        return cnt;
    }
    void occ_delete(NodeId x, int h, uint32_t p) {
        ++st.occ_deletes;
        size_t slot = dir_slot(x, h);
        size_t bs = blk_off[slot], be = blk_off[slot + 1];
        auto block_begin = occ_iterator(bs);
        auto it = std::lower_bound(block_begin, occ_iterator(be), p);
        fen_add(bs, be, static_cast<int>(it - block_begin), -1);
    }

    // ---------- array AVL tree (budget forest) ----------
    inline void t_apply(TreeNode node, int32_t delta) {
        t_budget(node) += delta;
        t_subtree_min(node) += delta;
        t_lazy(node) += delta;
    }
    inline void t_push(TreeNode node) {
        int32_t lazy = t_lazy(node);
        if (lazy != 0) {
            TreeNode left = t_left(node);
            TreeNode right = t_right(node);
            if (left >= 0) t_apply(left, lazy);
            if (right >= 0) t_apply(right, lazy);
            t_lazy(node) = 0;
        }
    }
    inline void t_pull(TreeNode node) {
        TreeNode left = t_left(node);
        TreeNode right = t_right(node);
        int32_t subtree_min = t_budget(node);
        if (left >= 0) subtree_min = std::min(subtree_min, t_subtree_min(left));
        if (right >= 0) subtree_min = std::min(subtree_min, t_subtree_min(right));
        t_subtree_min(node) = subtree_min;
        int left_height = left < 0 ? 0 : static_cast<int>(t_stored_height(left));
        int right_height = right < 0 ? 0 : static_cast<int>(t_stored_height(right));
        int height = 1 + std::max(left_height, right_height);
#ifndef NDEBUG
        if (height > static_cast<int>(std::numeric_limits<uint8_t>::max()))
            throw std::logic_error("AVL height exceeds uint8 encoding");
#endif
        t_stored_height(node) = static_cast<uint8_t>(height);
    }
    inline int t_height(TreeNode node) const {
        return node < 0 ? 0 : static_cast<int>(t_stored_height(node));
    }
#ifndef NDEBUG
    void t_debug_local_balanced(TreeNode node) const {
        if (node < 0) return;
        TreeNode left = t_left(node);
        TreeNode right = t_right(node);
        int left_height = t_height(left);
        int right_height = t_height(right);
        if (std::abs(left_height - right_height) > 1 ||
            t_stored_height(node) != static_cast<uint8_t>(1 + std::max(left_height, right_height)))
            throw std::logic_error("AVL balance/height invariant failed");
        if (t_lazy(node) != 0) throw std::logic_error("AVL structural update retained a root lazy tag");
        int32_t subtree_min = t_budget(node);
        if (left >= 0) {
            if (side_pos[tree_index(left)] >= side_pos[tree_index(node)])
                throw std::logic_error("AVL left-child key invariant failed");
            subtree_min = std::min(subtree_min, t_subtree_min(left));
        }
        if (right >= 0) {
            if (side_pos[tree_index(right)] <= side_pos[tree_index(node)])
                throw std::logic_error("AVL right-child key invariant failed");
            subtree_min = std::min(subtree_min, t_subtree_min(right));
        }
        if (subtree_min != t_subtree_min(node))
            throw std::logic_error("AVL subtree-min invariant failed");
    }
#else
    inline void t_debug_local_balanced(TreeNode) const {}
#endif
    TreeNode t_rotate_left(TreeNode root) {
        t_push(root);
        TreeNode pivot = t_right(root);
        t_push(pivot);
        t_right(root) = t_left(pivot);
        t_left(pivot) = root;
        t_pull(root);
        t_pull(pivot);
        return pivot;
    }
    TreeNode t_rotate_right(TreeNode root) {
        t_push(root);
        TreeNode pivot = t_left(root);
        t_push(pivot);
        t_left(root) = t_right(pivot);
        t_right(pivot) = root;
        t_pull(root);
        t_pull(pivot);
        return pivot;
    }
    TreeNode t_balance(TreeNode node) {
        if (node < 0) return node;
        t_pull(node);
        TreeNode left = t_left(node);
        TreeNode right = t_right(node);
        int balance_factor = t_height(left) - t_height(right);
        if (balance_factor > 1) {
            if (t_height(t_left(left)) < t_height(t_right(left)))
                t_left(node) = t_rotate_left(left);
            node = t_rotate_right(node);
        } else if (balance_factor < -1) {
            if (t_height(t_right(right)) < t_height(t_left(right)))
                t_right(node) = t_rotate_right(right);
            node = t_rotate_left(node);
        }
        t_debug_local_balanced(node);
        return node;
    }
    // Join two AVL trees around a detached node.  Every key in left is below the
    // node key and every key in right is above it.
    TreeNode t_join3(TreeNode left, TreeNode node, TreeNode right) {
        int left_height = t_height(left);
        int right_height = t_height(right);
        if (left_height > right_height + 1) {
            t_push(left);
            t_right(left) = t_join3(t_right(left), node, right);
            return t_balance(left);
        }
        if (right_height > left_height + 1) {
            t_push(right);
            t_left(right) = t_join3(left, node, t_left(right));
            return t_balance(right);
        }
        t_left(node) = left;
        t_right(node) = right;
        t_pull(node);
        t_debug_local_balanced(node);
        return node;
    }
    // split by key: a = keys <= k, b = keys > k  (k is int64 so k = -1 works)
    void t_split(TreeNode node, int64_t key, TreeNode& lower, TreeNode& upper) {
        if (node < 0) { lower = upper = -1; return; }
        t_push(node);
        if (static_cast<int64_t>(tn[tree_index(node)].key) <= key) {
            TreeNode left = t_left(node), right_le = -1;
            TreeNode right = t_right(node);
            t_left(node) = t_right(node) = -1;
            t_pull(node);
            t_split(right, key, right_le, upper);
            lower = t_join3(left, node, right_le);
        } else {
            TreeNode left = t_left(node), left_gt = -1;
            TreeNode right = t_right(node);
            t_left(node) = t_right(node) = -1;
            t_pull(node);
            t_split(left, key, lower, left_gt);
            upper = t_join3(left_gt, node, right);
        }
    }
    TreeNode t_detach_min(TreeNode node, TreeNode& min_node) {
        t_push(node);
        if (t_left(node) < 0) {
            TreeNode right = t_right(node);
            t_left(node) = t_right(node) = -1;
            t_pull(node);
            min_node = node;
            return right;
        }
        t_left(node) = t_detach_min(t_left(node), min_node);
        return t_balance(node);
    }
    TreeNode t_merge(TreeNode left, TreeNode right) {
        if (left < 0) return right;
        if (right < 0) return left;
        if (t_height(left) > t_height(right) + 1) {
            t_push(left);
            t_right(left) = t_merge(t_right(left), right);
            return t_balance(left);
        }
        if (t_height(right) > t_height(left) + 1) {
            t_push(right);
            t_left(right) = t_merge(left, t_left(right));
            return t_balance(right);
        }
        TreeNode root = -1;
        right = t_detach_min(right, root);
        t_left(root) = left;
        t_right(root) = right;
        return t_balance(root);
    }
    TreeNode t_build_balanced(const std::vector<uint32_t>& order, size_t begin, size_t end) {
        if (begin == end) return -1;
        size_t mid = begin + (end - begin) / 2;
        TreeNode node = side_tree_node(order[mid]);
        t_budget(node) = 0;
        t_subtree_min(node) = 0;
        t_lazy(node) = 0;
        t_stored_height(node) = 1;
        in_tree[tree_index(node)] = 1;
        t_left(node) = t_build_balanced(order, begin, mid);
        t_right(node) = t_build_balanced(order, mid + 1, end);
        t_pull(node);
        return node;
    }
#ifndef NDEBUG
    int t_debug_validate_rec(TreeNode node, int64_t lower, int64_t upper) const {
        if (node < 0) return 0;
        int64_t key = static_cast<int64_t>(side_pos[tree_index(node)]);
        if (key <= lower || key >= upper || !in_tree[tree_index(node)])
            throw std::logic_error("AVL global key/membership invariant failed");
        TreeNode left = t_left(node);
        TreeNode right = t_right(node);
        int left_height = t_debug_validate_rec(left, lower, key);
        int right_height = t_debug_validate_rec(right, key, upper);
        if (std::abs(left_height - right_height) > 1 ||
            t_stored_height(node) != static_cast<uint8_t>(1 + std::max(left_height, right_height)))
            throw std::logic_error("AVL global balance/height invariant failed");
        int64_t subtree_min = t_budget(node);
        if (left >= 0)
            subtree_min = std::min(subtree_min,
                                   static_cast<int64_t>(t_subtree_min(left)) + t_lazy(node));
        if (right >= 0)
            subtree_min = std::min(subtree_min,
                                   static_cast<int64_t>(t_subtree_min(right)) + t_lazy(node));
        if (subtree_min != t_subtree_min(node))
            throw std::logic_error("AVL global lazy-min invariant failed");
        return 1 + std::max(left_height, right_height);
    }
    void t_debug_validate_slot(size_t slot) const {
        static const bool enabled = std::getenv("KCS_AVL_CHECK") != nullptr;
        if (enabled)
            t_debug_validate_rec(troot[slot], -1,
                                 static_cast<int64_t>(std::numeric_limits<uint32_t>::max()) + 1);
    }
#else
    inline void t_debug_validate_slot(size_t) const {}
#endif
    TreeNode t_insert_rec(TreeNode root, TreeNode node) {
        if (root < 0) return node;
        t_push(root);
        if (tn[tree_index(node)].key < tn[tree_index(root)].key)
            t_left(root) = t_insert_rec(t_left(root), node);
        else
            t_right(root) = t_insert_rec(t_right(root), node);
        return t_balance(root);
    }
    void t_insert(size_t slot, TreeNode node) {
        t_left(node) = t_right(node) = -1;
        t_lazy(node) = 0;
        t_subtree_min(node) = t_budget(node);
        t_stored_height(node) = 1;
        troot[slot] = t_insert_rec(troot[slot], node);
        in_tree[tree_index(node)] = 1;
        t_debug_validate_slot(slot);
    }
    TreeNode t_erase_rec(TreeNode root, int64_t key, TreeNode& removed) {
        if (root < 0) throw std::logic_error("AVL erase key missing");
        t_push(root);
        int64_t root_key = static_cast<int64_t>(tn[tree_index(root)].key);
        if (key < root_key) {
            t_left(root) = t_erase_rec(t_left(root), key, removed);
            return t_balance(root);
        }
        if (key > root_key) {
            t_right(root) = t_erase_rec(t_right(root), key, removed);
            return t_balance(root);
        }
        removed = root;
        TreeNode left = t_left(root), right = t_right(root);
        t_left(root) = t_right(root) = -1;
        t_pull(root);
        return t_merge(left, right);
    }
    void t_erase(size_t slot, TreeNode node) {
        int64_t key = static_cast<int64_t>(side_pos[tree_index(node)]);
        TreeNode removed = -1;
        troot[slot] = t_erase_rec(troot[slot], key, removed);
        if (removed != node) throw std::logic_error("AVL erase key mismatch");
        in_tree[tree_index(node)] = 0;
        t_debug_validate_slot(slot);
    }
    void t_range_add_rec(TreeNode node, int64_t lo, int64_t hi,
                         int64_t klo, int64_t khi, int32_t delta) {
        if (node < 0) return;
        if (klo <= lo && hi <= khi) { t_apply(node, delta); return; }
        t_push(node);
        const int64_t k = static_cast<int64_t>(tn[tree_index(node)].key);
        if (klo <= k && k <= khi) t_budget(node) += delta;
        if (klo <= k - 1) t_range_add_rec(t_left(node), lo, k - 1, klo, khi, delta);
        if (khi >= k + 1) t_range_add_rec(t_right(node), k + 1, hi, klo, khi, delta);
        t_pull(node);
    }
    void t_range_add(size_t slot, uint32_t key_lower, uint32_t key_upper, int32_t delta) {
        ++st.range_adds;
        t_range_add_rec(troot[slot], 0,
                        static_cast<int64_t>(std::numeric_limits<uint32_t>::max()),
                        static_cast<int64_t>(key_lower), static_cast<int64_t>(key_upper), delta);
        t_debug_validate_slot(slot);
    }
    // Remove the leftmost negative-budget side in one root traversal.  Rebalancing
    // happens while that same path unwinds; no second key search is needed.
    TreeNode t_extract_neg_rec(TreeNode& root) {
        if (root < 0 || t_subtree_min(root) >= 0)
            throw std::logic_error("AVL negative extraction precondition failed");
        TreeNode node = root;
        t_push(node);
        TreeNode left = t_left(node);
        if (left >= 0 && t_subtree_min(left) < 0) {
            TreeNode victim = t_extract_neg_rec(t_left(node));
            root = t_balance(node);
            return victim;
        }
        if (t_budget(node) < 0) {
            root = t_merge(t_left(node), t_right(node));
            t_left(node) = t_right(node) = -1;
            t_lazy(node) = 0;
            t_subtree_min(node) = t_budget(node);
            t_stored_height(node) = 1;
            return node;
        }
        TreeNode victim = t_extract_neg_rec(t_right(node));
        root = t_balance(node);
        return victim;
    }
    TreeNode t_extract_neg(size_t slot) {
        TreeNode victim = t_extract_neg_rec(troot[slot]);
        in_tree[tree_index(victim)] = 0;
        t_debug_validate_slot(slot);
        return victim;
    }

    // ---------- window helpers ----------
    inline void window_range(NodeId x, Time t, Time delta, uint32_t& pl, uint32_t& pr_incl) {
        size_t base = graph.csr.node_begin(x);
        int lo = graph.csr.lower_time(x, lower_window(t, delta));
        int hi = graph.csr.upper_time(x, upper_window(t, delta));
        pl = static_cast<uint32_t>(base) + static_cast<uint32_t>(lo);
        pr_incl = static_cast<uint32_t>(base) + static_cast<uint32_t>(hi) - 1;   // hi > lo always (self)
    }

    // select the (rank)-th live entry of block [bs, bs+len) (1-based rank; 0-based index)
    inline int fen_select(size_t bs, int len, int rank) const {
        int idx = 0, acc = 0;
        int step = len > 0 ? (1 << (31 - __builtin_clz(static_cast<unsigned>(len)))) : 0;
        for (; step > 0; step >>= 1) {
            int nxt = idx + step;
            if (nxt <= len && acc + occ_bit[bs + static_cast<size_t>(nxt - 1)] < rank) {
                idx = nxt; acc += occ_bit[bs + static_cast<size_t>(nxt - 1)];
            }
        }
        return idx;
    }

    // (k)-th largest relevant gap for side at node x, level h, center t, radius delta;
    // returns 0 if fewer than k relevant members have gap >= 1.
    // k == 1 is two Fenwick selects (the outermost live members); k > 1 is a rank-space
    // two-array order-statistic selection (P1) over the position-sorted left/right runs.
    Time timer_gap(uint32_t sid, NodeId x, int h, Time t, Time delta, int k, int cnt_delta,
                   const TimerWindow* timer_window = nullptr) {
        if (cnt_delta < k) return 0;                 // fewer than k relevant members in total
        size_t slot = dir_slot(x, h);
        size_t bs, be;
        int len, rank_before;
        if (timer_window && timer_window->matches(x, h, t, delta, cnt_delta)) {
            bs = timer_window->bs;
            len = timer_window->len;
            rank_before = timer_window->rank_before;
            be = bs + static_cast<size_t>(len);
        } else {
            bs = blk_off[slot]; be = blk_off[slot + 1];
            len = static_cast<int>(be - bs);
            uint32_t pl, pr;
            window_range(x, t, delta, pl, pr);
            auto block_begin = occ_iterator(bs);
            auto first = std::lower_bound(block_begin, occ_iterator(be), pl);
            rank_before = fen_pref(bs, static_cast<int>(first - block_begin));
        }
        auto block_begin = occ_iterator(bs);
        // outermost live entries within the window (cnt_delta live entries from rank_before+1)
        int i_first = fen_select(bs, len, rank_before + 1);
        int i_last = fen_select(bs, len, rank_before + cnt_delta);
        Time t_first = graph.csr.incidences[occ_pos[bs + static_cast<size_t>(i_first)]].t;
        Time t_last = graph.csr.incidences[occ_pos[bs + static_cast<size_t>(i_last)]].t;
        Time g1 = std::max(t - t_first, t_last - t);
        if (g1 <= 0) return 0;                       // all relevant members share the center time
        if (k == 1) return g1;
        // ---- P1: rank-space order-statistic selection of the (k)-th largest gap ----
        // The cnt_delta window-live level-h members occupy the contiguous live-rank
        // band [rank_before+1, rank_before+cnt_delta] and are position/time sorted.
        // Split at the center t into a LEFT run (time <= t: gaps t - time, non-increasing
        // in live-rank) and a RIGHT run (time > t: gaps time - t, non-increasing from the
        // far end).  Both runs are descending, so the (k)-th largest gap is the classic
        // k-th order statistic of two sorted arrays, found by a two-array partition search
        // in O(log a) Fenwick selects instead of O(log span) value-space occ_counts.
        int hi_le = graph.csr.upper_time(x, t);      // # incidences of x with time <= t
        uint32_t Pb = static_cast<uint32_t>(graph.csr.node_begin(x)) + static_cast<uint32_t>(hi_le);
        int C = static_cast<int>(std::lower_bound(block_begin, occ_iterator(be), Pb) - block_begin);
        int nL = fen_pref(bs, C) - rank_before;      // window-live members with time <= t (>= 1: self)
        int nR = cnt_delta - nL;                     // window-live members with time > t
        const Time POS_INF = std::numeric_limits<Time>::max(), NEG_INF = -1;
        // The partition probe and the final min share endpoint selections.  Keep this
        // tiny per-arm memo so seeding never pays twice for the same exact rank.
        struct GapMemo { int idx[64]; Time val[64]; int n = 0; } lmemo, rmemo;
        auto Lgap = [&](int idx) -> Time {           // idx-th largest LEFT gap (0-based, 0..nL-1)
            for (int q = 0; q < lmemo.n; ++q) if (lmemo.idx[q] == idx) return lmemo.val[q];
            ++st.timer_probes;
            int bi = fen_select(bs, len, rank_before + 1 + idx);
            Time g = t - graph.csr.incidences[occ_pos[bs + static_cast<size_t>(bi)]].t;
            if (lmemo.n < 64) { lmemo.idx[lmemo.n] = idx; lmemo.val[lmemo.n++] = g; }
            return g;
        };
        auto Rgap = [&](int idx) -> Time {           // idx-th largest RIGHT gap (0-based, 0..nR-1)
            for (int q = 0; q < rmemo.n; ++q) if (rmemo.idx[q] == idx) return rmemo.val[q];
            ++st.timer_probes;
            int bi = fen_select(bs, len, rank_before + cnt_delta - idx);
            Time g = graph.csr.incidences[occ_pos[bs + static_cast<size_t>(bi)]].t - t;
            if (rmemo.n < 64) { rmemo.idx[rmemo.n] = idx; rmemo.val[rmemo.n++] = g; }
            return g;
        };
        // Pick the top k as (top i of L) u (top k-i of R).  Lin(i) = smallest chosen-left
        // is non-increasing in i; Rout(i) = largest unchosen-right is increasing in i, so
        // the predicate Lin(i) >= Rout(i) is true-then-false: binary-search its last true i.
        int iLo = std::max(0, k - nR), iHi = std::min(k, nL);
        // Re-arms of a side normally keep the two-run split close to its prior exact
        // partition.  Probe that split first, then binary-search only the remaining
        // half-bracket.  This is the same monotone predicate and returns the same
        // last-true index as the from-scratch search, including ties.
        int seed = timer_split[sid];
        if (seed >= iLo && seed <= iHi) {
            Time Lin = (seed >= 1) ? Lgap(seed - 1) : POS_INF;
            Time Rout = (k - seed < nR) ? Rgap(k - seed) : NEG_INF;
            if (Lin >= Rout) iLo = seed; else iHi = seed - 1;
        }
        while (iLo < iHi) {
            int i = iLo + (iHi - iLo + 1) / 2;
            Time Lin = (i >= 1) ? Lgap(i - 1) : POS_INF;
            Time Rout = (k - i < nR) ? Rgap(k - i) : NEG_INF;
            if (Lin >= Rout) iLo = i; else iHi = i - 1;
        }
        int i = iLo, j = k - i;
        timer_split[sid] = i;
        Time Lin = (i >= 1) ? Lgap(i - 1) : POS_INF;
        Time Rin = (j >= 1) ? Rgap(j - 1) : POS_INF;
        Time gk = std::min(Lin, Rin);                // k-th largest gap = smallest of the chosen top-k
        return gk >= 1 ? gk : 0;                      // < 1 <=> fewer than k members with gap >= 1
    }

    // --relax departure alarm.  Split the certificate's departure allowance between
    // the two time flanks and alarm
    // when either flank spends its share.  Since level-h members are a subset of raw
    // incidences, this is no later than the corresponding relevant-departure alarm.
    // The conservative flank ranks are addressed directly in the static CSR arrays;
    // no live Fenwick rank selection or two-array partition is needed.
    Time relax_timer_gap(NodeId x, Time t, Time delta, int allowance) const {
        int lo = graph.csr.lower_time(x, lower_window(t, delta));
        int hi = graph.csr.upper_time(x, upper_window(t, delta));
        int left_end = std::min(hi, std::max(lo, graph.csr.lower_time(x, t)));
        int right_begin = std::min(hi, std::max(lo, graph.csr.upper_time(x, t)));
        int allow_left = allowance / 2;
        int allow_right = allowance - allow_left;
        Time gl = 0, gr = 0;
        if (left_end - lo > allow_left)
            gl = t - graph.csr.time_at(x, lo + allow_left);
        if (hi - right_begin > allow_right)
            gr = graph.csr.time_at(x, hi - 1 - allow_right) - t;
        return std::max(gl, gr);
    }

    BucketQueueMax rh;
    IndexedTimerMax iq;
    bool indexed_timer = false;

    inline NodeId side_node(uint32_t sid) const {
        const Edge& ed = graph.edges[sid >> 1];
        return (sid & 1) ? ed.v : ed.u;
    }

    inline bool eager_side(uint32_t sid) const {
        return hybrid_active && eager[sid] != 0;
    }
    inline uint32_t advance_epoch(uint32_t sid) {
        // Lazy invalidation is exact only while generations cannot wrap and make an
        // ancient queue record look current again.  Abort before that collision.
        if (epoch[sid] == std::numeric_limits<uint32_t>::max()) {
            throw std::overflow_error("Certificate epoch exhausted for side " + std::to_string(sid));
        }
        return ++epoch[sid];
    }
    inline uint32_t side_level(uint32_t sid) const {
        return eager_side(sid) ? eager_H[sid] : Hs[sid];
    }
    inline int eager_capval(uint32_t sid, uint32_t v) const {
        return static_cast<int>(std::min<uint32_t>(v, static_cast<uint32_t>(eager_cap[sid])));
    }
    bool eager_rebalance(uint32_t sid) {
        uint32_t oldH = eager_H[sid];
        while (eager_H[sid] > 0 && eager_Sgeq[sid] < static_cast<int>(eager_H[sid])) {
            --eager_H[sid];
            eager_Sgeq[sid] += eager_hist[eager_histoff[sid] + eager_H[sid]];
        }
        return eager_H[sid] != oldH;
    }
    bool eager_dec_value(uint32_t sid, int ov, int nv) {
        --eager_hist[eager_histoff[sid] + static_cast<size_t>(ov)];
        ++eager_hist[eager_histoff[sid] + static_cast<size_t>(nv)];
        if (ov >= static_cast<int>(eager_H[sid]) && nv < static_cast<int>(eager_H[sid])) {
            --eager_Sgeq[sid];
            return eager_rebalance(sid);
        }
        return false;
    }
    bool eager_rem_value(uint32_t sid, int v) {
        --eager_hist[eager_histoff[sid] + static_cast<size_t>(v)];
        if (v >= static_cast<int>(eager_H[sid])) {
            --eager_Sgeq[sid];
            return eager_rebalance(sid);
        }
        return false;
    }
    void eager_enqueue(EdgeId e) {
        size_t ei = static_cast<size_t>(e);
        if (std::min(side_level(static_cast<uint32_t>(2 * ei)),
                     side_level(static_cast<uint32_t>(2 * ei + 1))) < c[ei] && !eager_inq[ei]) {
            eager_inq[ei] = 1;
            eager_work.push_back(e);
        }
    }
    void eager_departure(uint32_t sid, uint32_t member_pos) {
        const Incidence& in = graph.csr.incidences[member_pos];
        int v = eager_capval(sid, c[static_cast<size_t>(in.edge)]);
        if (eager_rem_value(sid, v)) {
            eager_enqueue(static_cast<EdgeId>(sid >> 1));
        }
    }
    void eager_note_drop(NodeId x, Time t, uint32_t old, uint32_t nv, Time delta) {
        int lo = graph.csr.lower_time(x, lower_window(t, delta));
        int hi = graph.csr.upper_time(x, upper_window(t, delta));
        size_t begin = graph.csr.node_begin(x);
        for (int i = lo; i < hi; ++i) {
            const Incidence& in = graph.csr.incidences[begin + static_cast<size_t>(i)];
            uint32_t sid = 2 * in.edge + in.side;
            if (!eager_side(sid)) continue;
            int ov = eager_capval(sid, old), nvv = eager_capval(sid, nv);
            if (ov == nvv) continue;
            eager_dec_value(sid, ov, nvv);
            eager_enqueue(in.edge);
        }
    }
    void eager_reeval(EdgeId e, Time delta) {
        size_t ei = static_cast<size_t>(e);
        uint32_t nv = std::min(side_level(static_cast<uint32_t>(2 * ei)),
                               side_level(static_cast<uint32_t>(2 * ei + 1)));
        if (nv < c[ei]) do_drop(e, nv, delta);
    }
    void drain_hybrid(Time delta) {
        while (!eager_work.empty() || !wakeq.empty()) {
            while (!eager_work.empty()) {
                EdgeId e = eager_work.back(); eager_work.pop_back();
                eager_inq[static_cast<size_t>(e)] = 0;
                eager_reeval(e, delta);
            }
            if (!wakeq.empty()) {
                uint32_t sid = wakeq.back(); wakeq.pop_back();
                in_wake[sid] = 0;
                bool tf = tfired[sid] != 0; tfired[sid] = 0;
                int php = ph_enter(PH_CASC);
                verify_side(sid, delta, tf);
                ph_exit(php);
            }
        }
    }

    // full (re-)arm for side sid at level h with fresh count cnt at radius delta.
    // Any split with a + b = r is never-late-safe; ssplit adapts it geometrically
    // toward the trigger that has NOT been false-alarming.
    void arm(uint32_t sid, NodeId x, Time t, int h, int cnt, Time delta,
             const TimerWindow* timer_window = nullptr) {
        ++st.arms;
        // The indexed queue replaces/removes the unique record in place.  Only the
        // hybrid legacy queue needs a generation bump for lazy invalidation.
        if (!indexed_timer) advance_epoch(sid);
        int r = cnt - h;                              // slack >= 0
        if (relaxed) {
            // Keep a square-root coarse margin for direct drop notifications and give
            // the remaining slack to the cheap raw-flank timer.  Thus a+b = r: an
            // invalid side must exceed at least one budget, so alarms cannot be late.
            int b = std::min(r / 2, static_cast<int>(std::sqrt(static_cast<double>(r))));
            int a = r - b;
            rbud[sid] = b;
            Time g = relax_timer_gap(x, t, delta, a);
            if (indexed_timer) iq.schedule(sid, g);
            else if (g >= 1) rh.push(g, sid, epoch[sid]);
            return;
        }
        int b = r * static_cast<int>(ssplit[sid]) / 8;   // drop budget (adaptive eighths)
        int a = r - b;                                // departure budget
        if (g_propsplit) sa[sid] = a;
        Time g = timer_gap(sid, x, h, t, delta, a + 1, cnt, timer_window);
        if (indexed_timer) iq.schedule(sid, g);
        else if (g >= 1) rh.push(g, sid, epoch[sid]);
        tn[sid].bud = b;
        if (g_propsplit || g_fadiag) armed_b[sid] = b;
        t_insert(dir_slot(x, h), side_tree_node(sid));
    }

    // P2 role-aware (re-)arm.  Only min(H_u, H_v) is output-visible, so a side whose
    // level `lvl` strictly exceeds the other side's pinned level otherH is a LOSER: it
    // gets the cheap threshold certificate at level otherH+1 (fires exactly when it might
    // reach the winner), and Hs[sid] holds the proxy otherH+1 (> otherH), keeping
    // min(Hs[u], Hs[v]) = c(e).  A winner/tie (lvl <= otherH) arms exactly at lvl.
    // cnt_at_lvl = count(v >= lvl in window), reused when the armed level stays lvl.
    void rearm_role(uint32_t sid, NodeId x, Time t, int lvl, Time delta, int cnt_at_lvl,
                    const TimerWindow* timer_window = nullptr) {
        int php = ph_enter(PH_ARM);
        int otherH = static_cast<int>(side_level(sid ^ 1u));
        int hL = (lvl > otherH) ? (otherH + 1) : lvl;
        int cnt;
        const TimerWindow* arm_window = nullptr;
        if (hL == lvl) {
            cnt = cnt_at_lvl;
            if (!relaxed && timer_window && timer_window->matches(x, lvl, t, delta, cnt)) {
                arm_window = timer_window;
                ++st.cheap_rearms;
            }
        } else {
            uint32_t pl, pr;
            window_range(x, t, delta, pl, pr);
            cnt = occ_count(x, hL, pl, pr);
        }
        Hs[sid] = static_cast<uint32_t>(hL);
        arm(sid, x, t, hL, cnt, delta, arm_window);
        ph_exit(php);
    }

    // Deliver one neighbor-value support loss to every relaxed certificate whose
    // centered window contains the dropped incidence and whose armed level was
    // crossed.  The scan may inspect irrelevant sides, but each decision is O(1)
    // and only exact level crossings spend the small integer budget.
    void relax_note_drop(NodeId x, Time t, uint32_t old, uint32_t nv, Time delta) {
        int lo = graph.csr.lower_time(x, lower_window(t, delta));
        int hi = graph.csr.upper_time(x, upper_window(t, delta));
        size_t begin = graph.csr.node_begin(x);
        for (int i = lo; i < hi; ++i) {
            ++st.relax_drop_checks;
            const Incidence& in = graph.csr.incidences[begin + static_cast<size_t>(i)];
            uint32_t sid = 2 * in.edge + in.side;
            if (sid == active_sid) continue;           // its live verify already accounts for self-loss
            uint32_t h = Hs[sid];
            if (h == 0 || old < h || nv >= h) continue;
            ++st.relax_drop_hits;
            if (in_wake[sid]) continue;                // queued verify sees all same-radius losses
            if (--rbud[sid] < 0) {
                ++st.budget_wakes;                     // direct-counter drop alarm
                in_wake[sid] = 1;
                wakeq.push_back(sid);
            }
        }
    }

    // record a value drop of edge e to nv at radius delta; O-deletes + range-adds;
    // pending underflows are collected (drained by the caller's wake loop).
    void do_drop(EdgeId e, uint32_t nv, Time delta) {
        int php = ph_enter(PH_DROP);
        uint32_t old = c[static_cast<size_t>(e)];
        if (nv >= old)
            throw std::logic_error("certificate drop must strictly decrease coreness");
        c[static_cast<size_t>(e)] = nv;
        ++st.breakpoints;
        if (dodump) drops.emplace_back(e, delta + 1, old);
        if (sink) sink->on_drop(e);
        const Edge& ed = graph.edges[static_cast<size_t>(e)];
        for (int sd = 0; sd < 2; ++sd) {
            NodeId x = sd == 0 ? ed.u : ed.v;
            uint32_t p = side_pos[2 * static_cast<size_t>(e) + static_cast<size_t>(sd)];
            int hi_lvl = std::min<int>(static_cast<int>(old), KO[static_cast<size_t>(x)]);
            for (int h = static_cast<int>(nv) + 1; h <= hi_lvl; ++h) occ_delete(x, h, p);
            if (hybrid_active) eager_note_drop(x, ed.t, old, nv, delta);
            if (relaxed) {
                relax_note_drop(x, ed.t, old, nv, delta);
                continue;
            }
            uint32_t pl, pr;
            window_range(x, ed.t, delta, pl, pr);
            for (int h = static_cast<int>(nv) + 1; h <= hi_lvl; ++h) {
                size_t slot = dir_slot(x, h);
                if (troot[slot] < 0) continue;
                t_range_add(slot, pl, pr, -1);
                if (t_subtree_min(troot[slot]) < 0) pend_lvls.push_back(slot);
            }
        }
        if (!relaxed) {
            // block boundary: drain underflows into the wake queue
            for (size_t slot : pend_lvls) {
                while (troot[slot] >= 0 && t_subtree_min(troot[slot]) < 0) {
                    TreeNode side_node_id = t_extract_neg(slot);
                    ++st.budget_wakes;
                    size_t side_index = tree_index(side_node_id);
                    if (!in_wake[side_index]) {
                        in_wake[side_index] = 1;
                        wakeq.push_back(static_cast<uint32_t>(side_node_id));
                    }
                }
            }
            pend_lvls.clear();
        }
        ph_exit(php);
    }

    // live re-verification of side sid at radius delta (the certificate wake handler).
    // timer_fired: wake source was the departure timer (its queue entry is consumed).
    void verify_side(uint32_t sid, Time delta, bool timer_fired) {
        ++st.recomputes;
        int cs_b = 0; uint64_t cs_t0 = 0;
        if (g_census) {
            cs_b = cs_bucket[sid];
            ++cs_cnt[cs_b];
            if (cs_last_batch[sid] == cs_batch) ++cs_repeats;
            else cs_last_batch[sid] = cs_batch;
            cs_t0 = census_now();
        }
        uint32_t previous_active = active_sid;
        active_sid = sid;
        size_t e = sid >> 1;
        NodeId x = side_node(sid);
        const Edge& ed = graph.edges[e];
        int h = static_cast<int>(Hs[sid]);
        if (!relaxed && in_tree[sid]) t_erase(dir_slot(x, h), static_cast<int>(sid));
        if (h == 0) {
            if (indexed_timer) iq.cancel(sid);
            else advance_epoch(sid);
            active_sid = previous_active;
            if (g_census) cs_cyc[cs_b] += census_now() - cs_t0;
            return;
        }
        uint32_t pl, pr;
        window_range(x, ed.t, delta, pl, pr);
        TimerWindow timer_window;
        int cnt = occ_count(x, h, pl, pr, &timer_window);
        timer_window.bind(x, h, ed.t, delta, pl, pr, cnt);
        bool dropped_any = false;
        for (;;) {
            if (cnt < h) {
                // descend to the live h-index: max h' <= h-1 with count(h') >= h'.
                // Fast path: single-level drops dominate; else binary search (P is monotone).
                int nh = h - 1;
                TimerWindow next_window;
                int cm = nh > 0 ? occ_count(x, nh, pl, pr, &next_window) : 0;
                if (nh > 0) next_window.bind(x, nh, ed.t, delta, pl, pr, cm);
                if (nh > 0 && cm < nh) {
                    int lo = 0, hi = nh - 1;
                    while (lo < hi) {
                        int mid = lo + (hi - lo + 1) / 2;
                        if (occ_count(x, mid, pl, pr) >= mid) lo = mid;
                        else hi = mid - 1;
                    }
                    nh = lo;
                    cm = nh > 0 ? occ_count(x, nh, pl, pr, &next_window) : 0;
                    if (nh > 0) next_window.bind(x, nh, ed.t, delta, pl, pr, cm);
                }
                st.h_drop_units += h - nh;
                h = nh; cnt = cm; timer_window = next_window;
            }
            if (h == static_cast<int>(Hs[sid])) break;
            Hs[sid] = static_cast<uint32_t>(h);
            dropped_any = true;
            uint32_t nv = std::min(side_level(static_cast<uint32_t>(2 * e)),
                                   side_level(static_cast<uint32_t>(2 * e + 1)));
            if (nv < c[e]) {
                do_drop(static_cast<EdgeId>(e), nv, delta);
                if (h > 0) {
                    cnt = occ_count(x, h, pl, pr, &timer_window);
                    timer_window.bind(x, h, ed.t, delta, pl, pr, cnt);
                    continue;
                }  // self-drop may cross h
            }
            break;
        }
        if (dropped_any) {
            ++st.real_alarms;
            if (h >= 1) rearm_role(sid, x, ed.t, h, delta, cnt, &timer_window);
            else if (indexed_timer) iq.cancel(sid);    // side is dead; kill its timer
            else advance_epoch(sid);
            active_sid = previous_active;
            if (g_census) cs_cyc[cs_b] += census_now() - cs_t0;
            return;
        }
        ++st.false_alarms;
        if (g_census) ++cs_false[cs_b];
        if (g_fadiag && !relaxed) {
            if (timer_fired) {
                ++fd->trigger_timer;
                int remaining = tn[sid].bud;       // materialized by t_erase above
                if (remaining < 0) {
                    ++fd->timer_unspent_underflow;
                } else if (remaining == 0) {
                    ++fd->timer_unspent_zero;
                } else {
                    int bucket = 31 - __builtin_clz(static_cast<unsigned>(remaining));
                    ++fd->timer_unspent_log2[bucket];
                }
                if (remaining == armed_b[sid]) ++fd->timer_no_drops;
            } else {
                ++fd->trigger_budget;
            }
            ++fd->fresh_slack[cnt - h];
            ++fd->ssplit[ssplit[sid]];             // split before this FA adapts it
        }
        // Adaptive slack split (any a + b = r is never-late-safe).  KCS_PROPSPLIT
        // computes round(8X/(X+a+1)), using the drops spent before this false alarm and
        // the armed departure budget (plus its triggering gap), then averages that target
        // with the prior split for mild hysteresis.  Direct-set oscillated at the exact
        // corners by immediately trading timer FAs for drop FAs.  With the flag unset,
        // preserve the historical geometric +/-1 trigger drift.
        // A budget-false-alarm "keep the timer" shortcut is provably unreachable (after
        // b+1 drops the fresh slack r' <= a-1 < a), so every false alarm takes a full re-arm.
        if (!relaxed) {
            const int split_lo = g_corner ? 0 : 1;
            const int split_hi = g_corner ? 8 : 7;
            if (g_propsplit) {
                const int spent = armed_b[sid] - tn[sid].bud; // tbud materialized by t_erase
                const int denom = spent + sa[sid] + 1;
                int target = static_cast<int>((8ll * spent + denom / 2) / denom);
                target = std::max(split_lo, std::min(split_hi, target));
                ssplit[sid] = static_cast<uint8_t>((static_cast<int>(ssplit[sid]) + target) / 2);
            } else if (!timer_fired) {
                if (ssplit[sid] < split_hi) ++ssplit[sid]; // drops are hot
            } else {
                if (ssplit[sid] > split_lo) --ssplit[sid]; // departures are hot
            }
        }
        rearm_role(sid, x, ed.t, h, delta, cnt, &timer_window);
        active_sid = previous_active;
        if (g_census) cs_cyc[cs_b] += census_now() - cs_t0;
    }

    void run() {
        Timer t_init; t_init.start();
        // ---- build occurrence structures from the c_inf seed ----
        KO.assign(n, 0);
        // Labels only decrease during the sweep.  Therefore no incidence at x can
        // ever support a level above the largest incident seed label, and a side
        // h-index (or its loser proxy) cannot exceed that maximum either.  Using
        // max_{e incident to x} c_inf(e) is thus an exact level-directory bound;
        // it shrinks empty (x,h) directories without changing occurrence payloads.
        for (size_t e = 0; e < m; ++e) {
            if (c[e] > static_cast<uint32_t>(std::numeric_limits<int>::max())) {
                throw std::overflow_error("Seed coreness exceeds signed level encoding");
            }
            int cv = static_cast<int>(c[e]);
            const Edge& ed = graph.edges[e];
            KO[static_cast<size_t>(ed.u)] = std::max(KO[static_cast<size_t>(ed.u)], cv);
            KO[static_cast<size_t>(ed.v)] = std::max(KO[static_cast<size_t>(ed.v)], cv);
        }
        alloc_log("KO", n, sizeof(int));
        alloc_log("lvl_off", n + 1, sizeof(size_t));
        lvl_off.assign(n + 1, 0);
        for (size_t x = 0; x < n; ++x) lvl_off[x + 1] = lvl_off[x] + static_cast<size_t>(KO[x]);
        size_t nlvls = lvl_off[n];   // = sum_x KO[x] <= 2m  (level-directory dimension)
        alloc_log("blk_off", nlvls + 1, sizeof(size_t));
        blk_off.assign(nlvls + 1, 0);
        // count per (x,h): number of positions with min(c, KO[x]) >= h
        {
            // per node walk (positions are contiguous per node)
            for (size_t x = 0; x < n; ++x) {
                size_t b = graph.csr.node_begin(static_cast<NodeId>(x));
                size_t e = graph.csr.node_end(static_cast<NodeId>(x));
                int kox = KO[x];
                if (kox == 0) continue;
                // difference trick: level h in [1, min(c, kox)] gets +1
                for (size_t p = b; p < e; ++p) {
                    int v = static_cast<int>(std::min<uint32_t>(c[graph.csr.incidences[p].edge], static_cast<uint32_t>(kox)));
                    if (v >= 1) {
                        blk_off[lvl_off[x] + 1] += 1;                 // level 1 gains one
                        if (v < kox) blk_off[lvl_off[x] + static_cast<size_t>(v) + 1] -= 1;  // levels > v lose it
                    }
                }
                // prefix within the node's levels turns diffs into counts
                for (int h = 2; h <= kox; ++h)
                    blk_off[lvl_off[x] + static_cast<size_t>(h)] += blk_off[lvl_off[x] + static_cast<size_t>(h) - 1];
            }
            // global prefix sum into offsets (blk_off currently holds counts at slot+1)
            for (size_t i = 1; i <= nlvls; ++i) blk_off[i] += blk_off[i - 1];
        }
        // Overflow tripwire: blk_off is a prefix sum of non-negative per-(x,h) counts,
        // so it MUST be non-decreasing.  A size_t count/offset overflow (or a corrupted
        // difference-array cancellation) would produce a wrapped huge/negative slot and
        // break monotonicity -> we abort with a precise message instead of letting the
        // subsequent occ_pos.assign() request an absurd size and die in bad_alloc.
        // C0 = blk_off[nlvls] is the true occurrence-structure size = sum_p min(c_p,KO).
        {
            size_t bad = SIZE_MAX;
            for (size_t i = 1; i <= nlvls; ++i) {
                if (blk_off[i] < blk_off[i - 1]) { bad = i; break; }
            }
            if (g_alloclog) {
                std::cerr << "OCC nlvls=" << nlvls << " C0_occ_entries=" << blk_off[nlvls]
                          << " (approx 2*sum_e c_e) monotone=" << (bad == SIZE_MAX)
                          << " maxc=" << maxc << "\n";
            }
            if (bad != SIZE_MAX) {
                throw std::overflow_error("Occurrence offset prefix sum overflow at slot " +
                                          std::to_string(bad));
            }
        }
        alloc_log("occ_pos", blk_off[nlvls], sizeof(uint32_t));
        occ_pos.assign(blk_off[nlvls], 0);
        {
            std::vector<size_t> cursor(blk_off.begin(), blk_off.end() - 1);
            for (size_t x = 0; x < n; ++x) {
                size_t b = graph.csr.node_begin(static_cast<NodeId>(x));
                size_t e = graph.csr.node_end(static_cast<NodeId>(x));
                int kox = KO[x];
                for (size_t p = b; p < e; ++p) {
                    int v = static_cast<int>(std::min<uint32_t>(c[graph.csr.incidences[p].edge], static_cast<uint32_t>(kox)));
                    for (int h = 1; h <= v; ++h)
                        occ_pos[cursor[lvl_off[x] + static_cast<size_t>(h - 1)]++] = static_cast<uint32_t>(p);
                }
            }
        }
        alloc_log("occ_bit", occ_pos.size(), sizeof(int32_t));
        occ_bit.resize(occ_pos.size());
        for (size_t slot = 0; slot < nlvls; ++slot) {
            size_t bs = blk_off[slot], be = blk_off[slot + 1];
            int len = static_cast<int>(be - bs);
            for (int i = 1; i <= len; ++i)
                occ_bit[bs + static_cast<size_t>(i - 1)] = static_cast<int32_t>(i & -i);
        }

        // ---- side state ---- (S = 2m sides; strict AVL state is omitted by --relax)
        size_t S = 2 * m;
        size_t side_bytes = sizeof(uint32_t) + 2 * sizeof(uint8_t); // Hs, tfired, in_wake
        Hs.assign(S, 0);
        tfired.assign(S, 0);
        in_wake.assign(S, 0);
        if (relaxed) {
            side_bytes += sizeof(int32_t); // rbud
            rbud.assign(S, 0);        // sentinel: first relevant drop wakes the side
        } else {
            side_bytes += sizeof(int32_t) + 2 * sizeof(uint8_t) +
                          2 * sizeof(int32_t) + sizeof(uint8_t) + 3 * sizeof(int32_t);
            // timer_split + in_tree + ssplit + tl/tr + thgt + tbud/tmin/tlz
            if (g_propsplit) {
                side_bytes += sizeof(int32_t);
                sa.assign(S, 0);
            }
            if (g_propsplit || g_fadiag) {
                side_bytes += sizeof(int32_t);
                armed_b.assign(S, 0);
            }
            ssplit.assign(S, 4);      // start with the even split b = floor(r/2)
            timer_split.assign(S, -1);
            in_tree.assign(S, 0);
            tn.assign(S, TN{});
            for (size_t i = 0; i < S; ++i) tn[i].key = side_pos[i];
            if (g_fadiag) {
                fd = std::make_unique<Fadiag>();
            }
        }
        if (!relaxed) {
            alloc_log("troot", nlvls, sizeof(int32_t));
            troot.assign(nlvls, -1);
        }

        // ---- initial H per side + P2 winner/loser roles (P3: O(1) occ_counts) ----
        // The seed guarantees min(exactH_u, exactH_v) = c[e], so the post-P2 level of
        // each side is either c[e] (winner/tie) or the loser proxy c[e]+1; a side is a
        // strict loser iff occ_count at level c[e]+1 reaches c[e]+1 (the level predicate
        // is monotone).  Two O(1)-level occ_counts per side replace the old binary
        // search over levels; the count at level c[e] doubles as the seed assertion.
        std::vector<uint32_t> wpl(S), wpr(S);        // per-side window at Dmax (reused below)
        if (eager_limit) eager.assign(S, 0);
        if (g_census) { cs_bucket.assign(S, 0); cs_last_batch.assign(S, 0xFFFFFFFFu); }
        for (size_t e = 0; e < m; ++e) {
            int cv = static_cast<int>(c[e]);
            int losers = 0;
            for (int sd = 0; sd < 2; ++sd) {
                uint32_t sid = 2 * static_cast<uint32_t>(e) + static_cast<uint32_t>(sd);
                NodeId x = side_node(sid);
                const Edge& ed = graph.edges[e];
                uint32_t pl, pr;
                window_range(x, ed.t, Dmax, pl, pr);
                wpl[sid] = pl; wpr[sid] = pr;
                uint32_t xi = (pr - pl) + 1u;         // same xi_s used by the census
                if (g_census)                          // xi >= 1 (window contains self)
                    cs_bucket[sid] = static_cast<uint8_t>(31 - __builtin_clz(xi));
                if (eager_limit && xi <= eager_limit) eager[sid] = 1;
                int kox = KO[static_cast<size_t>(x)];
                if (cv > kox || (cv >= 1 && occ_count(x, cv, pl, pr) < cv))
                    throw std::logic_error("certificate side initialization fell below the seed");
                bool loser = (cv + 1 <= kox) && occ_count(x, cv + 1, pl, pr) >= cv + 1;
                Hs[sid] = static_cast<uint32_t>(cv + (loser ? 1 : 0));
                if (loser) ++losers;
            }
            if (losers == 2)
                throw std::logic_error("certificate initialization made both sides exceed the seed");
        }

        // Eager sides use the strict engine's exact capped histogram from the same
        // Dmax window.  Lazy sides retain their P2 proxy Hs and certificate state.
        if (eager_limit) {
            size_t eager_count = 0;
            for (uint8_t v : eager) eager_count += v;
            hybrid_active = eager_count != 0;
        }
        if (hybrid_active) {
            eager_cap.assign(S, 0);
            std::vector<int> scratch(static_cast<size_t>(maxc) + 2, 0);
            std::vector<int> touched; touched.reserve(256);
            for (size_t sid = 0; sid < S; ++sid) {
                if (!eager[sid]) continue;
                int top = std::min<int>(static_cast<int>(wpr[sid] - wpl[sid] + 1), static_cast<int>(maxc));
                for (uint32_t p = wpl[sid]; p <= wpr[sid]; ++p) {
                    uint32_t cv = c[graph.csr.incidences[p].edge];
                    int v = static_cast<int>(std::min<uint32_t>(cv, static_cast<uint32_t>(top)));
                    if (scratch[nonnegative_index(v)]++ == 0) touched.push_back(v);
                }
                int acc = 0, h = 0;
                for (int v = top; v >= 1; --v) {
                    acc += scratch[nonnegative_index(v)];
                    if (acc >= v) { h = v; break; }
                }
                eager_cap[sid] = h;
                for (int v : touched) scratch[nonnegative_index(v)] = 0;
                touched.clear();
            }
            eager_histoff.assign(S + 1, 0);
            for (size_t sid = 0; sid < S; ++sid)
                eager_histoff[sid + 1] = eager_histoff[sid] + (eager[sid] ? static_cast<size_t>(eager_cap[sid]) + 1 : 0);
            eager_hist.assign(eager_histoff[S], 0);
            eager_H.assign(S, 0);
            eager_Sgeq.assign(S, 0);
            for (size_t sid = 0; sid < S; ++sid) {
                if (!eager[sid]) continue;
                for (uint32_t p = wpl[sid]; p <= wpr[sid]; ++p) {
                    uint32_t cv = c[graph.csr.incidences[p].edge];
                    ++eager_hist[eager_histoff[sid] + static_cast<size_t>(eager_capval(static_cast<uint32_t>(sid), cv))];
                }
                eager_H[sid] = static_cast<uint32_t>(eager_cap[sid]);
                eager_Sgeq[sid] = eager_hist[eager_histoff[sid] + static_cast<size_t>(eager_cap[sid])];
                Hs[sid] = eager_H[sid];
            }
            eager_inq.assign(m, 0);
        }

        // ---- P3: lazy arming — sentinel certificates only ----
        // No side gets its full certificate up front.  Each side with h >= 1
        // (a) starts with drop budget 0, so the FIRST relevant neighbor drop wakes it
        //     (direct counter in --relax, budget-forest underflow otherwise), and
        // (b) gets a raw first-departure timer at the window-extreme gap, an O(1) upper
        //     bound on the first relevant (level-h) departure gap — never late.
        // The full certificate is built only on first wake (verify_side -> rearm_role);
        // a side that never receives an event is never armed.  Budget forests are
        // bulk-built in O(size) per slot: sides arrive in increasing position order and
        // each slot is therefore bulk-built as a perfectly balanced AVL tree.
        indexed_timer = !hybrid_active;
        if (indexed_timer) {
            iq.init(S, Dmax);
        } else {
            epoch.assign(S, 0);
            side_bytes += sizeof(uint32_t);
            rh.init(Dmax);
        }
        alloc_log("side_state_total", S, side_bytes);
        if (relaxed) {
            for (size_t sid = 0; sid < S; ++sid) {
                int h = static_cast<int>(Hs[sid]);
                if (h == 0) continue;
                const Edge& ed = graph.edges[sid >> 1];
                Time g = std::max(ed.t - graph.csr.incidences[wpl[sid]].t,
                                  graph.csr.incidences[wpr[sid]].t - ed.t);
                if (g >= 1) {
                    if (indexed_timer) iq.schedule(static_cast<uint32_t>(sid), g);
                    else rh.push(g, static_cast<uint32_t>(sid), epoch[sid]);
                }
            }
            std::vector<uint32_t>().swap(wpl);
            std::vector<uint32_t>().swap(wpr);
        } else {
            std::vector<size_t> scnt(nlvls + 1, 0);
            for (size_t sid = 0; sid < S; ++sid) {
                if (eager_side(static_cast<uint32_t>(sid))) continue;
                int h = static_cast<int>(Hs[sid]);
                if (h == 0) continue;
                ++scnt[dir_slot(side_node(static_cast<uint32_t>(sid)), h) + 1];
                const Edge& ed = graph.edges[sid >> 1];
                Time g = std::max(ed.t - graph.csr.incidences[wpl[sid]].t,
                                  graph.csr.incidences[wpr[sid]].t - ed.t);
                if (g >= 1) {
                    if (indexed_timer) iq.schedule(static_cast<uint32_t>(sid), g);
                    else rh.push(g, static_cast<uint32_t>(sid), epoch[sid]);
                }
            }
            for (size_t i = 1; i <= nlvls; ++i) scnt[i] += scnt[i - 1];
            alloc_log("ssort", scnt[nlvls], sizeof(uint32_t));
            std::vector<uint32_t> ssort(scnt[nlvls]);
            {
                std::vector<size_t> cur(scnt.begin(), scnt.end() - 1);
                for (size_t sid = 0; sid < S; ++sid) {
                    if (eager_side(static_cast<uint32_t>(sid))) continue;
                    int h = static_cast<int>(Hs[sid]);
                    if (h == 0) continue;
                    ssort[cur[dir_slot(side_node(static_cast<uint32_t>(sid)), h)]++] =
                        static_cast<uint32_t>(sid);
                }
            }
            for (size_t slot = 0; slot < nlvls; ++slot) {
                size_t sb = scnt[slot], se = scnt[slot + 1];
                if (sb == se) continue;
#ifndef NDEBUG
                for (size_t i = sb + 1; i < se; ++i) {
                    if (side_pos[ssort[i - 1]] >= side_pos[ssort[i]])
                        throw std::logic_error("AVL bulk input is not key-sorted");
                }
#endif
                troot[slot] = t_build_balanced(ssort, sb, se);
                t_debug_validate_slot(slot);
            }
            if (hybrid_active) {
                // One directed strict departure per eager-side membership.  The shared
                // queue also carries lazy certificate timers, so a radius sees both
                // engines before its fixed point is emitted.
                for (size_t sid = 0; sid < S; ++sid) {
                    if (!eager[sid]) continue;
                    const Edge& ed = graph.edges[sid >> 1];
                    for (uint32_t p = wpl[sid]; p <= wpr[sid]; ++p) {
                        Time g = std::llabs(graph.csr.incidences[p].t - ed.t);
                        if (g >= 1 && g > floorv)
                            rh.push_eager(g, static_cast<uint32_t>(sid), p);
                    }
                }
            }
            std::vector<uint32_t>().swap(wpl);
            std::vector<uint32_t>().swap(wpr);
        }

        init_s = t_init.stop();

        // ---- the sweep: lazy certificate wakes, plus eager exact departures ----
        Timer t_loop; t_loop.start();
        std::vector<BucketQueueMax::Entry> batch;
        std::vector<uint32_t> indexed_batch;
        if (indexed_timer) {
            while (!iq.empty()) {
                Time g = iq.pop_batch(indexed_batch);
                if (g <= floorv) break;
                Time delta = g - 1;
                if (g_census) ++cs_batch;
                for (uint32_t sid : indexed_batch) {
                    // A prior callback in this same gap batch can re-arm or cancel
                    // the side.  Its state then ceases to be POPPED, so this old
                    // local item is invalid without an epoch comparison.
                    if (!iq.consume(sid)) { ++st.stale_pops; continue; }
                    ++st.timer_wakes;
                    if (in_wake[sid]) { tfired[sid] = 1; continue; }
                    {
                        int php = ph_enter(PH_DEP);
                        verify_side(sid, delta, true);
                        ph_exit(php);
                    }
                    while (!wakeq.empty()) {
                        uint32_t s2 = wakeq.back(); wakeq.pop_back();
                        in_wake[s2] = 0;
                        bool tf = tfired[s2] != 0; tfired[s2] = 0;
                        int php = ph_enter(PH_CASC);
                        verify_side(s2, delta, tf);
                        ph_exit(php);
                    }
                }
                if (sink) sink->flush_radius(delta, c);
            }
        } else {
            while (!rh.empty()) {
                Time g = rh.pop_batch(batch);
                if (g <= floorv) break;
                Time delta = g - 1;
                if (g_census) ++cs_batch;
                // Exact departures commute within a radius.  Apply them all before
                // lazy timer wakes, matching the strict sweep's boundary discipline.
                for (const BucketQueueMax::Entry& en : batch)
                    if (en.eager) eager_departure(en.pi, en.pj);
                drain_hybrid(delta);
                for (const BucketQueueMax::Entry& en : batch) {
                    if (en.eager) continue;
                    uint32_t sid = en.pi;
                    if (en.pj != epoch[sid]) { ++st.stale_pops; continue; }
                    ++st.timer_wakes;
                    if (in_wake[sid]) { tfired[sid] = 1; continue; }
                    {
                        int php = ph_enter(PH_DEP);
                        verify_side(sid, delta, true);
                        ph_exit(php);
                    }
                    drain_hybrid(delta);
                }
                if (sink) sink->flush_radius(delta, c);
            }
        }
        loop_s = t_loop.stop();
    }

    // ---- KCS_CENSUS report: one row per non-empty xi bucket.  Bucket recompute /
    // false-alarm columns sum exactly to the kcs: recomputes= / false_alarms= totals
    // (every verify_side entry is binned; the h==0 early return and real alarms are
    // counted but not false).  wake_cycles includes the nested do_drop + re-arm. ----
    void report_census() const {
        long long tot_cnt = 0, tot_false = 0;
        unsigned long long tot_cyc = 0;
        for (int b = 0; b < CENSUS_B; ++b) {
            tot_cnt += cs_cnt[b]; tot_false += cs_false[b]; tot_cyc += cs_cyc[b];
        }
        std::cout << "CENSUS bucket | xi_range | recomputes | false_alarms | false% | wake_cycles | cyc%\n";
        for (int b = 0; b < CENSUS_B; ++b) {
            if (cs_cnt[b] == 0) continue;
            unsigned long long xlo = 1ull << b, xhi = (1ull << (b + 1)) - 1;
            double fp = 100.0 * static_cast<double>(cs_false[b]) / static_cast<double>(cs_cnt[b]);
            double cp = tot_cyc ? 100.0 * static_cast<double>(cs_cyc[b]) / static_cast<double>(tot_cyc) : 0.0;
            std::cout << "CENSUS " << b << " | [" << xlo << "," << xhi << "] | " << cs_cnt[b]
                      << " | " << cs_false[b] << " | " << fp << " | " << cs_cyc[b]
                      << " | " << cp << "\n";
        }
        double rp = tot_cnt ? 100.0 * static_cast<double>(cs_repeats) / static_cast<double>(tot_cnt) : 0.0;
        std::cout << "CENSUS totals recomputes=" << tot_cnt << " false_alarms=" << tot_false
                  << " wake_cycles=" << tot_cyc << "\n";
        std::cout << "CENSUS same_radius_repeats=" << cs_repeats << " (" << rp
                  << "% of recomputes)\n";
    }

    // ---- KCS_FADIAG report: all rows are false alarms from the strict sweep. ----
    void report_fadiag() const {
        long long total = fd->trigger_timer + fd->trigger_budget;
        std::cout << "FADIAG triggers timer=" << fd->trigger_timer
                  << " budget=" << fd->trigger_budget << " total=" << total << "\n";
        std::cout << "FADIAG timer_unspent tbud=0 count=" << fd->timer_unspent_zero;
        if (fd->timer_unspent_underflow)
            std::cout << " underflow=" << fd->timer_unspent_underflow;
        std::cout << "\n";
        for (int b = 0; b < 32; ++b) {
            if (!fd->timer_unspent_log2[b]) continue;
            unsigned long long lo = 1ull << b;
            unsigned long long hi = (1ull << (b + 1)) - 1;
            std::cout << "FADIAG timer_unspent log2=" << b << " range=[" << lo << "," << hi
                      << "] count=" << fd->timer_unspent_log2[b] << "\n";
        }
        double no_drop_pct = fd->trigger_timer
            ? 100.0 * static_cast<double>(fd->timer_no_drops) / static_cast<double>(fd->trigger_timer) : 0.0;
        std::cout << "FADIAG timer_zero_drops=" << fd->timer_no_drops << "/" << fd->trigger_timer
                  << " (" << no_drop_pct << "%)\n";
        for (const auto& it : fd->fresh_slack)
            std::cout << "FADIAG fresh_slack r=" << it.first << " count=" << it.second << "\n";
        std::cout << "FADIAG ssplit";
        for (int s = 0; s <= 8; ++s) std::cout << " s=" << s << ":" << fd->ssplit[s];
        std::cout << "\n";
    }

    // ---- peak-RSS breakdown (analytic; printed in --stream mode) ----
    // Answers WHICH structure dominates when streaming removes the O(B) output:
    // occ_* is O(C_0) = sum_e c_inf(e) per incidence level.  The indexed default
    // timer stores one live record per side; the hybrid legacy queue may contain
    // stale certificate records and exact eager departures.
    void report_mem() const {
        auto mb = [](size_t bytes) { return static_cast<double>(bytes) / (1 << 20); };
        size_t csr_b = graph.csr.incidences.capacity() * sizeof(Incidence) +
                       graph.csr.offsets.capacity() * sizeof(size_t) +
                       graph.csr.bit.capacity() * sizeof(int);
        size_t edges_b = graph.edges.capacity() * sizeof(Edge) +
                         graph.incidence_pos.capacity() * sizeof(uint32_t);
        size_t occ_b = occ_pos.capacity() * sizeof(uint32_t) +
                       occ_bit.capacity() * sizeof(int32_t);
        size_t dir_b = (blk_off.capacity() + lvl_off.capacity()) * sizeof(size_t) +
                       KO.capacity() * sizeof(int) + troot.capacity() * sizeof(int32_t);
        size_t side_b = tn.capacity() * sizeof(TN) +
                        (Hs.capacity() + epoch.capacity()) * sizeof(uint32_t) +
                        (sa.capacity() + armed_b.capacity() + timer_split.capacity() + rbud.capacity()) *
                            sizeof(int32_t) +
                        ssplit.capacity() + tfired.capacity() +
                        in_tree.capacity() + in_wake.capacity();
        size_t q_peak_b = indexed_timer
            ? (iq.next.capacity() + iq.prev.capacity()) * sizeof(uint32_t) +
              iq.key.capacity() * sizeof(Time) +
              (iq.bucket.capacity() + iq.state.capacity()) * sizeof(uint8_t) + sizeof(iq.head) +
              iq.batch_capacity_peak * sizeof(uint32_t)
            : rh.capacity_peak_entries * sizeof(BucketQueueMax::Entry);
        // Hybrid total = peak vector payload above + the fixed 65 vector objects.
        size_t q_dir_b = indexed_timer ? 0 : sizeof(rh.buckets);
        size_t c_b = c.capacity() * 4;
        std::cout << "MEMBK m=" << m << " n=" << n
                  << " occ_entries=" << occ_pos.size() << " nlvls=" << (blk_off.size() ? blk_off.size() - 1 : 0)
                  << " queue_peak_entries=" << (indexed_timer ? iq.peak : rh.peak)
                  << " queue_batch_peak=" << (indexed_timer ? iq.batch_peak : rh.batch_peak) << "\n";
        std::cout << "MEMBK csr=" << mb(csr_b) << "MB edges+pos=" << mb(edges_b)
                  << "MB c=" << mb(c_b) << "MB occ=" << mb(occ_b)
                  << "MB lvl_dir=" << mb(dir_b) << "MB side_state=" << mb(side_b)
                  << "MB timer_queue_peak=" << mb(q_peak_b) << "MB queue_dir=" << mb(q_dir_b)
                  << "MB sink=" << mb(sink ? sink->ram_bytes() : 0)
                  << "MB drops=" << mb(drops.allocated_bytes()) << "MB\n";
    }
};

template <class Fn>
void for_each_unique_drop(size_t edge, const DropLog& drops, Fn&& fn) {
    size_t at = drops.first(edge), end = drops.sentinel();
    Time previous = std::numeric_limits<Time>::min();
    while (at != end) {
        Time delta = drops[at].delta;
        if (delta < previous) throw std::logic_error("per-edge breakpoint chain is not ascending");
        uint32_t value = 0;
        do {
            value = std::max(value, drops[at].value);
            at = drops.following(at);
        } while (at != end && drops[at].delta == delta);
        fn(delta, value);
        previous = delta;
    }
}

class AtomicTextOutput {
public:
    AtomicTextOutput(const std::string& final_path, const std::string& input_path)
        : final_path_(normalized_path(final_path).string()) {
        if (paths_alias(input_path, final_path_))
            throw std::runtime_error("refusing to overwrite input through output alias '" +
                                     final_path_ + "'");
        for (unsigned attempt = 0; attempt < 1000; ++attempt) {
            temp_path_ = final_path_ + ".tmp." + std::to_string(static_cast<long long>(::getpid())) +
                         "." + std::to_string(attempt);
            int fd = ::open(temp_path_.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0666);
            if (fd >= 0) {
                file_ = ::fdopen(fd, "w");
                if (file_) return;
                int saved = errno;
                ::close(fd);
                ::unlink(temp_path_.c_str());
                throw std::runtime_error("fdopen failed: " + std::string(std::strerror(saved)));
            }
            if (errno != EEXIST)
                throw std::runtime_error("cannot create temporary index: " +
                                         std::string(std::strerror(errno)));
        }
        throw std::runtime_error("cannot allocate a unique temporary index path");
    }
    AtomicTextOutput(const AtomicTextOutput&) = delete;
    AtomicTextOutput& operator=(const AtomicTextOutput&) = delete;
    ~AtomicTextOutput() {
        if (file_) std::fclose(file_);
        if (!committed_ && !temp_path_.empty()) ::unlink(temp_path_.c_str());
    }
    void print(const char* format, ...) {
        va_list args;
        va_start(args, format);
        int rc = std::vfprintf(file_, format, args);
        va_end(args);
        if (rc < 0) throw std::runtime_error("index write failed: " +
                                             std::string(std::strerror(errno)));
    }
    void commit() {
        if (std::fflush(file_) != 0 || ::fsync(::fileno(file_)) != 0)
            throw std::runtime_error("index flush/fsync failed: " + std::string(std::strerror(errno)));
        if (std::fclose(file_) != 0) {
            file_ = nullptr;
            throw std::runtime_error("index close failed: " + std::string(std::strerror(errno)));
        }
        file_ = nullptr;
#if defined(KCS_TEST_FAIL_BEFORE_RENAME)
        throw std::runtime_error("injected failure after fsync and before rename");
#endif
        if (::rename(temp_path_.c_str(), final_path_.c_str()) != 0)
            throw std::runtime_error("atomic index rename failed: " +
                                     std::string(std::strerror(errno)));
        committed_ = true;
        sync_parent_directory(final_path_);
    }
private:
    std::string final_path_, temp_path_;
    std::FILE* file_ = nullptr;
    bool committed_ = false;
};

void write_index(const std::string& dataset, const Graph& graph, const std::vector<uint32_t>& c,
                 const DropLog& drops, Time floorv, Time Dmax) {
    size_t m = graph.edges.size();
    std::string idxpath = dataset + ".kcs_index";
    AtomicTextOutput idx(idxpath, dataset);
    idx.print("# (k,Delta)-core spectrum index  |  Delta in [%lld, %lld]\n",
              static_cast<long long>(floorv), static_cast<long long>(Dmax));
    idx.print("# format:  u v t | base=<core at Delta=floor> ; <Delta_up>:<core> ...\n");
    uint64_t total_bp = 0;
    for (size_t e = 0; e < m; ++e) {
        const Edge& ed = graph.edges[e];
        if (c[e] > m)
            throw std::logic_error("text-index base coreness exceeds edge-count bound");
        idx.print("%d %d %lld | base=%u ;", static_cast<int>(ed.u), static_cast<int>(ed.v),
                  static_cast<long long>(ed.t), c[e]);
        Time previous_delta = floorv;
        uint32_t previous_value = c[e];
        for_each_unique_drop(e, drops, [&](Time delta, uint32_t value) {
            if (delta <= previous_delta || delta > Dmax || value <= previous_value || value > m)
                throw std::logic_error("invalid monotone text-index staircase");
            idx.print(" %lld:%u", static_cast<long long>(delta), value);
            ++total_bp;
            previous_delta = delta;
            previous_value = value;
        });
        idx.print("\n");
    }
    idx.commit();
    std::cout << "INDEX " << idxpath << "  breakpoints=" << total_bp << "\n";
}

// ===== --recompute: NAIVE baseline — independent exact peeling FROM SCRATCH at
// every critical radius (no incremental carry-over between radii; that is the
// point).  The spectrum is piecewise-constant in Delta: an edge's Delta-degree at
// node w changes only when some incidence enters/leaves a window, i.e. at
// Delta = |t_f - t_e| for two incidences at the same node — exactly the gap set
// engine_current streams.  So peel at Delta = floor and at every distinct gap in
// (floor, Dmax], ascending; a breakpoint (g, core_at_g) is emitted wherever two
// consecutive plateaus differ, which reproduces the sweep's staircase bit-exactly
// (the sweep records (delta+1, value_above) = (g, coreness at Delta >= g) on each
// strict change, and write_index sorts/dedups per edge identically). =====
int recompute_baseline(const std::string& dataset, Graph& graph, Time Dmax, Time floorv, bool dodump) {
    size_t m = graph.edges.size();
    Timer ts; ts.start();

    // 1) critical radii = distinct same-node incidence gaps in (floor, Dmax]
    std::vector<Time> radii;
    auto compact = [&]() {
        std::sort(radii.begin(), radii.end());
        radii.erase(std::unique(radii.begin(), radii.end()), radii.end());
    };
    for (NodeId node = 0; node < graph.node_count; ++node) {
        size_t b = graph.csr.node_begin(node), e = graph.csr.node_end(node);
        for (size_t i = b; i < e; ++i) {
            for (size_t j = i + 1; j < e; ++j) {
                Time g = graph.csr.incidences[j].t - graph.csr.incidences[i].t;
                if (g > Dmax) break;                        // per-node times ascend
                if (g > floorv) radii.push_back(g);
            }
            if (radii.size() > (size_t(1) << 26)) compact(); // bound scratch; dedup is idempotent
        }
    }
    compact();

    // 2) independent from-scratch peel at floor and at every critical radius
    graph.csr.build_fenwick();                              // fresh oracle state each radius
    std::vector<uint32_t> base = compute_fast_kdelta_core(graph, floorv);
    DropLog drops(m, DropOrder::AscendingRadius);
    std::vector<uint32_t> prev = base;
    for (Time g : radii) {
        graph.csr.build_fenwick();
        std::vector<uint32_t> cur = compute_fast_kdelta_core(graph, g);
        for (size_t e = 0; e < m; ++e)
            if (cur[e] != prev[e]) drops.emplace_back(static_cast<EdgeId>(e), g, cur[e]);
        prev.swap(cur);
    }
    double sweep_s = ts.stop();
    std::cout << "recompute: peels=" << (radii.size() + 1)
              << "  breakpoints=" << drops.size()
              << "  sweep=" << sweep_s << "s\n";
    if (dodump) write_index(dataset, graph, base, drops, floorv, Dmax);
    return 0;
}

// wrong-Delta assertion: query the index at sampled Delta values and cross-check
// every edge against a fresh exact peeling.  A breakpoint emitted at a wrong Delta
// would make some sampled query disagree with the oracle.
int qcheck(Graph& graph, const std::vector<uint32_t>& base_c, const DropLog& drops,
           Time floorv, Time Dmax) {
    size_t m = graph.edges.size();
    auto query = [&](size_t e, Time Q) -> uint32_t {
        uint32_t val = base_c[e];
        for_each_unique_drop(e, drops, [&](Time delta, uint32_t value) {
            if (delta <= Q) val = value;
        });
        return val;
    };
    int fails = 0;
    constexpr uint32_t numerators[5] = {1, 1, 1, 3, 1};
    constexpr uint32_t denominators[5] = {10, 4, 2, 4, 1};
    for (size_t sample = 0; sample < 5; ++sample) {
        const Time den = static_cast<Time>(denominators[sample]);
        const Time num = static_cast<Time>(numerators[sample]);
        // Quotient/remainder scaling is exact integer floor arithmetic and cannot
        // overflow because num <= den.  A double product loses units near INT64_MAX.
        Time Q = (Dmax / den) * num + ((Dmax % den) * num) / den;
        if (Q < floorv) Q = floorv;
        graph.csr.build_fenwick();
        std::vector<uint32_t> orc = compute_fast_kdelta_core(graph, Q);
        size_t bad = 0;
        for (size_t e = 0; e < m; ++e) if (query(e, Q) != orc[e]) ++bad;
        std::cout << "qcheck Delta=" << Q << ": mism=" << bad << "/" << m << (bad ? "  FAIL" : "  OK") << "\n";
        if (bad) ++fails;
    }
    return fails;
}

int run_cli(int argc, char** argv) {
    if (argc < 2) { std::cerr << "usage: kcs dataset [Dmax] [floor] [--dump] [--stream <KCSSTRM2>] [--nofilter|--hist|--relax|--recompute] [--seedcheck] [--qcheck] [--novalidate]\n"; return 1; }
    std::string dataset = argv[1];
    bool dodump = false, use_nofilter = false, use_hist = false, use_relax = false, use_recompute = false;
    bool seedcheck = false, novalidate = false, do_qcheck = false;
    std::string stream_path;
    std::vector<std::string> pos;
    for (int i = 2; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--dump") dodump = true;
        else if (a == "--stream") {
            if (i + 1 >= argc) { std::cerr << "--stream needs an output file\n"; return 1; }
            if (!stream_path.empty()) { std::cerr << "--stream may be specified only once\n"; return 1; }
            stream_path = argv[++i];
        }
        else if (a == "--reconstruct") {
            std::cerr << "--reconstruct was replaced by: stream_tool finalize <KCSSTRM2> <KCSIDX3>\n";
            return 1;
        }
        else if (a == "--nofilter") use_nofilter = true;
        else if (a == "--hist") use_hist = true;
        else if (a == "--relax") use_relax = true;
        else if (a == "--recompute") use_recompute = true;
        else if (a == "--seedcheck") seedcheck = true;
        else if (a == "--novalidate") novalidate = true;
        else if (a == "--qcheck") { do_qcheck = true; dodump = true; }
        else if (a.rfind("--", 0) == 0) {
            std::cerr << "unknown option: " << a << '\n';
            return 1;
        } else pos.push_back(a);
    }
    if ((int)use_nofilter + (int)use_hist + (int)use_relax + (int)use_recompute > 1) {
        std::cerr << "--nofilter, --hist, --relax and --recompute are mutually exclusive\n";
        return 1;
    }
    if (pos.size() > 2) {
        std::cerr << "too many positional arguments; expected [Dmax] [floor]\n";
        return 1;
    }
    if (use_recompute && (!stream_path.empty() || seedcheck || do_qcheck)) {
        std::cerr << "--recompute cannot be combined with --stream, --seedcheck, or --qcheck\n";
        return 1;
    }

    Timer tl; tl.start();
    Graph graph;
    try {
        graph = load_graph(dataset);
    } catch (const std::exception& ex) {
        std::cerr << "input error: " << ex.what() << "\n";
        return 1;
    }
    double load_s = tl.stop();
    size_t m = graph.edges.size();
    if (m == 0) { std::cerr << "empty graph\n"; return 1; }
    Time Dmax = 0, floorv = 0;
    if ((pos.size() >= 1 && !parse_time_arg(pos[0], Dmax)) ||
        (pos.size() >= 2 && !parse_time_arg(pos[1], floorv))) {
        std::cerr << "Dmax and floor must be signed 64-bit integers\n";
        return 1;
    }
    if (pos.empty()) {
        // The loader sorts edges by non-negative timestamp.  Their global span
        // dominates every same-node critical gap and therefore covers the complete
        // spectrum; it can legitimately be zero for an all-simultaneous graph.
        Dmax = graph.edges.back().t - graph.edges.front().t;
    }
    if (Dmax < 0) { std::cerr << "Dmax must be non-negative\n"; return 1; }
    if (floorv < 0 || floorv > Dmax) {
        std::cerr << "floor must satisfy 0 <= floor <= Dmax\n";
        return 1;
    }
    const char* mode_name = use_recompute ? "recompute(naive)" :
                            (use_nofilter ? "nofilter(direct-sweep)" :
                            (use_hist ? "hist(stage0)" :
                            (use_relax ? "relax(sqrt-drop-margin)" : "kcs(certificates)")));
    std::cout << "#edges: " << m << "  Dmax: " << Dmax << "  floor: " << floorv
              << "  mode: " << mode_name << "\n";
    if (use_recompute) return recompute_baseline(dataset, graph, Dmax, floorv, dodump);

    Timer total; total.start();

    // ---- seed: c_inf ----
    Timer tb; tb.start();
    Time dcore_star = 0;
    for (NodeId x = 0; x < graph.node_count; ++x) {
        int len = graph.csr.node_size(x);
        if (len >= 2) dcore_star = std::max(dcore_star, graph.csr.time_at(x, len - 1) - graph.csr.time_at(x, 0));
    }
    std::vector<uint32_t> c;
    bool used_static = Dmax >= dcore_star;
    if (used_static) {
        c = static_core_seed(graph);          // Lemma 1.1: O(m+n), no window scans
    } else {
        graph.csr.build_fenwick();
        c = compute_fast_kdelta_core(graph, Dmax);
        std::vector<int>().swap(graph.csr.bit);
    }
    double base_s = tb.stop();
    std::cout << "seed: " << (used_static ? "static-core (Lemma 1.1)" : "temporal-peel")
              << "  Delta_core*=" << dcore_star << "  (" << base_s << "s)\n";

    if (seedcheck) {
        graph.csr.build_fenwick();
        std::vector<uint32_t> ref = compute_fast_kdelta_core(graph, Dmax);
        std::vector<int>().swap(graph.csr.bit);
        size_t bad = 0;
        for (size_t e = 0; e < m; ++e) if (c[e] != ref[e]) ++bad;
        std::cout << "seedcheck vs temporal peel at Dmax: mism=" << bad << "/" << m << (bad ? "  FAIL" : "  OK") << "\n";
        if (bad) return 4;
    }

    // --stream: open the sink on the seed vector BEFORE the sweep mutates c.
    StreamSink sink;
    StreamSink* sinkp = nullptr;
    if (!stream_path.empty()) {
        if (dodump && paths_alias(stream_path, dataset + ".kcs_index")) {
            std::cerr << "--stream output must not alias the --dump index path\n";
            return 1;
        }
        sink.open(stream_path, dataset, graph, c, Dmax, floorv);
        sinkp = &sink;
    }

    DropLog drops(m, DropOrder::DescendingRadius);
    Timer ts; ts.start();
    // Degenerate hybrid: when every side is eager, call the existing strict engine
    // directly.  Besides being the requested all-eager boundary, this retains the
    // strict engine's serialized staircase exactly.  Keep KCS_CENSUS on the Kcs path
    // so its (zero-wake) accounting remains available for all-eager census runs.
    bool hybrid_all_eager = false;
    if (!g_census && !g_fadiag && !use_nofilter && !use_hist && !use_relax) {
        uint32_t limit = eager_xi_limit();
        if (limit) {
            hybrid_all_eager = true;
            for (size_t e = 0; e < m && hybrid_all_eager; ++e) {
                const Edge& ed = graph.edges[e];
                for (int sd = 0; sd < 2; ++sd) {
                    NodeId x = sd == 0 ? ed.u : ed.v;
                    size_t base = graph.csr.node_begin(x);
                    uint32_t pl = static_cast<uint32_t>(base) + static_cast<uint32_t>(graph.csr.lower_time(x, lower_window(ed.t, Dmax)));
                    uint32_t pr = static_cast<uint32_t>(base) +
                                  static_cast<uint32_t>(graph.csr.upper_time(x, upper_window(ed.t, Dmax))) - 1;
                    uint32_t xi = (pr - pl) + 1u;
                    if (xi > limit) { hybrid_all_eager = false; break; }
                }
            }
        }
    }
    if (use_nofilter || use_hist || hybrid_all_eager) {
        HistStats st = hist_sweep(graph, c, Dmax, floorv, dodump, drops, sinkp, !use_nofilter);
        double sweep_s = ts.stop();
        const char* statp = use_nofilter ? "nofilter" : (hybrid_all_eager ? "hybrid-hist" : "hist");
        std::cout << statp << ": removals=" << st.removals << "  reevals=" << st.reevals
                  << "  real_reevals=" << st.real_reevals << "  false_reevals=" << st.false_reevals
                  << "  member_updates=" << st.member_updates << "  events=" << st.ev_count
                  << "  breakpoints=" << st.breakpoints << "  sweep=" << sweep_s << "s\n";
    } else {
        Kcs kcs(graph, c, Dmax, floorv, dodump, drops, use_relax);
        kcs.sink = sinkp;
        kcs.run();
        double sweep_s = ts.stop();
        const KcsStats& st = kcs.st;
        const char* statp = use_relax ? "relax" : "kcs";
        std::cout << statp << ": arms=" << st.arms << "  recomputes=" << st.recomputes
                  << "  timer_wakes=" << st.timer_wakes
                  << "  stale_pops=" << st.stale_pops << "  budget_wakes=" << st.budget_wakes << "\n";
        std::cout << statp << ": false_alarms=" << st.false_alarms << "  real_alarms=" << st.real_alarms
                  << "  h_drop_units=" << st.h_drop_units << "  breakpoints=" << st.breakpoints << "\n";
        std::cout << statp << ": occ_counts=" << st.occ_counts << "  occ_deletes=" << st.occ_deletes
                  << "  range_adds=" << st.range_adds << "  timer_probes=" << st.timer_probes
                  << "  cheap_rearms=" << st.cheap_rearms << "\n";
        if (use_relax)
            std::cout << "relax: drop_checks=" << st.relax_drop_checks
                      << "  drop_hits=" << st.relax_drop_hits << "\n";
        std::cout << statp << ": WORK=" << st.work() << "  sweep=" << sweep_s << "s\n";
        // P5: stable parseable line in both builds.  Fine-grained fields are explicit
        // NA in release instead of silently disappearing or reporting misleading zeroes.
        std::cout << "PHASE seed=" << base_s << " init=" << kcs.init_s
#if defined(KCS_PROFILE)
                  << " departure=" << kcs.phw[Kcs::PH_DEP] << " drop=" << kcs.phw[Kcs::PH_DROP]
                  << " cascade=" << kcs.phw[Kcs::PH_CASC] << " arm=" << kcs.phw[Kcs::PH_ARM]
#else
                  << " departure=NA drop=NA cascade=NA arm=NA"
#endif
                  << " sweep=" << kcs.loop_s
                  << " total=" << (base_s + kcs.init_s + kcs.loop_s)
#if defined(KCS_PROFILE)
                  << " profile=enabled\n";
#else
                  << " profile=disabled (compile with -DKCS_PROFILE)\n";
#endif
        if (g_census) kcs.report_census();
        if (g_fadiag) kcs.report_fadiag();
        if (sinkp) kcs.report_mem();
    }
    double total_s = total.stop();

    // ---- validate final c against exact peeling at Delta = floor ----
    if (!novalidate) {
        graph.csr.build_fenwick();
        std::vector<uint32_t> oracle = compute_fast_kdelta_core(graph, floorv);
        size_t mism = 0; long long fb = -1;
        for (size_t e = 0; e < m; ++e) if (c[e] != oracle[e]) { if (fb < 0) fb = (long long)e; ++mism; }
        std::cout << "validate at Delta=floor=" << floorv << ": mism=" << mism << "/" << m;
        if (mism) { size_t e = (size_t)fb; std::cout << " (edge " << e << " got " << c[e] << " oracle " << oracle[e] << ")\n"; return 2; }
        std::cout << "  EXACT\n";
    }
    std::cout << "total = " << total_s << "s  (load " << load_s << "s)\n";

    if (do_qcheck && qcheck(graph, c, drops, floorv, Dmax) != 0) return 5;
    if (dodump) write_index(dataset, graph, c, drops, floorv, Dmax);
    if (sinkp) {
        sink.finish();
        std::cout << "STREAM KCSSTRM2 " << stream_path << "  groups=" << sink.groups()
                  << "  records=" << sink.records() << "  bytes=" << sink.file_bytes()
                  << "  spool_peak=" << sink.spool_peak() << "\n";
    }
    return 0;
}

// The stdout lines (kcs:/PHASE/INDEX/STREAM/validate/...) ARE the product of a
// measurement run.  When stdout is redirected to a log on a full disk, stdio
// only reports the lost writes through the sticky stream error state; without
// this check the process exits 0 with a silently truncated log and downstream
// experiment scripts trust an incomplete run.  Success-path bytes are unchanged.
static int finish_with_stdout_check(int rc) {
    std::cout.flush();
    const bool stdout_failed = !std::cout || std::ferror(stdout) != 0;
    if (stdout_failed && rc == 0) {
        std::cerr << "fatal: writing results to stdout failed; the run log is incomplete\n";
        return 1;
    }
    return rc;
}

int main(int argc, char** argv) {
    try {
        return finish_with_stdout_check(run_cli(argc, argv));
    } catch (const std::bad_alloc&) {
        std::cerr << "fatal: insufficient memory\n";
    } catch (const std::exception& ex) {
        std::cerr << "fatal: " << ex.what() << '\n';
    } catch (...) {
        std::cerr << "fatal: unknown exception\n";
    }
    return 1;
}
