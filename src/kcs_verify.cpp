// kcs_verify.cpp -- independent differential check of a .kcsb against the .kcs_index TEXT
// file it was packed from.  Nothing here shares code with the packer except the format
// header; the oracle is a LINEAR scan of the text-parsed staircase, so it cannot inherit a
// bug from the reader's binary search.
//
//   1. structure    m, nbp, dfloor, dmax, kmax, maxnode, and for EVERY edge: u, v, t, base,
//                   the CSR bounds, and EVERY breakpoint (Delta_up, value).
//   2. deathlevel   for EVERY edge, against the text-derived value
//                   (last breakpoint's value, or base when the edge has none).
//   3. coreness     on a random edge sample, EXHAUSTIVELY over the interesting radii:
//                   dfloor, dfloor-1, every breakpoint radius and that radius +/- 1, dmax,
//                   dmax+1 -- plus uniformly random radii in [dfloor, dmax].
//   4. onset        the inverse staircase, on the same edge sample, for every k in
//                   [0, deathlevel(e)+1].
//
// THE ORACLE REFUSES A DAMAGED TEXT FILE.  The oracle used to skip malformed lines exactly as
// the packer did, which made the whole differential vacuous against the one failure that
// actually happens: a .kcs_index truncated by a full disk parses into a smaller staircase on
// BOTH sides, so every check agrees and kcs_verify prints OK.  A text file that does not parse
// completely, or does not end in a newline, is now a hard failure before any comparison runs.
//
// usage: kcs_verify <graph>.txt.kcs_index <graph>.kcsb [--edges N] [--seed S]
//        --edges N   edges in the coreness/onset sample (0 = all).  Default 20000.
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdint>
#include <cerrno>
#include <string>
#include <vector>
#include <random>
#include <algorithm>
#include <chrono>
#include "kcs_format.hpp"
#include "kcs_reader.hpp"
using namespace std;
using namespace kcs;
using ll = long long;

static double now_s() {
    return chrono::duration<double>(chrono::steady_clock::now().time_since_epoch()).count();
}

int main(int argc, char** argv) {
    if (argc < 3) { fprintf(stderr, "usage: kcs_verify <text.kcs_index> <file.kcsb> "
                                    "[--edges N] [--seed S]\n"); return 1; }
    string tpath = argv[1], bpath = argv[2];
    u64 nsample = 20000; unsigned seed = 20260817u;
    for (int i = 3; i < argc; ++i) {
        if (!strcmp(argv[i], "--edges") && i + 1 < argc) nsample = strtoull(argv[++i], nullptr, 10);
        else if (!strcmp(argv[i], "--seed") && i + 1 < argc) seed = (unsigned)strtoul(argv[++i], nullptr, 10);
        else { fprintf(stderr, "kcs_verify: unknown arg %s\n", argv[i]); return 1; }
    }

    // ---- oracle: re-parse the text exactly as the consumer does ----
    double t0 = now_s();
    FILE* f = fopen(tpath.c_str(), "r");
    if (!f) { perror("open text"); return 1; }
    vector<u32> eu, ev, ebase, curveD, curveV; vector<ll> et; vector<u64> coff; coff.push_back(0);
    ll DMAX = 0, DFLOOR = 0; u32 maxnode = 0;
    size_t bufcap = 1 << 20; char* buf = (char*)malloc(bufcap); ssize_t nn;
    u64 lineno = 0, nbad = 0; bool saw_range = false, unterminated = false;
    auto bad_line = [&](const char* why) {
        ++nbad;
        if (nbad <= 20) fprintf(stderr, "kcs_verify: %s:%llu: %s\n", tpath.c_str(),
                                (unsigned long long)lineno, why);
    };
    while ((nn = getline(&buf, &bufcap, f)) > 0) {
        ++lineno;
        unterminated = (buf[nn - 1] != '\n');
        if (buf[0] == '#') {
            char* p = strstr(buf, "Delta in [");
            if (p) { p += 10; DFLOOR = strtoll(p, &p, 10); while (*p == ',' || *p == ' ') ++p;
                     DMAX = strtoll(p, &p, 10); saw_range = true; }
            continue;
        }
        {   const char* q = buf; while (*q==' '||*q=='\t'||*q=='\r'||*q=='\n') ++q;
            if (!*q) { bad_line("blank line inside the index"); continue; } }
        char* p = buf; char* q0;
        q0 = p; long u = strtol(p, &p, 10); if (p == q0) { bad_line("no endpoint u"); continue; }
        q0 = p; long v = strtol(p, &p, 10); if (p == q0) { bad_line("no endpoint v"); continue; }
        q0 = p; ll   t = strtoll(p, &p, 10); if (p == q0) { bad_line("no timestamp t"); continue; }
        char* bar = strchr(p, '|'); if (!bar) { bad_line("record has no '|'"); continue; }
        char* bp = strstr(bar, "base="); if (!bp) { bad_line("record has no 'base='"); continue; }
        bp += 5;
        q0 = bp; u64 base64 = strtoull(bp, &bp, 10);
        if (bp == q0) { bad_line("'base=' has no value"); continue; }
        if (u < 0 || (unsigned long long)u > 0xFFFFFFFFull) { bad_line("endpoint u outside u32"); continue; }
        if (v < 0 || (unsigned long long)v > 0xFFFFFFFFull) { bad_line("endpoint v outside u32"); continue; }
        if (base64 > 0xFFFFFFFFull) { bad_line("base outside u32"); continue; }
        u32 base = (u32)base64;
        const size_t mark = curveD.size(); bool line_ok = true;
        char* sc = strchr(bp, ';');
        if (sc) { p = sc + 1;
            while (*p) { while (*p == ' ' || *p == '\n' || *p == '\r') ++p; if (!*p) break;
                q0 = p; ll d = strtoll(p, &p, 10);
                if (p == q0) { bad_line("breakpoint has no radius"); line_ok = false; break; }
                if (*p != ':') { bad_line("breakpoint radius not followed by ':'"); line_ok = false; break; }
                ++p;
                q0 = p; u64 val64 = strtoull(p, &p, 10);
                if (p == q0) { bad_line("breakpoint has no value"); line_ok = false; break; }
                if (d < 0 || (unsigned long long)d > 0xFFFFFFFFull) {
                    bad_line("breakpoint radius outside u32 (would wrap)"); line_ok = false; break; }
                if (val64 > 0xFFFFFFFFull) { bad_line("breakpoint value outside u32"); line_ok = false; break; }
                curveD.push_back((u32)d); curveV.push_back((u32)val64); } }
        if (!line_ok) { curveD.resize(mark); curveV.resize(mark); continue; }
        eu.push_back((u32)u); ev.push_back((u32)v); et.push_back(t); ebase.push_back(base);
        if ((u32)u > maxnode) maxnode = (u32)u;
        if ((u32)v > maxnode) maxnode = (u32)v;
        coff.push_back(curveD.size());
    }
    if (ferror(f)) { perror("read text"); fclose(f); free(buf); return 1; }
    fclose(f); free(buf);
    if (unterminated || nbad || !saw_range) {
        if (unterminated) fprintf(stderr, "kcs_verify: FATAL %s ends without a newline -- the "
                                          "text file is TRUNCATED\n", tpath.c_str());
        if (nbad) fprintf(stderr, "kcs_verify: FATAL %llu unparseable line%s in %s -- the "
                                  "oracle would silently agree with a damaged index\n",
                          (unsigned long long)nbad, nbad == 1 ? "" : "s", tpath.c_str());
        if (!saw_range) fprintf(stderr, "kcs_verify: FATAL %s has no '# ... Delta in [lo, hi]' "
                                        "header line\n", tpath.c_str());
        printf("KCSVERIFY FAIL\n");
        fflush(stdout);
        return 3;
    }
    const u64 m = eu.size(), nbp = curveD.size();
    fprintf(stderr, "[kcs_verify] text: m=%llu nbp=%llu Dfloor=%lld Dmax=%lld (%.2fs)\n",
            (unsigned long long)m, (unsigned long long)nbp, DFLOOR, DMAX, now_s() - t0);

    KcsIndex R;
    if (!R.open(bpath.c_str())) return 1;

    u64 bad = 0, checks = 0;
    auto err = [&](const char* what, u64 i, ll got, ll want) {
        ++bad; if (bad <= 20)
            fprintf(stderr, "  MISMATCH %-12s idx=%llu got=%lld want=%lld\n",
                    what, (unsigned long long)i, got, want); };

    // ---- 1. structure ----
    u32 KMAX = 0;
    for (u64 e = 0; e < m; ++e) {
        u32 fin = (coff[e+1] > coff[e]) ? curveV[coff[e+1]-1] : ebase[e];
        if (fin > KMAX) KMAX = fin;
    }
    if (R.m() != m)             err("m", 0, (ll)R.m(), (ll)m);
    if (R.nbp() != nbp)         err("nbp", 0, (ll)R.nbp(), (ll)nbp);
    if (R.dfloor() != DFLOOR)   err("dfloor", 0, R.dfloor(), DFLOOR);
    if (R.dmax() != DMAX)       err("dmax", 0, R.dmax(), DMAX);
    if (R.kmax() != KMAX)       err("kmax", 0, (ll)R.kmax(), (ll)KMAX);
    if (R.maxnode() != maxnode) err("maxnode", 0, (ll)R.maxnode(), (ll)maxnode);
    checks += 6;
    double t1 = now_s();
    for (u64 e = 0; e < m; ++e) {
        if (R.eu(e) != eu[e])           err("eu", e, R.eu(e), eu[e]);
        if (R.ev(e) != ev[e])           err("ev", e, R.ev(e), ev[e]);
        if (R.et(e) != et[e])           err("et", e, R.et(e), et[e]);
        if (R.base(e) != ebase[e])      err("base", e, R.base(e), ebase[e]);
        if (R.bpbegin(e) != coff[e])    err("bpbegin", e, (ll)R.bpbegin(e), (ll)coff[e]);
        if (R.bpend(e) != coff[e+1])    err("bpend", e, (ll)R.bpend(e), (ll)coff[e+1]);
        checks += 6;
    }
    for (u64 c = 0; c < nbp; ++c) {
        if (R.bpd(c) != curveD[c]) err("bpd", c, R.bpd(c), curveD[c]);
        if (R.bpv(c) != curveV[c]) err("bpv", c, R.bpv(c), curveV[c]);
        checks += 2;
    }
    fprintf(stderr, "[kcs_verify] 1. structure: %llu checks, %llu bad (%.2fs)\n",
            (unsigned long long)checks, (unsigned long long)bad, now_s() - t1);

    // ---- 2. deathlevel, every edge, O(1) path ----
    t1 = now_s(); u64 dl_bad = bad, dl_checks = 0;
    for (u64 e = 0; e < m; ++e) {
        u32 want = (coff[e+1] > coff[e]) ? curveV[coff[e+1]-1] : ebase[e];
        if (R.deathlevel(e) != want) err("deathlevel", e, R.deathlevel(e), want);
        ++dl_checks;
    }
    fprintf(stderr, "[kcs_verify] 2. deathlevel: %llu edges, %llu bad (%.2fs)\n",
            (unsigned long long)dl_checks, (unsigned long long)(bad - dl_bad), now_s() - t1);
    checks += dl_checks;

    // ---- 3/4. coreness + onset on an edge sample, exhaustive over interesting radii ----
    vector<u64> samp;
    if (nsample == 0 || nsample >= m) { samp.resize(m); for (u64 e = 0; e < m; ++e) samp[e] = e; }
    else {
        mt19937_64 rng(seed);
        vector<u64> all(m); for (u64 e = 0; e < m; ++e) all[e] = e;
        for (u64 i = 0; i < nsample; ++i) std::swap(all[i], all[i + rng() % (m - i)]);
        samp.assign(all.begin(), all.begin() + nsample);
        sort(samp.begin(), samp.end());
    }
    t1 = now_s();
    mt19937_64 rng(seed ^ 0x9e3779b97f4a7c15ull);
    u64 core_checks = 0, core_bad = bad, onset_checks = 0;
    vector<ll> radii;
    for (u64 si = 0; si < samp.size(); ++si) {
        u64 e = samp[si];
        radii.clear();
        radii.push_back(DFLOOR - 1); radii.push_back(DFLOOR); radii.push_back(DFLOOR + 1);
        radii.push_back(DMAX - 1);   radii.push_back(DMAX);   radii.push_back(DMAX + 1);
        for (u64 c = coff[e]; c < coff[e+1]; ++c) {
            ll d = curveD[c];
            radii.push_back(d - 1); radii.push_back(d); radii.push_back(d + 1);
        }
        for (int r = 0; r < 8; ++r)
            radii.push_back(DFLOOR + (ll)(rng() % (u64)(DMAX - DFLOOR + 1)));
        for (ll D : radii) {
            // oracle: linear scan of the staircase
            u32 want = ebase[e];
            ll Dc = D < DFLOOR ? DFLOOR : (D > DMAX ? DMAX : D);
            for (u64 c = coff[e]; c < coff[e+1]; ++c) { if ((ll)curveD[c] <= Dc) want = curveV[c]; else break; }
            u32 got = R.coreness(e, D);
            if (got != want) { err("coreness", e, got, want);
                if (bad <= 20) fprintf(stderr, "      (at Delta=%lld)\n", D); }
            ++core_checks;
        }
        u32 dl = (coff[e+1] > coff[e]) ? curveV[coff[e+1]-1] : ebase[e];
        for (u32 k = 0; k <= dl + 1; ++k) {
            ll want;
            if (ebase[e] >= k) want = DFLOOR;
            else {
                want = DMAX + 1;
                for (u64 c = coff[e]; c < coff[e+1]; ++c)
                    if (curveV[c] >= k) { want = curveD[c]; break; }
            }
            ll got = R.onset(e, k);
            if (got != want) { err("onset", e, got, want);
                if (bad <= 20) fprintf(stderr, "      (at k=%u)\n", k); }
            ++onset_checks;
        }
    }
    fprintf(stderr, "[kcs_verify] 3. coreness: %llu (e,Delta) probes over %zu edges, %llu bad\n",
            (unsigned long long)core_checks, samp.size(), (unsigned long long)(bad - core_bad));
    fprintf(stderr, "[kcs_verify] 4. onset:    %llu (e,k) probes (%.2fs total)\n",
            (unsigned long long)onset_checks, now_s() - t1);
    checks += core_checks + onset_checks;

    R.close();
    fprintf(stderr, "[kcs_verify] %s : %llu checks, %llu mismatches\n",
            bad ? "FAIL" : "OK", (unsigned long long)checks, (unsigned long long)bad);
    // The verdict line is what validate_kcsb.sh greps for.  Losing it (redirect to a full
    // disk) would turn a passing run into a spurious FAIL, so the write is checked.
    printf("%s\n", bad ? "KCSVERIFY FAIL" : "KCSVERIFY OK");
    if (fflush(stdout) != 0 || ferror(stdout)) {
        fprintf(stderr, "kcs_verify: FATAL could not write the verdict to stdout: %s\n",
                strerror(errno));
        return 1;
    }
    return bad ? 3 : 0;
}
