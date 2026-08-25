// kcs_reader.hpp -- mmap reader for the .kcsb binary Delta-spectrum index.
//
// Header-only, no allocation, no parsing: open() maps the file and takes seven pointers out
// of the header.  Everything below is an indexed load.
//
//   deathlevel(e)      O(1)        coreness_e(dmax) -- the level at which e leaves every core.
//                                  This is THE reason the binary form exists: the community
//                                  index needs it per edge for whole-level queries and must
//                                  not have to duplicate it.  It is the last breakpoint's
//                                  value, which CSR puts at a known index.
//   coreness(e, D)     O(log b_e)  predecessor search over edge e's breakpoint radii.  Only
//                                  KS_BPD is touched during the search; the value array is
//                                  read once at the end.
//   onset(e, k)        O(log b_e)  the inverse staircase: smallest Delta with coreness >= k,
//                                  or dmax+1 if e never reaches k.  (The community index's
//                                  radius derivation dlo(X,k) = max(g*, onset_k(a*), onset_k(b*))
//                                  is stated in terms of this.)
//   m(), nbp(), dfloor(), dmax(), kmax(), maxnode(), eu/ev/et  -- raw header + edge table.
//
// Widths are per-file (see kcs_format.hpp).  Each accessor loads 8 bytes and masks, so every
// width-packed section is written with KCSB_PAD bytes of slack behind it.
#pragma once
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <sys/mman.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include "kcs_format.hpp"

namespace kcs {

struct KcsIndex {
    const unsigned char* base_ = nullptr;
    size_t fsize = 0;
    const KcsbHeader* h = nullptr;
    const u32* eu_ = nullptr;
    const u32* ev_ = nullptr;
    const unsigned char *et_ = nullptr, *base8_ = nullptr, *off_ = nullptr,
                        *bpd_ = nullptr, *bpv_ = nullptr;
    unsigned wt = 0, wbase = 0, woff = 0, wd = 0, wv = 0;
    bool tsigned = false;

    // Returns false and prints why on any structural problem; never aborts.
    bool open(const char* path) {
        int fd = ::open(path, O_RDONLY);
        if (fd < 0) { perror("open .kcsb"); return false; }
        struct stat st;
        if (fstat(fd, &st) != 0) { perror("fstat .kcsb"); ::close(fd); return false; }
        fsize = (size_t)st.st_size;
        if (fsize < sizeof(KcsbHeader)) {
            fprintf(stderr, "%s: too small to be a .kcsb\n", path); ::close(fd); return false; }
        void* p = mmap(nullptr, fsize, PROT_READ, MAP_PRIVATE, fd, 0);
        ::close(fd);
        if (p == MAP_FAILED) { perror("mmap .kcsb"); return false; }
        base_ = (const unsigned char*)p;
        h = (const KcsbHeader*)base_;
        if (memcmp(h->magic, KCSB_MAGIC, 8) != 0) {
            fprintf(stderr, "%s: bad magic (expected KCSB01)\n", path); return false; }
        if (h->version != KCSB_VERSION) {
            fprintf(stderr, "%s: version %u, this build reads %u\n", path, h->version, KCSB_VERSION);
            return false; }
        if (h->endian != KCSB_ENDIAN) {
            fprintf(stderr, "%s: endian marker %08x -- foreign byte order\n", path, h->endian);
            return false; }
        wt = h->w_t; wbase = h->w_base; woff = h->w_off; wd = h->w_d; wv = h->w_v;
        tsigned = (h->flags & KHF_T_SIGNED) != 0;
        if (!wt || !wbase || !woff || !wd || !wv || wt > 8 || wbase > 8 || woff > 8 || wd > 8 || wv > 8) {
            fprintf(stderr, "%s: illegal field widths %u/%u/%u/%u/%u\n", path,
                    wt, wbase, woff, wd, wv); return false; }
        // Bound m and nbp before any size arithmetic: the checks below multiply them by a
        // width, and a corrupt or forged header with m near 2^62 would wrap the product to a
        // small number that passes the in-file test.  Nothing this format describes can be
        // anywhere near 2^48 elements, and 2^48 * 8 still cannot overflow u64.
        const u64 KCSB_MAX_ELEMS = (u64)1 << 48;
        if (h->m >= KCSB_MAX_ELEMS || h->nbp >= KCSB_MAX_ELEMS) {
            fprintf(stderr, "%s: implausible header m=%llu nbp=%llu (limit %llu)\n", path,
                    (unsigned long long)h->m, (unsigned long long)h->nbp,
                    (unsigned long long)KCSB_MAX_ELEMS);
            return false;
        }
        // every section must lie inside the file, with its slack
        const u64 want[KS_COUNT] = { h->m * 4, h->m * 4, h->m * wt, h->m * wbase,
                                     (h->m + 1) * woff, h->nbp * wd, h->nbp * wv };
        for (int s = 0; s < KS_COUNT; ++s) {
            if (h->sec[s].len != want[s] ||
                h->sec[s].off + h->sec[s].len + KCSB_PAD > fsize) {
                fprintf(stderr, "%s: section %d off=%llu len=%llu (want %llu), file %zu\n", path, s,
                        (unsigned long long)h->sec[s].off, (unsigned long long)h->sec[s].len,
                        (unsigned long long)want[s], fsize);
                return false;
            }
        }
        eu_    = (const u32*)(base_ + h->sec[KS_EU].off);
        ev_    = (const u32*)(base_ + h->sec[KS_EV].off);
        et_    = base_ + h->sec[KS_ET].off;
        base8_ = base_ + h->sec[KS_BASE].off;
        off_   = base_ + h->sec[KS_BPOFF].off;
        bpd_   = base_ + h->sec[KS_BPD].off;
        bpv_   = base_ + h->sec[KS_BPV].off;
        return true;
    }
    void close() {
        if (base_) { munmap((void*)base_, fsize); base_ = nullptr; h = nullptr; }
    }

    // ---- header ----
    u64  m()       const { return h->m; }
    u64  nbp()     const { return h->nbp; }
    i64  dfloor()  const { return h->dfloor; }
    i64  dmax()    const { return h->dmax; }
    u32  kmax()    const { return h->kmax; }
    u32  maxnode() const { return h->maxnode; }

    // ---- edge table ----
    u32 eu(u64 e) const { return eu_[e]; }
    u32 ev(u64 e) const { return ev_[e]; }
    i64 et(u64 e) const {
        u64 raw = kcsb_load(et_ + e * wt, wt);
        if (tsigned) { i64 s; std::memcpy(&s, &raw, 8); return s; }
        return (i64)raw;
    }
    u32 base(u64 e) const { return (u32)kcsb_load(base8_ + e * wbase, wbase); }

    // ---- breakpoint CSR ----
    u64 bpbegin(u64 e) const { return kcsb_load(off_ + e * woff, woff); }
    u64 bpend  (u64 e) const { return kcsb_load(off_ + (e + 1) * woff, woff); }
    u64 bpcount(u64 e) const { return bpend(e) - bpbegin(e); }
    u32 bpd(u64 c) const { return (u32)kcsb_load(bpd_ + c * wd, wd); }
    u32 bpv(u64 c) const { return (u32)kcsb_load(bpv_ + c * wv, wv); }

    // ---- O(1): coreness at Delta = dmax, i.e. the level at which e dies ----
    u32 deathlevel(u64 e) const {
        u64 b = bpbegin(e), n = bpend(e);
        return n > b ? bpv(n - 1) : base(e);
    }

    // ---- O(log b_e): coreness_e(D).  D < dfloor is treated as dfloor. ----
    u32 coreness(u64 e, i64 D) const {
        u64 lo = bpbegin(e), hi = bpend(e);
        if (D >= h->dmax) return hi > lo ? bpv(hi - 1) : base(e);   // saturates at dmax
        if (D < h->dfloor) D = h->dfloor;
        // last c in [lo,hi) with bpd(c) <= D
        u64 a = lo, b = hi;                    // invariant: answer in [a-1, b-1]
        while (a < b) { u64 mid = a + (b - a) / 2;
                        if ((i64)bpd(mid) <= D) a = mid + 1; else b = mid; }
        return a > lo ? bpv(a - 1) : base(e);
    }

    // ---- O(log b_e): smallest Delta at which coreness_e reaches k (dmax+1 if never) ----
    i64 onset(u64 e, u32 k) const {
        if (base(e) >= k) return h->dfloor;
        u64 lo = bpbegin(e), hi = bpend(e);
        if (hi == lo || bpv(hi - 1) < k) return h->dmax + 1;
        u64 a = lo, b = hi - 1;                // first c with bpv(c) >= k
        while (a < b) { u64 mid = a + (b - a) / 2;
                        if (bpv(mid) >= k) b = mid; else a = mid + 1; }
        return (i64)bpd(a);
    }
};

} // namespace kcs
