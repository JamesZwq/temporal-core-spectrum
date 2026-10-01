// qtime.cpp -- query timing to the researcher's two-class protocol.
//
// WHY THIS EXISTS.  query.cpp has NO internal timer (it includes <chrono> and never uses it),
// so every query number taken with it was whole-process wall clock, including: process start,
// BinaryIndex construction (open + fstat + mmap + header decode + footer validate), demand
// paging of the mapping on FIRST TOUCH, and writing one line per edge to stdout.  On the
// YouTube sweep that produced 276.626 s and 25.887 s at two interior radii while every
// neighbour read 2.2-2.5 s -- a cold mmap of a 1.8 GB index after the page cache was evicted.
// None of that is query work.
//
// SYMMETRY.  EdgeCD's `Elapsed:` wraps compute_kdelta_core alone: it excludes its own graph
// load and excludes writing its output file.  So we exclude index load and (for the headline)
// output emission too.  Both sides then report algorithm time only, by a stated protocol.
//
// CLASS A -- point queries, AMORTIZED.  Microsecond-scale, so a single one cannot be timed.
//   1. open index                      UNTIMED
//   2. warmup batch of N queries       UNTIMED, but its own time is reported as cold_batch_s
//   3. 3 timed batches of N            report min(batch)/N
//   N auto-scales from 10000 until a batch takes >= 1 s, so timer resolution never binds.
//   Sampling is uniform over edges AND over the Delta domain, from a recorded seed, so the
//   exact batch is regenerable.
//
// CLASS B -- whole-output ops, NOT amortized.  A full snapshot at one Delta is one large
//   operation whose cost is proportional to output; dividing it by anything is meaningless.
//   open untimed, explicit warm pass (reported separately), then best-of-3 of the operation.
//
// TIMER PLACEMENT (auditable): in class A the timer starts at `const auto a = clk::now();`
// inside run_batch(), AFTER BinaryIndex construction and AFTER the warmup batch.  In class B
// it starts at `const auto a = clk::now();` inside the rep loop, AFTER construction and AFTER
// the explicit warm pass.  No timer in this file ever spans the BinaryIndex constructor.
//
// usage: qtime <index.kcsidx3> full  <delta> [reps]
//        qtime <index.kcsidx3> batch [seed] [nbatches]
// build: g++ -std=c++17 -O3 -DNDEBUG -iquote . -o qtime qtime.cpp

#include "kcs_index_format.hpp"
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <random>
#include <string>
#include <vector>
#include <sys/resource.h>

using namespace kcsidx;
using clk = std::chrono::steady_clock;
static double secs(clk::time_point a, clk::time_point b) {
  return std::chrono::duration<double>(b - a).count();
}
static long majflt() { struct rusage ru{}; getrusage(RUSAGE_SELF, &ru); return ru.ru_majflt; }

int main(int argc, char **argv) {
  if (argc < 3) {
    std::fprintf(stderr, "usage: qtime <index.kcsidx3> full <delta> [reps]\n"
                         "       qtime <index.kcsidx3> batch [seed] [nbatches]\n");
    return 1;
  }
  const std::string path = argv[1], mode = argv[2];

  // ---------- index open: UNTIMED with respect to every reported query number ----------
  const long mf0 = majflt();
  const auto o0 = clk::now();
  BinaryIndex idx(path);
  const auto o1 = clk::now();
  const double open_s = secs(o0, o1);
  const auto &h = idx.header();
  const uint64_t E = h.edges;
  const int64_t lo = h.floor, hi = h.dmax;
  volatile uint64_t sink = 0;

  // 2026-09-30: pre-touch every page of the directory and record sections (untimed), so that point and onset
  // batches both start from a warm index and the batch size auto-scales the same way in both modes.
  auto touch_all = [&]() {
    const auto t0 = clk::now(); uint64_t acc = 0;
    for (uint64_t e = 0; e < E; e += 256) acc += idx.directory(e).base;   // 16-byte entries: one per 4 KB page
    if (E) acc += idx.directory(E - 1).base;
    const uint64_t R = h.records;
    for (uint64_t i = 0; i < R; i += 170) acc += idx.record(i).value;      // 24-byte records: one per 4 KB page
    if (R) acc += idx.record(R - 1).value;
    sink += acc;
    std::printf("TOUCH seconds=%.3f\n", secs(t0, clk::now()));
  };

  if (mode == "batch") {
    touch_all();
    // ================= CLASS A: point queries, amortized =================
    const uint64_t seed = argc > 3 ? strtoull(argv[3], nullptr, 10) : 20260818;
    const int nbatch = argc > 4 ? atoi(argv[4]) : 3;
    uint64_t N = 10000;

    auto make = [&](uint64_t n, uint64_t sd) {
      std::mt19937_64 rng(sd);
      std::uniform_int_distribution<uint64_t> de(0, E ? E - 1 : 0);
      std::uniform_int_distribution<int64_t> dd(lo, hi);
      std::vector<std::pair<uint64_t, int64_t>> v;
      v.reserve(n);
      for (uint64_t i = 0; i < n; ++i) v.emplace_back(de(rng), dd(rng));
      return v;
    };
    auto run_batch = [&](const std::vector<std::pair<uint64_t, int64_t>> &q) {
      const auto a = clk::now();                       // <-- CLASS A TIMER STARTS HERE
      uint64_t acc = 0;
      for (const auto &p : q) acc += idx.point(p.first, p.second);
      const auto b = clk::now();
      sink += acc;
      return secs(a, b);
    };

    // warmup batch: UNTIMED for the headline, but its cost is reported as cold_batch_s
    auto warm = make(N, seed ^ 0x9E3779B97F4A7C15ULL);
    const double cold_batch_s = run_batch(warm);
    const long mf_cold = majflt() - mf0;

    // auto-scale N so each timed batch takes >= 1 s (pilot, recorded)
    double pilot = run_batch(warm);
    while (pilot < 1.0 && N < (1ull << 31)) {
      N *= 4;
      warm = make(N, seed ^ 0x9E3779B97F4A7C15ULL);
      pilot = run_batch(warm);
    }

    auto q = make(N, seed);
    std::vector<double> bt;
    for (int b = 0; b < nbatch; ++b) {
      const double s = run_batch(q);
      bt.push_back(s);
      std::printf("QBATCH rep=%d n=%llu batch_s=%.6f per_query_us=%.6f major_faults=%ld\n",
                  b + 1, (unsigned long long)N, s, 1e6 * s / (double)N, majflt() - mf0);
    }
    std::sort(bt.begin(), bt.end());
    const double best = bt.front();
    std::printf("QTIMESUM class=A index=%s n_queries=%llu n_batches=%d warmup=yes "
                "cold_batch_s=%.6f cold_major_faults=%ld min_batch_s=%.6f per_query_us=%.6f "
                "open_s=%.6f rng_seed=%llu domain=[%lld,%lld] edges=%llu checksum=%u\n",
                path.c_str(), (unsigned long long)N, nbatch, cold_batch_s, mf_cold, best,
                1e6 * best / (double)N, open_s, (unsigned long long)seed,
                (long long)lo, (long long)hi, (unsigned long long)E, (uint32_t)sink);
    std::printf("QTIMECSV,A,point,NA,%llu,%d,yes,%.6f,%.6f,%.6f,%.6f,NA,%llu\n",
                (unsigned long long)N, nbatch, cold_batch_s, best,
                1e6 * best / (double)N, open_s, (unsigned long long)seed);
    return 0;
  }

  if (mode == "onset") {
    touch_all();
    // 2026-09-30: onset queries, amortized exactly as class A.  A query (e, k) with k drawn uniformly from
    // [1, the edge's final core number] (so the answer is finite; the +infinity case is one comparison).
    // The onset is the Delta of the first breakpoint whose core number reaches k (0 when the base reaches k):
    // a binary search over the edge's breakpoint list, which Definition 3.3 orders by Delta and by core number.
    const uint64_t seed = argc > 3 ? strtoull(argv[3], nullptr, 10) : 20260818;
    const int nbatch = argc > 4 ? atoi(argv[4]) : 3;
    uint64_t N = 10000;
    auto onset = [&](uint64_t e, uint32_t k) -> int64_t {   // same directory/record access as point()
      const auto d = idx.directory(e);
      if (d.base >= k) return 0;
      uint64_t lo = 0, hi = d.count;
      while (lo < hi) { const uint64_t mid = lo + (hi - lo) / 2; if (idx.record(d.first + mid).value < k) lo = mid + 1; else hi = mid; }
      return lo < d.count ? idx.record(d.first + lo).delta : INT64_MAX;
    };
    auto make = [&](uint64_t n, uint64_t sd) {
      std::mt19937_64 rng(sd);
      std::uniform_int_distribution<uint64_t> de(0, E ? E - 1 : 0);
      std::vector<std::pair<uint64_t, uint32_t>> v; v.reserve(n);
      while (v.size() < n) { const uint64_t e = de(rng); const auto d = idx.directory(e);
        const uint32_t top = d.count ? idx.record(d.first + d.count - 1).value : d.base;
        std::uniform_int_distribution<uint32_t> dk(1, top ? top : 1); v.emplace_back(e, dk(rng)); }
      return v;
    };
    auto run_batch = [&](const std::vector<std::pair<uint64_t, uint32_t>> &q) {
      const auto a = clk::now(); uint64_t acc = 0;
      for (const auto &p : q) acc += (uint64_t)onset(p.first, p.second);
      const auto b = clk::now(); sink += acc; return secs(a, b);
    };
    { // correctness check against point(): onset o satisfies point(e,o) >= k and point(e,o-1) < k
      auto chk = make(100000, seed + 7); uint64_t bad = 0;
      for (const auto &p : chk) { const int64_t o = onset(p.first, p.second);
        if (o == INT64_MAX) { if (idx.point(p.first, hi) >= p.second) ++bad; continue; }
        if (idx.point(p.first, o) < p.second) ++bad;
        if (o > lo && idx.point(p.first, o - 1) >= p.second) ++bad; }
      std::printf("ONSETCHECK queries=100000 bad=%llu\n", (unsigned long long)bad);
      if (bad) return 2; }
    auto warm = make(N, seed ^ 0x9E3779B97F4A7C15ULL);
    run_batch(warm); double pilot = run_batch(warm);
    while (pilot < 1.0 && N < (1ull << 31)) { N *= 4; warm = make(N, seed ^ 0x9E3779B97F4A7C15ULL); pilot = run_batch(warm); }
    auto q = make(N, seed); std::vector<double> bt;
    for (int b = 0; b < nbatch; ++b) bt.push_back(run_batch(q));
    std::sort(bt.begin(), bt.end());
    std::printf("QTIMECSV,A,onset,NA,%llu,%d,yes,NA,%.6f,%.6f,%.6f,NA,%llu\n",
                (unsigned long long)N, nbatch, bt.front(), 1e6 * bt.front() / (double)N, open_s, (unsigned long long)seed);
    std::printf("QTIMESUM class=A op=onset per_query_us=%.6f checksum=%u\n", 1e6 * bt.front() / (double)N, (uint32_t)sink);
    return 0;
  }

  if (mode != "full") { std::fprintf(stderr, "mode must be full or batch\n"); return 1; }

  // ================= CLASS B: whole-output snapshot, NOT amortized =================
  if (argc < 4) { std::fprintf(stderr, "full needs <delta>\n"); return 1; }
  const int64_t delta = strtoll(argv[3], nullptr, 10);
  const int reps = argc > 4 ? std::max(1, atoi(argv[4])) : 3;

  // explicit first-touch of the whole mapping, so no repetition pays demand paging.
  // Its cost is REPORTED, not hidden -- it is the cold-cache cost that produced the outliers.
  const auto w0 = clk::now();
  for (uint64_t i = 0; i < h.records; ++i) sink += (uint64_t)idx.record(i).value;
  for (uint64_t e = 0; e < E; ++e) sink += idx.directory(e).count;
  const auto w1 = clk::now();
  const double warm_s = secs(w0, w1);
  const long mf_warm = majflt() - mf0;

  std::vector<double> q;
  for (int r = 0; r < reps; ++r) {
    const long mfa = majflt();
    const auto a = clk::now();                        // <-- CLASS B TIMER STARTS HERE
    uint64_t acc = 0;
    for (uint64_t e = 0; e < E; ++e) acc += idx.point(e, delta);
    const auto b = clk::now();
    sink += acc;
    const double s = secs(a, b);
    q.push_back(s);
    std::printf("QREP rep=%d query_s=%.6f major_faults=%ld\n", r + 1, s, majflt() - mfa);
  }
  std::sort(q.begin(), q.end());
  const double best = q.front(), med = q[q.size() / 2];

  // with-output variant (formatting cost, to /dev/null so no disk is measured)
  double emit_s = -1.0;
  {
    FILE *dn = fopen("/dev/null", "w");
    if (dn) {
      static char buf[1 << 20];
      setvbuf(dn, buf, _IOFBF, sizeof buf);
      const auto a = clk::now();
      for (uint64_t e = 0; e < E; ++e)
        fprintf(dn, "%llu %u\n", (unsigned long long)e, idx.point(e, delta));
      fflush(dn);
      emit_s = secs(a, clk::now());
      fclose(dn);
    }
  }
  std::printf("QTIMESUM class=B index=%s delta=%lld edges=%llu reps=%d best_query_s=%.6f "
              "median_query_s=%.6f open_s=%.6f warm_s=%.6f warm_major_faults=%ld emit_s=%.6f "
              "ns_per_edge=%.3f checksum=%u\n",
              path.c_str(), (long long)delta, (unsigned long long)E, reps, best, med, open_s,
              warm_s, mf_warm, emit_s, E ? 1e9 * best / (double)E : 0.0, (uint32_t)sink);
  std::printf("QTIMECSV,B,full,%lld,%llu,%d,yes,%.6f,%.6f,%.6f,%.6f,%.6f,NA\n",
              (long long)delta, (unsigned long long)E, reps, warm_s, best,
              E ? 1e6 * best / (double)E : 0.0, open_s, emit_s);
  return 0;
}
