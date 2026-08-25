// kcs_format.hpp -- on-disk layout of the .kcsb binary form of the Delta-spectrum index
//                   (shared by the packer kcs_pack and the reader kcs_reader.hpp).
//
// WHAT IT STORES.  Exactly what the ".kcs_index" TEXT file stores, nothing more:
//
//     # (k,Delta)-core spectrum index  |  Delta in [<dfloor>, <dmax>]
//     # format:  u v t | base=<core at Delta=dfloor> ; <Delta_up>:<core> ...
//     0 229 2 | base=1 ; 244:2 439:3 753:4 ...
//
// i.e. for every temporal edge e a triple (u,v,t), a base coreness, and a staircase of
// breakpoints (Delta_up, value) meaning "from radius Delta_up upwards, coreness_e = value".
// The staircase is strictly increasing in both coordinates (the text builder guarantees it;
// the packer re-checks every edge and refuses to write a file that violates it).
//
// WHY A BINARY FORM.  Two reasons, in this order:
//
//   1. deathlevel(e) = coreness_e(dmax) -- the level at which edge e leaves every core --
//      is the LAST breakpoint's value, or `base` for an edge with no breakpoints.  The
//      community index needs it for whole-level queries, to skip edges that are not alive
//      at the queried k.  In this CSR layout that is
//          bpv[bpoff[e+1]-1]      (or base[e] when bpoff[e+1] == bpoff[e])
//      -- ONE indexed load, no predecessor search, and no need to duplicate the value into
//      the community index.
//   2. The text form must be parsed before any of it is usable: 4.4-4.6 s for the 3.1-3.4 GB
//      email-Eu / enron indexes.  The binary form is mmap'ed and usable at once.
//
// LAYOUT.  Fixed 256-byte header, then seven sections, each 8-byte aligned and each followed
// by 8 bytes of slack so that a width-packed element may always be read with one unaligned
// 8-byte load and a mask:
//
//     KS_EU     m     x u32                 endpoint u
//     KS_EV     m     x u32                 endpoint v
//     KS_ET     m     x w_t                 timestamp t
//     KS_BASE   m     x w_base              coreness at Delta = dfloor
//     KS_BPOFF  (m+1) x w_off               CSR offsets into KS_BPD / KS_BPV
//     KS_BPD    nbp   x w_d                 breakpoint radius Delta_up, ascending within e
//     KS_BPV    nbp   x w_v                 breakpoint coreness value, ascending within e
//
// The two breakpoint arrays are split (struct-of-arrays) rather than interleaved.  Total
// bytes are identical either way; the split is free and it halves the footprint of the
// predecessor search, which reads only KS_BPD.
//
// FIELD WIDTHS ARE MEASURED, NOT ASSUMED.  The packer scans the whole file first and picks
// the narrowest width that holds the observed maximum.  This matters -- the widths are NOT
// the ones a glance at the small test graphs suggests:
//
//                     ex     rand    hub    scale_h100k   email-Eu      enron
//     dmax           1e5     1e5     1e5       1e5        69,459,254  1,026,473,760
//     max Delta_up     9    1,969  2,877     99,534       69,440,220    112,092,034
//     w_d              1       2      2          3            4              4
//     max coreness     3      14      8          2          4,878          1,500
//     w_v              1       1      1          1              2              2
//
// The "Delta in [0, 100000]" of the toy graphs is a property of those generators, not of the
// format: on the real graphs Delta is a raw timestamp difference and needs the full u32.
//
// ENDIANNESS: little-endian, like the .cc files.  `endian` in the header is a marker.
#pragma once
#include <cstdint>
#include <cstddef>
#include <cstring>

namespace kcs {

using u8  = uint8_t;
using u32 = uint32_t;
using u64 = uint64_t;
using i64 = int64_t;

static const char  KCSB_MAGIC[8]  = { 'K','C','S','B','0','1','\0','\0' };
static const u32   KCSB_VERSION   = 1;
static const u32   KCSB_ENDIAN    = 0x01020304u;
static const size_t KCSB_PAD      = 8;     // slack after every section (masked 8-byte loads)
static const size_t KCSB_ALIGN    = 8;

enum {
    KS_EU = 0,      // m     x u32
    KS_EV,          // m     x u32
    KS_ET,          // m     x w_t      (4 or 8; 8 iff some t is negative or exceeds u32)
    KS_BASE,        // m     x w_base   (1..4)
    KS_BPOFF,       // (m+1) x w_off    (4 or 8; 8 iff nbp exceeds u32)
    KS_BPD,         // nbp   x w_d      (1..8)
    KS_BPV,         // nbp   x w_v      (1..4)
    KS_COUNT
};

// header flags
enum { KHF_T_SIGNED = 1u };   // KS_ET holds two's-complement i64 (w_t == 8 and some t < 0)

struct KSection { u64 off, len; };

#pragma pack(push,1)
struct KcsbHeader {
    char magic[8];        // "KCSB01"
    u32  version;         // KCSB_VERSION
    u32  endian;          // KCSB_ENDIAN
    u64  m;               // #temporal edges
    u64  nbp;             // total breakpoints over all edges
    i64  dfloor;          // radius floor of the source spectrum
    i64  dmax;            // radius ceiling
    u32  kmax;            // max_e deathlevel(e)  == max coreness anywhere in the file
    u32  maxnode;         // max endpoint id
    u8   w_t, w_base, w_off, w_d, w_v, wpad[3];
    u32  flags;
    u32  reserved0;
    KSection sec[KS_COUNT];      // 72 + 7*16 = 184
    u8   reserved[72];           // -> 256
};
#pragma pack(pop)
static_assert(sizeof(KcsbHeader) == 256, "kcsb header must be 256 bytes");

static const u64 KCSB_WMASK[9] = { 0ull, 0xFFull, 0xFFFFull, 0xFFFFFFull, 0xFFFFFFFFull,
    0xFFFFFFFFFFull, 0xFFFFFFFFFFFFull, 0xFFFFFFFFFFFFFFull, 0xFFFFFFFFFFFFFFFFull };

// narrowest byte width holding `v` (never 0, so that a width is always a legal stride)
inline unsigned kcsb_width(u64 v) { unsigned w = 1; while (v >> (8 * w)) ++w; return w; }

// one unaligned 8-byte load + mask; requires KCSB_PAD bytes of slack past the array
inline u64 kcsb_load(const unsigned char* p, unsigned w) {
    u64 v; std::memcpy(&v, p, 8); return v & KCSB_WMASK[w];
}
inline void kcsb_store(unsigned char* p, unsigned w, u64 v) {
    for (unsigned i = 0; i < w; ++i) p[i] = (unsigned char)(v >> (8 * i));
}

} // namespace kcs
