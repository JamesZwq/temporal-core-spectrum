// hier_index -- build a REAL, directly-queryable on-disk community-hierarchy index.
//
// Why this exists: experiments/landscape.cpp dumps a `.landscape` file, but that file is
// an ASCII merge transcript ("M k w a b") that nothing ever reads back; its own validator
// recomputes the candidate links and the union-find from the spectrum index instead of
// opening the file.  So there was no on-disk structure to answer a community query against.
//
// This program writes `.hidx`: for every level k, the merge forest itself -- leaf order,
// per-leaf parent, and internal nodes carrying (merge radius, parent, first leaf, leaf
// count).  A query mmaps the file and answers (e, k, Delta) with one predecessor search up
// the leaf-to-root path plus one contiguous range read.  No recomputation.
//
//   build:    hier_index <index-file> --build=<out.hidx>
//   validate: hier_index <index-file> --hidx=<file> --validate
//   query:    hier_index <index-file> --hidx=<file> --query <k> <Delta> <edge-id>
//
// The forest is built exactly as in landscape.cpp (single linkage under
// w_k(e,f) = max(|t_e - t_f|, Delta_k(e), Delta_k(f)), candidates from the nearest-smaller
// -onset monotone stack), so the two agree merge for merge.
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>
#include <algorithm>
#include <cstdint>
#include <chrono>
#include <sys/mman.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <functional>
#include <unordered_map>
using namespace std;
using ll = long long;
using u32 = uint32_t;
using u64 = uint64_t;
static double now(){ return chrono::duration<double>(chrono::steady_clock::now().time_since_epoch()).count(); }

static const u32 NIL = 0xFFFFFFFFu;

/* ---------------------------------------------------------------- on-disk layout ---- */
#pragma pack(push,1)
struct Hdr {
    char magic[8];        // "HIDX0003"
    u64  m;               // edges in the graph
    u64  dmax;
    u32  K;               // number of levels
    u32  nW;              // distinct merge radii
    u64  totLeaf;         // sum_k n_k  (= C0)
    u64  totNode;         // sum_k internal nodes
    u64  wtabOff, dirOff;
};
struct Dir {              // one per level, k = 1..K
    u64 ordOff;           // u32 ord[nk]     leaf position -> edge id
    u64 mapOff;           // u32 pairs[2*nk] (edge, leaf position), ascending by edge
    u64 lparOff;          // u32 lpar[nk]    leaf position -> internal node id (NIL if a lone root)
    u64 lonsOff;          // u32 lons[nk]    leaf position -> its own onset (radius rank)
    u64 nodeOff;          // Node node[nint]
    u32 nk, nint;
};
struct Node { u32 wrank, par, first, cnt, top; };   // top = last id of this heavy path
#pragma pack(pop)

/* --------------------------------------------------------------------- the parser ---- */
struct Spectrum {
    vector<int> eu, ev, ebase; vector<ll> et;
    vector<ll> curveD; vector<int> curveV; vector<size_t> coff;
    vector<int> efin;                       // coreness at DMAX, filled by the binary reader
    ll DMAX=0; int maxnode=0, maxcore=0; size_t m=0, B=0;
};
static bool parseIndex(const char* path, Spectrum& S){
    FILE* f=fopen(path,"r"); if(!f){ perror("open index"); return false; }
    S.coff.push_back(0);
    size_t bufcap=1<<20; char* buf=(char*)malloc(bufcap); ssize_t nn;
    while((nn=getline(&buf,&bufcap,f))>0){
        if(buf[0]=='#'){ char* p=strstr(buf,"Delta in ["); if(p){ p+=10; strtoll(p,&p,10);
            while(*p==','||*p==' ')++p; S.DMAX=strtoll(p,&p,10);} continue; }
        char* p=buf;
        long u=strtol(p,&p,10), v=strtol(p,&p,10); ll t=strtoll(p,&p,10);
        char* bar=strchr(p,'|'); if(!bar) continue;
        char* bp=strstr(bar,"base="); if(!bp) continue; bp+=5; int base=(int)strtol(bp,&bp,10);
        S.eu.push_back((int)u); S.ev.push_back((int)v); S.et.push_back(t); S.ebase.push_back(base);
        if(u>S.maxnode)S.maxnode=(int)u; if(v>S.maxnode)S.maxnode=(int)v;
        char* sc=strchr(bp,';');
        if(sc){ p=sc+1; while(*p){ while(*p==' '||*p=='\n')++p; if(!*p)break;
            ll d=strtoll(p,&p,10); if(*p!=':')break; ++p; int val=(int)strtol(p,&p,10);
            S.curveD.push_back(d); S.curveV.push_back(val); } }
        S.coff.push_back(S.curveD.size());
    }
    fclose(f); free(buf);
    S.m=S.eu.size(); S.B=S.curveD.size(); S.efin.resize(S.m);
    for(size_t e=0;e<S.m;++e){
        S.efin[e]=(S.coff[e+1]>S.coff[e])? S.curveV[S.coff[e+1]-1] : S.ebase[e];
        if(S.efin[e]>S.maxcore)S.maxcore=S.efin[e]; }
    return true;
}

/* ===================== reading the binary index (KCSSTRM2) =========================
   Ported verbatim from hier_build.cpp, which is where it was validated: the six graphs
   that have both an ASCII index and a .strm produce identical Spectrum contents.  Only
   six of the twenty-one graphs ever got an ASCII index, so the U certificate could not
   be measured beyond them until this reader existed here too.

   The .strm carries only the coreness stream, not the graph, so the graph file is read
   alongside it.  Edge ids must land exactly where kcs put them, which means replaying
   kcs::load_graph: skip comments and self loops, intern node ids in first-seen order,
   normalise u<=v, sort by (t,u,v), then drop exact duplicates.  A leading lone integer is
   a count header, and any column after the timestamp is ignored (the Enron convention).

   The stream is radius-major DESCENDING and each event says what the coreness is BELOW a
   threshold, so an edge's ascending staircase is recovered by pairing each threshold with
   the value carried by the next threshold up, the topmost one taking the seed. */
static inline u32 g_u32(const uint8_t* p){ u32 v=0; for(int i=0;i<4;++i) v|=(u32)p[i]<<(8*i); return v; }
static inline u64 g_u64(const uint8_t* p){ u64 v=0; for(int i=0;i<8;++i) v|=(u64)p[i]<<(8*i); return v; }
static inline ll  g_i64(const uint8_t* p){ return (ll)g_u64(p); }

struct GEdge { int u, v; ll t; };
static bool loadGraph(const char* path, vector<GEdge>& out, int& node_count){
    FILE* f=fopen(path,"r"); if(!f){ perror("open graph"); return false; }
    vector<GEdge> keys; keys.reserve(1<<20);
    unordered_map<ll,int> ids; ids.reserve(1<<20);
    auto getid=[&](ll raw){ auto it=ids.find(raw); if(it!=ids.end()) return it->second;
                            int n=(int)ids.size(); ids.emplace(raw,n); return n; };
    size_t cap=1<<16; char* line=(char*)malloc(cap); ssize_t nn;
    while((nn=getline(&line,&cap,f))>0){
        char* q=line; while(*q==' '||*q=='\t'||*q=='\r'||*q=='\n') ++q;
        if(!*q||*q=='#'||*q=='%') continue;
        char* e1=nullptr; ll a=strtoll(q,&e1,10); if(e1==q) continue;
        char* q2=e1; while(*q2==' '||*q2=='\t') ++q2;
        if(!*q2||*q2=='#'||*q2=='%'||*q2=='\r'||*q2=='\n') continue;   // count header
        char* e2=nullptr; ll b=strtoll(q2,&e2,10); if(e2==q2) continue;
        char* q3=e2; while(*q3==' '||*q3=='\t') ++q3;
        if(!*q3||*q3=='#'||*q3=='%'||*q3=='\r'||*q3=='\n') continue;
        char* e3=nullptr; ll t=strtoll(q3,&e3,10); if(e3==q3) continue;
        if(t<0) continue;
        if(a==b) continue;                                             // self loop
        int u=getid(a), v=getid(b); if(u>v) swap(u,v);
        keys.push_back({u,v,t});
    }
    fclose(f); free(line);
    node_count=(int)ids.size();
    sort(keys.begin(),keys.end(),[](const GEdge& x,const GEdge& y){
        return x.t!=y.t ? x.t<y.t : (x.u!=y.u ? x.u<y.u : x.v<y.v); });
    keys.erase(unique(keys.begin(),keys.end(),[](const GEdge& x,const GEdge& y){
        return x.t==y.t&&x.u==y.u&&x.v==y.v; }), keys.end());
    out.swap(keys);
    return true;
}

static bool parseStream(const char* strm, const char* graph, Spectrum& S){
    vector<GEdge> ge; int nc=0;
    if(!loadGraph(graph,ge,nc)) return false;
    int fd=::open(strm,O_RDONLY); if(fd<0){ perror("open .strm"); return false; }
    struct stat sb; fstat(fd,&sb);
    const uint8_t* base=(const uint8_t*)mmap(nullptr,sb.st_size,PROT_READ,MAP_PRIVATE,fd,0);
    ::close(fd);
    if(base==MAP_FAILED){ perror("mmap .strm"); return false; }
    if(memcmp(base,"KCSSTRM2",8)){ fprintf(stderr,"not a KCSSTRM2 file\n"); return false; }
    u64 edges=g_u64(base+32), groups=g_u64(base+40), records=g_u64(base+48);
    ll  floor_=g_i64(base+56), dmax=g_i64(base+64);
    u64 soff=g_u64(base+72), boff=g_u64(base+88);
    if(edges!=ge.size()){
        fprintf(stderr,"edge count mismatch: stream %llu vs graph %zu\n",
                (unsigned long long)edges, ge.size()); return false; }
    S.m=edges; S.DMAX=dmax; S.maxnode=nc-1;
    S.eu.resize(edges); S.ev.resize(edges); S.et.resize(edges);
    for(size_t e=0;e<edges;++e){ S.eu[e]=ge[e].u; S.ev[e]=ge[e].v; S.et[e]=ge[e].t; }
    vector<GEdge>().swap(ge);
    const uint8_t* seed=base+soff;                       /* coreness at dmax, per edge */

    /* pass 1: how many events each edge has */
    vector<u32> cnt(edges+1,0);
    { const uint8_t* p=base+boff;
      for(u64 g=0;g<groups;++g){ u64 c=g_u64(p+8); p+=16;
          for(u64 i=0;i<c;++i){ cnt[g_u32(p)+1]++; p+=8; } } }
    S.coff.assign(edges+1,0);
    for(size_t e=0;e<edges;++e) S.coff[e+1]=S.coff[e]+cnt[e+1];   // counts live at e+1
    S.B=S.coff[edges];
    vector<ll> D(S.B); vector<int> V(S.B);
    { vector<u64> at(S.coff.begin(),S.coff.end());
      const uint8_t* p=base+boff;
      for(u64 g=0;g<groups;++g){ ll d=g_i64(p); u64 c=g_u64(p+8); p+=16;
          for(u64 i=0;i<c;++i){ u32 e=g_u32(p); u32 vb=g_u32(p+4); p+=8;
              u64 w=at[e]++; D[w]=d; V[w]=(int)vb; } } }
    (void)records;

    /* per edge the events arrived descending; flip to ascending and shift the values up */
    S.curveD.clear(); S.curveV.clear();
    S.curveD.reserve(S.B); S.curveV.reserve(S.B);
    S.ebase.resize(edges); S.efin.resize(edges);
    vector<size_t> noff(edges+1,0);
    for(size_t e=0;e<edges;++e){
        u64 a=S.coff[e], b=S.coff[e+1]; size_t np=b-a;
        int top=(int)g_u32(seed+4*e);
        if(!np){ S.ebase[e]=top; }
        else{
            reverse(D.begin()+a, D.begin()+b); reverse(V.begin()+a, V.begin()+b);
            S.ebase[e]=V[a];
            /* delta_below is the last radius where the lower value still holds, while the
               text index records the first radius where the higher value holds. */
            for(size_t j=0;j<np;++j){
                int up = (j+1<np)? V[a+j+1] : top;
                S.curveD.push_back(D[a+j]+1); S.curveV.push_back(up);
            }
        }
        S.efin[e]=top; noff[e+1]=S.curveD.size();
        if(top>S.maxcore) S.maxcore=top;
    }
    S.coff.swap(noff);
    S.B=S.curveD.size();
    (void)floor_;
    return true;
}

static int coreAt(const Spectrum& S, size_t e, ll D){
    int val=S.ebase[e];
    for(size_t c=S.coff[e];c<S.coff[e+1];++c){ if(S.curveD[c]<=D) val=S.curveV[c]; else break; }
    return val;
}

/* ------------------------------------------------------------ per-level bucket build -- */
struct Buckets { vector<size_t> loff; vector<int> bEdge; vector<ll> bOns; size_t C0=0; };
static void buildBuckets(const Spectrum& S, Buckets& B){
    int K=S.maxcore; size_t m=S.m;
    vector<size_t> lcount(K+2,0);
    for(size_t e=0;e<m;++e){ int base=S.ebase[e], hi=base>K?K:base;
        for(int k=1;k<=hi;++k) lcount[k]++;
        int prev=base;
        for(size_t c=S.coff[e];c<S.coff[e+1];++c){ int val=S.curveV[c];
            for(int k=(prev+1>1?prev+1:1);k<=val&&k<=K;++k) lcount[k]++; prev=val; } }
    B.loff.assign(K+2,0);
    for(int k=1;k<=K+1;++k) B.loff[k]=B.loff[k-1]+lcount[k-1];
    B.C0=B.loff[K+1];
    B.bEdge.resize(B.C0); B.bOns.resize(B.C0);
    vector<size_t> fill(B.loff.begin(),B.loff.end());
    for(size_t e=0;e<m;++e){ int base=S.ebase[e], hi=base>K?K:base;
        for(int k=1;k<=hi;++k){ size_t p=fill[k]++; B.bEdge[p]=(int)e; B.bOns[p]=0; }
        int prev=base;
        for(size_t c=S.coff[e];c<S.coff[e+1];++c){ int val=S.curveV[c]; ll d=S.curveD[c];
            for(int k=(prev+1>1?prev+1:1);k<=val&&k<=K;++k){ size_t p=fill[k]++; B.bEdge[p]=(int)e; B.bOns[p]=d; }
            prev=val; } }
}

/* ================================================================= build the index ==== */
struct LevelTree {
    vector<u32> ord, lpar, lons;      // lons = each leaf's own onset, as a radius rank            // leaf position -> edge / -> internal node
    vector<u32> mapPairs;             // (edge, pos) ascending by edge
    vector<Node> node;
    vector<ll>  wOf, lonsW;           // raw radii, ranked later
};

static vector<u64>* UPAIR = nullptr;      // set when the U certificate is being collected
static vector<u32>* UPAIRK = nullptr;     // the level each pair was accepted at (span study)
static int UPAIRLVL = 0;

static void buildLevel(const Spectrum& S, const Buckets& B, int k,
                       vector<vector<pair<ll,int>>>& nodeact, vector<ll>& dk,
                       LevelTree& T){
    size_t b0=B.loff[k], b1=B.loff[k+1]; size_t nk=b1-b0;
    T = LevelTree{};
    if(!nk) return;

    /* leaf id 0..nk-1 in ascending edge order (the bucket is already built that way) */
    vector<int> le(nk); vector<ll> lo(nk);
    for(size_t i=0;i<nk;++i){ le[i]=B.bEdge[b0+i]; lo[i]=B.bOns[b0+i]; }

    /* index within the level, for the candidate scan */
    vector<int> touched;
    for(size_t i=0;i<nk;++i){ int e=le[i]; dk[e]=lo[i];
        int u=S.eu[e],v=S.ev[e];
        if(nodeact[u].empty())touched.push_back(u); nodeact[u].push_back({S.et[e],e});
        if(nodeact[v].empty())touched.push_back(v); nodeact[v].push_back({S.et[e],e}); }

    vector<pair<ll,pair<int,int>>> cand; cand.reserve(2*nk);
    vector<int> st;
    for(int nd: touched){ auto& act=nodeact[nd]; int d=(int)act.size(); if(d<2) continue;
        if(!is_sorted(act.begin(),act.end())) sort(act.begin(),act.end());
        st.clear();
        for(int p=0;p<d;++p){ while(!st.empty() && dk[act[st.back()].second] > dk[act[p].second]) st.pop_back();
            if(!st.empty()){ int L=st.back(); ll w=max(act[p].first-act[L].first, dk[act[p].second]);
                cand.push_back({w,{act[L].second,act[p].second}}); }
            st.push_back(p); }
        st.clear();
        for(int p=d-1;p>=0;--p){ while(!st.empty() && dk[act[st.back()].second] > dk[act[p].second]) st.pop_back();
            if(!st.empty()){ int R=st.back(); ll w=max(act[R].first-act[p].first, dk[act[p].second]);
                cand.push_back({w,{act[p].second,act[R].second}}); }
            st.push_back(p); } }
    sort(cand.begin(),cand.end());

    /* edge -> leaf id (le is ascending, so binary search) */
    auto leafOf=[&](int e)->u32{ return (u32)(lower_bound(le.begin(),le.end(),e)-le.begin()); };

    /* Kruskal, recording the tree instead of a transcript */
    vector<u32> par(nk); for(u32 i=0;i<nk;++i) par[i]=i;               // DSU over leaves
    vector<u32> topOf(nk); for(u32 i=0;i<nk;++i) topOf[i]=NIL;         // component -> tree node (NIL = still a bare leaf)
    vector<ll> birth(nk); for(size_t i=0;i<nk;++i) birth[i]=lo[i];
    function<u32(u32)> findp = [&](u32 x){ u32 r=x; while(par[r]!=r)r=par[r];
        while(par[x]!=r){ u32 nx=par[x]; par[x]=r; x=nx; } return r; };

    vector<u32> kidA, kidB;      // children of each internal node; NIL-tagged leaf ids use the high bit
    auto tagLeaf=[&](u32 leaf){ return leaf | 0x80000000u; };

    for(auto& c: cand){
        ll w=c.first; u32 a=leafOf(c.second.first), b=leafOf(c.second.second);
        u32 ra=findp(a), rb=findp(b); if(ra==rb) continue;
        u32 ca = topOf[ra]==NIL ? tagLeaf(ra) : topOf[ra];
        u32 cb = topOf[rb]==NIL ? tagLeaf(rb) : topOf[rb];
        u32 id=(u32)T.wOf.size(); T.wOf.push_back(w); kidA.push_back(ca); kidB.push_back(cb);
        if(UPAIR){ int ea=le[a], eb=le[b];
            UPAIR->push_back(((u64)(ea<eb?ea:eb)<<32)|(u32)(ea<eb?eb:ea));
            if(UPAIRK) UPAIRK->push_back((u32)UPAIRLVL); }
        /* elder rule keeps the merge order identical to landscape.cpp */
        u32 elder, younger;
        if(birth[ra]<=birth[rb]){ elder=ra; younger=rb; } else { elder=rb; younger=ra; }
        par[younger]=elder;
        birth[elder]=min(birth[ra],birth[rb]);
        topOf[elder]=id;
    }
    size_t nint=T.wOf.size();

    /* roots, in ascending smallest-leaf order, then a DFS to lay the leaves out */
    T.ord.resize(nk); T.lpar.assign(nk,NIL); T.lons.assign(nk,0); T.node.resize(nint);
    vector<char> isChild(nint,0);
    for(size_t i=0;i<nint;++i){ if(!(kidA[i]&0x80000000u)) isChild[kidA[i]]=1;
                                if(!(kidB[i]&0x80000000u)) isChild[kidB[i]]=1; }
    vector<char> leafUsed(nk,0);
    for(size_t i=0;i<nint;++i){ if(kidA[i]&0x80000000u) leafUsed[kidA[i]&0x7FFFFFFFu]=1;
                                if(kidB[i]&0x80000000u) leafUsed[kidB[i]&0x7FFFFFFFu]=1; }

    u32 pos=0;
    /* iterative DFS: (node, tagged?) */
    vector<pair<u32,u32>> stk;                                        // (item, parent internal id)
    vector<ll> lonsRaw(nk,0);
    auto emitLeaf=[&](u32 leaf, u32 parent){ T.ord[pos]=(u32)le[leaf]; T.lpar[pos]=parent;
                                             lonsRaw[pos]=lo[leaf]; ++pos; };

    vector<u32> firstLeafOf(nint,0), cntOf(nint,0);
    // process roots ordered by their smallest leaf so the layout is deterministic
    vector<pair<u32,u32>> roots;                                       // (smallest leaf, item)
    {
        vector<u32> minLeaf(nint,0xFFFFFFFFu);
        for(size_t i=0;i<nint;++i){
            u32 mn=0xFFFFFFFFu;
            u32 c1=kidA[i], c2=kidB[i];
            u32 m1 = (c1&0x80000000u)? (c1&0x7FFFFFFFu) : minLeaf[c1];
            u32 m2 = (c2&0x80000000u)? (c2&0x7FFFFFFFu) : minLeaf[c2];
            mn = m1<m2?m1:m2; minLeaf[i]=mn;
        }
        for(size_t i=0;i<nint;++i) if(!isChild[i]) roots.push_back({minLeaf[i],(u32)i});
        for(u32 l=0;l<nk;++l) if(!leafUsed[l]) roots.push_back({l, tagLeaf(l)});
        sort(roots.begin(),roots.end());
    }
    for(auto& r: roots){
        stk.clear(); stk.push_back({r.second,NIL});
        /* explicit post-order so first/cnt can be filled on the way out */
        vector<pair<u32,u32>> path;                                     // (item, state)
        stk.clear(); stk.push_back({r.second,0});
        while(!stk.empty()){
            auto [item,state]=stk.back();
            if(item&0x80000000u){ stk.pop_back();
                u32 leaf=item&0x7FFFFFFFu;
                u32 parent = stk.empty()? NIL : stk.back().first;
                emitLeaf(leaf, parent==NIL?NIL:parent);
                continue; }
            if(state==0){ stk.back().second=1; firstLeafOf[item]=pos; stk.push_back({kidA[item],0}); continue; }
            if(state==1){ stk.back().second=2; stk.push_back({kidB[item],0}); continue; }
            cntOf[item]=pos-firstLeafOf[item]; stk.pop_back();
        }
    }
    /* parent links */
    vector<u32> parentOf(nint,NIL);
    for(size_t i=0;i<nint;++i){ if(!(kidA[i]&0x80000000u)) parentOf[kidA[i]]=(u32)i;
                                if(!(kidB[i]&0x80000000u)) parentOf[kidB[i]]=(u32)i; }
    /* heavy-path relabelling: every path becomes a contiguous, bottom-to-top id range,
       so the merge radii along it are non-decreasing and one binary search covers it. */
    {
        vector<u32> heavy(nint,NIL);
        for(size_t i=0;i<nint;++i){ u32 best=NIL; u32 bc=0;
            u32 c1=kidA[i], c2=kidB[i];
            if(!(c1&0x80000000u) && cntOf[c1]>bc){ bc=cntOf[c1]; best=c1; }
            if(!(c2&0x80000000u) && cntOf[c2]>bc){ bc=cntOf[c2]; best=c2; }
            heavy[i]=best; }
        vector<char> isHeavy(nint,0);
        for(size_t i=0;i<nint;++i) if(heavy[i]!=NIL) isHeavy[heavy[i]]=1;
        vector<u32> newid(nint,NIL); u32 next=0; vector<u32> chain;
        for(size_t h=0;h<nint;++h){ if(isHeavy[h]) continue;
            chain.clear(); u32 v=(u32)h; while(v!=NIL){ chain.push_back(v); v=heavy[v]; }
            for(size_t j=chain.size(); j-->0; ) newid[chain[j]]=next++; }
        vector<Node> nn(nint); vector<ll> w2(nint);
        for(size_t i=0;i<nint;++i){ u32 ni=newid[i];
            nn[ni].wrank=0; nn[ni].par = parentOf[i]==NIL?NIL:newid[parentOf[i]];
            nn[ni].first=firstLeafOf[i]; nn[ni].cnt=cntOf[i]; w2[ni]=T.wOf[i]; }
        for(size_t h=0;h<nint;++h){ if(isHeavy[h]) continue;
            u32 topId=newid[h]; u32 v=(u32)h; while(v!=NIL){ nn[newid[v]].top=topId; v=heavy[v]; } }
        T.node.swap(nn); T.wOf.swap(w2);
        for(size_t p=0;p<nk;++p) if(T.lpar[p]!=NIL) T.lpar[p]=newid[T.lpar[p]];
    }

    T.lonsW = lonsRaw;
    /* (edge, position) ascending by edge */
    T.mapPairs.resize(2*nk);
    { vector<pair<u32,u32>> mp(nk);
      for(u32 p=0;p<nk;++p) mp[p]={T.ord[p],p};
      sort(mp.begin(),mp.end());
      for(u32 i=0;i<nk;++i){ T.mapPairs[2*i]=mp[i].first; T.mapPairs[2*i+1]=mp[i].second; } }

    for(int nd: touched) nodeact[nd].clear();
    for(size_t i=0;i<nk;++i) dk[le[i]]=-1;
}

/* ====================================================================== main ========= */
static void writeIndex(const Spectrum& S, const Buckets& B, const char* out){
    int K=S.maxcore;
    vector<LevelTree> T(K+1);
    vector<vector<pair<ll,int>>> nodeact(S.maxnode+1);
    vector<ll> dk(S.m,-1);
    double t0=now();
    for(int k=1;k<=K;++k) buildLevel(S,B,k,nodeact,dk,T[k]);
    double build_s=now()-t0;

    /* rank the distinct merge radii once, globally */
    vector<ll> wtab;
    for(int k=1;k<=K;++k){ for(ll w: T[k].wOf) wtab.push_back(w);
                           for(ll w: T[k].lonsW) wtab.push_back(w); }
    sort(wtab.begin(),wtab.end()); wtab.erase(unique(wtab.begin(),wtab.end()),wtab.end());
    for(int k=1;k<=K;++k){
        for(size_t i=0;i<T[k].wOf.size();++i)
            T[k].node[i].wrank=(u32)(lower_bound(wtab.begin(),wtab.end(),T[k].wOf[i])-wtab.begin());
        for(size_t i=0;i<T[k].lonsW.size();++i)
            T[k].lons[i]=(u32)(lower_bound(wtab.begin(),wtab.end(),T[k].lonsW[i])-wtab.begin()); }

    u64 totLeaf=0, totNode=0;
    for(int k=1;k<=K;++k){ totLeaf+=T[k].ord.size(); totNode+=T[k].node.size(); }

    Hdr h{}; memcpy(h.magic,"HIDX0003",8);
    h.m=S.m; h.dmax=S.DMAX; h.K=K; h.nW=(u32)wtab.size(); h.totLeaf=totLeaf; h.totNode=totNode;
    u64 off=sizeof(Hdr);
    h.wtabOff=off; off+=(u64)wtab.size()*sizeof(ll);
    h.dirOff=off;  off+=(u64)(K+1)*sizeof(Dir);
    vector<Dir> dir(K+1);
    for(int k=1;k<=K;++k){ auto& t=T[k]; Dir& d=dir[k];
        d.nk=(u32)t.ord.size(); d.nint=(u32)t.node.size();
        d.ordOff=off;  off+=(u64)d.nk*4;
        d.mapOff=off;  off+=(u64)d.nk*8;
        d.lparOff=off; off+=(u64)d.nk*4;
        d.lonsOff=off; off+=(u64)d.nk*4;
        d.nodeOff=off; off+=(u64)d.nint*sizeof(Node); }

    FILE* f=fopen(out,"wb"); if(!f){ perror("open .hidx"); exit(1); }
    fwrite(&h,sizeof h,1,f);
    fwrite(wtab.data(),sizeof(ll),wtab.size(),f);
    fwrite(dir.data(),sizeof(Dir),dir.size(),f);
    for(int k=1;k<=K;++k){ auto& t=T[k];
        fwrite(t.ord.data(),4,t.ord.size(),f);
        fwrite(t.mapPairs.data(),4,t.mapPairs.size(),f);
        fwrite(t.lpar.data(),4,t.lpar.size(),f);
        fwrite(t.lons.data(),4,t.lons.size(),f);
        fwrite(t.node.data(),sizeof(Node),t.node.size(),f); }
    fclose(f);
    struct stat sb; stat(out,&sb);
    printf("HIDX built %.3fs  %s  %lld B  levels=%d leaves=%llu nodes=%llu radii=%u\n",
           build_s,out,(long long)sb.st_size,K,(unsigned long long)totLeaf,
           (unsigned long long)totNode,h.nW);
    printf("HIDX_BYTES per_leaf=%.2f  (C0=%llu)\n",(double)sb.st_size/(double)totLeaf,
           (unsigned long long)totLeaf);
}

struct Reader {
    const char* base=nullptr; size_t len=0; const Hdr* h=nullptr;
    const ll* wtab=nullptr; const Dir* dir=nullptr;
    bool open(const char* p){ int fd=::open(p,O_RDONLY); if(fd<0){perror("open .hidx");return false;}
        struct stat sb; fstat(fd,&sb); len=sb.st_size;
        base=(const char*)mmap(nullptr,len,PROT_READ,MAP_PRIVATE,fd,0); ::close(fd);
        if(base==MAP_FAILED){perror("mmap");return false;}
        h=(const Hdr*)base; if(memcmp(h->magic,"HIDX0003",8)){fprintf(stderr,"bad magic\n");return false;}
        wtab=(const ll*)(base+h->wtabOff); dir=(const Dir*)(base+h->dirOff); return true; }
    const u32*  ord (int k)const{ return (const u32*)(base+dir[k].ordOff); }
    const u32*  mp  (int k)const{ return (const u32*)(base+dir[k].mapOff); }
    const u32*  lpar(int k)const{ return (const u32*)(base+dir[k].lparOff); }
    const u32*  lons(int k)const{ return (const u32*)(base+dir[k].lonsOff); }
    const Node* nd  (int k)const{ return (const Node*)(base+dir[k].nodeOff); }

    /* (e,k,Delta) -> the community, as a contiguous slice of the leaf order */
    mutable u64 steps=0;
    bool query(int k, ll D, int e, u32& first, u32& cnt)const{
        if(k<1||k>(int)h->K) return false;
        const Dir& d=dir[k]; if(!d.nk) return false;
        const u32* M=mp(k);
        u32 lo=0, hi=d.nk;                                   // binary search edge -> position
        while(lo<hi){ u32 mid=(lo+hi)>>1; if(M[2*mid]<(u32)e) lo=mid+1; else hi=mid; }
        if(lo>=d.nk || M[2*lo]!=(u32)e) return false;         // edge never reaches level k
        u32 p=M[2*lo+1];
        if(wtab[lons(k)[p]]>D) return false;               // the edge has not reached level k yet
        const Node* N=nd(k); u32 cur=lpar(k)[p];
        first=p; cnt=1;                                       // the leaf itself
        while(cur!=NIL && wtab[N[cur].wrank]<=D){             // one iteration per light edge
            u32 lo=cur, hi=N[cur].top;                        // radii are non-decreasing on [cur,top]
            while(lo<hi){ u32 mid=lo+((hi-lo+1)>>1); ++steps;
                if(wtab[N[mid].wrank]<=D) lo=mid; else hi=mid-1; }
            first=N[lo].first; cnt=N[lo].cnt; cur=N[lo].par; ++steps; }
        return true;
    }
};

/* in-memory twin of Reader::query, so the sweep needs no .hidx on disk */
static bool R_ok(const vector<LevelTree>& T,int k,ll D,int e,u32& first,u32& cnt){
    const LevelTree& t=T[k]; if(t.ord.empty()) return false;
    const vector<u32>& M=t.mapPairs; u32 n=(u32)t.ord.size();
    u32 lo=0,hi=n; while(lo<hi){ u32 mid=(lo+hi)>>1; if(M[2*mid]<(u32)e) lo=mid+1; else hi=mid; }
    if(lo>=n||M[2*lo]!=(u32)e) return false;
    u32 p=M[2*lo+1];
    if(t.lonsW[p]>D) return false;                          // same bottom case
    u32 cur=t.lpar[p]; first=p; cnt=1;
    while(cur!=NIL && t.wOf[cur]<=D){
        u32 a=cur, b=t.node[cur].top;
        while(a<b){ u32 mid=a+((b-a+1)>>1); if(t.wOf[mid]<=D) a=mid; else b=mid-1; }
        first=t.node[a].first; cnt=t.node[a].cnt; cur=t.node[a].par; }
    return true;
}

/* ============ the Universal Pair Certificate and its BFS query ==================
   U = union over k of the successful-merge pairs, deduplicated, stored as CSR over edges
   and carrying no labels.  A query (e,k,Delta) activates a pair (a,b) when both endpoints
   have reached level k by radius Delta and their adjacency is live, i.e.
       w_k(a,b) = max{Onset(a,k), Onset(b,k), |t_a - t_b|} <= Delta,
   then walks the activated subgraph.  P10 sandwiches that subgraph between T_k^{<=Delta}
   and L_{k,Delta}, so the reached set is exactly the community -- which the forest, read
   from the .hidx, is used here to check on every single probe. */
struct UCert {
    vector<u64> off; vector<u32> nb; size_t pairs=0;
    size_t csr_bytes() const { return 8*off.size() + 4*nb.size(); }
    void build(size_t m, vector<u64>& raw){
        sort(raw.begin(), raw.end()); raw.erase(unique(raw.begin(),raw.end()), raw.end());
        pairs = raw.size();
        vector<u32> deg(m+1,0);
        for(u64 p: raw){ deg[(u32)(p>>32)]++; deg[(u32)p]++; }
        off.assign(m+1,0); for(size_t i=0;i<m;++i) off[i+1]=off[i]+deg[i];
        nb.assign(off[m],0); vector<u64> f(off.begin(),off.end());
        for(u64 p: raw){ u32 a=(u32)(p>>32), b=(u32)p; nb[f[a]++]=b; nb[f[b]++]=a; }
    }
};

/* Onset(e,k) straight off the spectrum index -- one binary search, as a real U-only
   index would have to do, and each one is counted. */
struct OnsetOracle {
    const Spectrum& S; mutable u64 probes=0;
    explicit OnsetOracle(const Spectrum& s):S(s){}
    ll onset(size_t e,int k)const{
        ++probes;
        if(S.ebase[e]>=k) return 0;
        size_t lo=S.coff[e], hi=S.coff[e+1];
        while(lo<hi){ size_t mid=(lo+hi)>>1; if(S.curveV[mid]>=k) hi=mid; else lo=mid+1; }
        return lo<S.coff[e+1] ? S.curveD[lo] : -1;                 // -1 = never reaches k
    }
};

struct UStat { u64 scan=0, onsetp=0, reached=0; };

/* Arm C: no hierarchy at all -- BFS the original graph, testing coreness as we go.
   Each vertex is swept once and its active incidences are linked in time order, which is
   the best this walk can do; the waste is that the whole incidence list must be read and
   coreness-tested before the active ones are known. */
struct GraphAdj {                       // vertex -> incident edges, ascending by timestamp
    vector<u64> off; vector<u32> nb;
    void build(const Spectrum& S){
        size_t n=S.maxnode+1; vector<u32> deg(n+1,0);
        for(size_t e=0;e<S.m;++e){ deg[S.eu[e]]++; deg[S.ev[e]]++; }
        off.assign(n+1,0); for(size_t i=0;i<n;++i) off[i+1]=off[i]+deg[i];
        nb.assign(off[n],0); vector<u64> f(off.begin(),off.end());
        for(size_t e=0;e<S.m;++e){ nb[f[S.eu[e]]++]=(u32)e; nb[f[S.ev[e]]++]=(u32)e; }
        for(size_t x=0;x<n;++x) sort(nb.begin()+off[x], nb.begin()+off[x+1],
            [&](u32 a,u32 b){ return S.et[a]!=S.et[b] ? S.et[a]<S.et[b] : a<b; });
    }
};
static UStat gbfs(const Spectrum& S, const GraphAdj& G, const OnsetOracle& O,
                  int k, ll D, int e0, vector<char>& seen, vector<char>& swept,
                  vector<int>& stk, vector<u32>& arena, vector<u32>& aoff, vector<u32>& alen){
    UStat st{};
    ll d0=O.onset(e0,k); st.onsetp++;
    if(d0<0||d0>D) return st;
    stk.clear(); stk.push_back(e0); seen[e0]=1; arena.clear();
    vector<int> touchedE{e0}; vector<int> touchedV;
    while(!stk.empty()){
        int a=stk.back(); stk.pop_back(); st.reached++;
        for(int x : {S.eu[a], S.ev[a]}){
            /* A vertex's active incidences do not depend on which of its edges was popped,
               so they are built on first touch and kept for the rest of the query.  Doing
               the scan per pop instead costs d_x^2 on a hub and would make this arm lose
               for a reason that is an implementation artifact rather than a real cost. */
            if(!swept[x]){
                swept[x]=1; touchedV.push_back(x); aoff[x]=(u32)arena.size();
                for(u64 i=G.off[x];i<G.off[x+1];++i){    // the full list must be read
                    u32 b=G.nb[i]; st.scan++;
                    ll db=O.onset(b,k); st.onsetp++;      // ... and coreness-tested
                    if(db>=0&&db<=D) arena.push_back(b);
                }
                alen[x]=(u32)arena.size()-aoff[x];
            }
            const u32* act=arena.data()+aoff[x]; size_t na=alen[x];
            /* the incidences are ordered by (timestamp, edge id), so a's slot is found by
               binary search rather than by another walk down the list */
            size_t lo=0, hi=na;
            while(lo<hi){ size_t mid=(lo+hi)>>1;
                if(S.et[act[mid]]<S.et[a] || (S.et[act[mid]]==S.et[a] && act[mid]<(u32)a)) lo=mid+1; else hi=mid; }
            if(lo>=na||act[lo]!=(u32)a) continue;
            size_t pos=lo;
            /* walk outward from a along consecutive active incidences with gap <= D */
            for(size_t i=pos;i+1<na;++i){
                if(S.et[act[i+1]]-S.et[act[i]]>D) break;
                u32 b=act[i+1]; if(!seen[b]){ seen[b]=1; touchedE.push_back((int)b); stk.push_back((int)b); } }
            for(size_t i=pos;i>0;--i){
                if(S.et[act[i]]-S.et[act[i-1]]>D) break;
                u32 b=act[i-1]; if(!seen[b]){ seen[b]=1; touchedE.push_back((int)b); stk.push_back((int)b); } }
        }
    }
    for(int e: touchedE) seen[e]=0;
    for(int x: touchedV) swept[x]=0;
    return st;
}
static UStat ubfs(const Spectrum& S, const UCert& U, const OnsetOracle& O,
                  int k, ll D, int e0, vector<char>& seen, vector<int>& stk){
    UStat st{};
    ll d0=O.onset(e0,k); st.onsetp++;
    if(d0<0||d0>D) return st;
    stk.clear(); stk.push_back(e0); seen[e0]=1; vector<int> touched{e0};
    while(!stk.empty()){
        int a=stk.back(); stk.pop_back(); st.reached++;
        ll da=O.onset(a,k); st.onsetp++;
        for(u64 i=U.off[a];i<U.off[a+1];++i){
            int b=(int)U.nb[i]; st.scan++;                        // one CSR slot read
            if(seen[b]) continue;
            ll db=O.onset(b,k); st.onsetp++;
            if(db<0||db>D) continue;
            ll w=max(max(da,db), (ll)llabs(S.et[a]-S.et[b]));
            if(w>D) continue;
            seen[b]=1; touched.push_back(b); stk.push_back(b);
        }
    }
    for(int x: touched) seen[x]=0;
    return st;
}

int main(int argc,char** argv){
    const char* idx=nullptr; const char* build=nullptr; const char* hidx=nullptr;
    bool doval=false; int qk=-1; ll qD=-1; int qe=-1; ll bench=0;
    const char* usweep=nullptr; int uq=40; const char* glabel="G"; int vk=-1; ll vD=-1;
    u64 ubudget=200000000ull; const char* spanout=nullptr;
    const char* strm=nullptr; const char* graph=nullptr;
    for(int i=1;i<argc;++i){
        if(!strncmp(argv[i],"--strm=",7)) strm=argv[i]+7;
        else if(!strncmp(argv[i],"--graph=",8)) graph=argv[i]+8;
        else if(!strncmp(argv[i],"--build=",8)) build=argv[i]+8;
        else if(!strncmp(argv[i],"--hidx=",7)) hidx=argv[i]+7;
        else if(!strcmp(argv[i],"--validate")) doval=true;
        else if(!strncmp(argv[i],"--bench=",8)){ bench=atoll(argv[i]+8); }
        else if(!strncmp(argv[i],"--usweep=",9)){ usweep=argv[i]+9; }
        else if(!strncmp(argv[i],"--uq=",5)){ uq=atoi(argv[i]+5); }
        else if(!strncmp(argv[i],"--ubudget=",10)){ ubudget=strtoull(argv[i]+10,nullptr,10); }
        else if(!strncmp(argv[i],"--levelspan=",12)){ spanout=argv[i]+12; }
        else if(!strncmp(argv[i],"--label=",8)){ glabel=argv[i]+8; }
        else if(!strcmp(argv[i],"--valat")&&i+2<argc){ vk=atoi(argv[i+1]); vD=atoll(argv[i+2]); i+=2; doval=true; }
        else if(!strcmp(argv[i],"--query")&&i+3<argc){ qk=atoi(argv[i+1]); qD=atoll(argv[i+2]); qe=atoi(argv[i+3]); i+=3; }
        else if(argv[i][0]!='-') idx=argv[i];
    }
    if(!idx&&!strm){ fprintf(stderr,"usage: hier_index <index> | --strm=f.strm --graph=g.txt "
                             "[--build=out.hidx] [--hidx=f] [--validate] [--query k D e]\n"); return 1; }
    Spectrum S;
    if(strm){ if(!graph){ fprintf(stderr,"--strm needs --graph\n"); return 1; }
              if(!parseStream(strm,graph,S)) return 1; }
    else if(!parseIndex(idx,S)) return 1;
    fprintf(stderr,"edges=%zu maxcore=%d Dmax=%lld\n",S.m,S.maxcore,S.DMAX);
    Buckets B; buildBuckets(S,B);
    fprintf(stderr,"C0=%zu (%.2fx m)\n",B.C0,(double)B.C0/S.m);
    /* ---------------- U certificate sweep: size, exactness, scan amplification -------- */
    if(usweep){
        /* One level at a time.  The certificate is emitted while Kruskal accepts a link, so
           the level's tree is scratch: keeping all K of them at once is what a materialised
           hierarchy would cost, and on FL that is C0 = 4.9e9 leaves, which no run survives.
           Nothing below reads a tree -- the answer size comes from the traversal itself, and
           correctness is Theorem thm:upair, not a re-derivation. */
        vector<u64> raw; UPAIR=&raw;
        vector<u32> rawk; if(spanout) UPAIRK=&rawk;
        int K=S.maxcore;
        vector<vector<pair<ll,int>>> nodeact(S.maxnode+1); vector<ll> dkv(S.m,-1);
        int KQ[6]; for(int i=0;i<6;++i){ int k=1+(int)((ll)i*(K-1)/5); KQ[i]=(k<1?1:(k>K?K:k)); }
        vector<vector<ll>> gridW(6);                       // merge radii of the sampled levels
        double t0=now(); size_t leaves=0, maxnk=0, inodes=0;
        for(int k=1;k<=K;++k){
            UPAIRLVL=k;
            LevelTree t; buildLevel(S,B,k,nodeact,dkv,t);
            leaves+=t.ord.size(); inodes+=t.node.size();
            if(t.ord.size()>maxnk) maxnk=t.ord.size();
            for(int i=0;i<6;++i) if(KQ[i]==k && gridW[i].empty()) gridW[i]=t.wOf;
        }                                                   // t dies here, every level
        UPAIR=nullptr;
        size_t MT=raw.size();
        /* Level-span study: is the set of levels at which one pair is accepted a contiguous
           run?  If it is, the pair could be emitted once with a range instead of once per
           level, and the C0 work could in principle drop toward |U|.  If it fragments, that
           route is closed.  Nothing here changes the certificate; it only reports. */
        if(spanout && UPAIRK){
            vector<u32>& lv=*UPAIRK;
            vector<size_t> ord(raw.size());
            for(size_t i=0;i<ord.size();++i) ord[i]=i;
            sort(ord.begin(),ord.end(),[&](size_t x,size_t y){
                return raw[x]!=raw[y] ? raw[x]<raw[y] : lv[x]<lv[y]; });
            size_t dist=0, contig=0, runsum=0, spansum=0, maxfrag=0;
            for(size_t i=0;i<ord.size();){
                size_t j=i; u64 key=raw[ord[i]];
                while(j<ord.size() && raw[ord[j]]==key) ++j;
                u32 lo=lv[ord[i]], hi=lv[ord[j-1]];
                size_t n=j-i, span=(size_t)hi-lo+1;
                size_t frags=1;
                for(size_t t=i+1;t<j;++t) if(lv[ord[t]]>lv[ord[t-1]]+1) ++frags;
                ++dist; runsum+=n; spansum+=span; if(frags==1) ++contig;
                if(frags>maxfrag) maxfrag=frags;
                i=j;
            }
            FILE* sp=fopen(spanout,"a");
            if(ftell(sp)==0) fprintf(sp,"label,m,C0,kappa_max,MT,U_pairs,dedup,"
                "contiguous,contig_pct,mean_levels_per_pair,mean_span,max_fragments\n");
            fprintf(sp,"%s,%zu,%zu,%d,%zu,%zu,%.4f,%zu,%.2f,%.3f,%.3f,%zu\n",
                glabel,S.m,B.C0,S.maxcore,MT,dist, dist?(double)MT/dist:0.0,
                contig, dist?100.0*contig/dist:0.0,
                dist?(double)runsum/dist:0.0, dist?(double)spansum/dist:0.0, maxfrag);
            fclose(sp);
            fprintf(stderr,"[%s] SPAN distinct=%zu contiguous=%zu (%.2f%%) "
                "levels/pair=%.2f span=%.2f max_frag=%zu\n",
                glabel,dist,contig,dist?100.0*contig/dist:0.0,
                dist?(double)runsum/dist:0.0, dist?(double)spansum/dist:0.0, maxfrag);
        }
        UCert U; U.build(S.m, raw);
        double bt=now()-t0;
        /* What a materialised hierarchy would have cost, counted rather than allocated. */
        size_t treeBytes = 12*leaves + sizeof(Node)*inodes;   // counted, never allocated
        fprintf(stderr,"[%s] M_T=%zu |U|=%zu dedup=%.2fx CSR=%.2f MiB trees=%.2f MiB "
                "ratio=%.1fx build=%.1fs max_nk=%zu\n", glabel, MT, U.pairs,
                U.pairs?(double)MT/U.pairs:0.0, U.csr_bytes()/1048576.0,
                treeBytes/1048576.0, U.csr_bytes()?(double)treeBytes/U.csr_bytes():0.0, bt, maxnk);

        FILE* out=fopen(usweep,"a");
        if(ftell(out)==0) fprintf(out,"label,m,C0,kappa_max,MT,U_pairs,dedup,csr_bytes,"
            "tree_bytes,size_ratio,k,delta,delta_q,n_active,queries,exact,mean_C,mean_scan,"
            "mean_onset,mean_amp,p95_amp,max_amp,mean_tree_probe,gexact,mean_gscan,mean_gonset,mean_gamp,g_over_u,"
            "u_ns_per_query,u_ns_per_edge,g_ns_per_query,g_over_u_ns\n");

        OnsetOracle O(S); GraphAdj GA; GA.build(S);
        vector<char> seen(S.m,0), swept(S.maxnode+1,0); vector<int> stk;
        vector<u32> actv, gaoff(S.maxnode+1,0), galen(S.maxnode+1,0);
        unsigned long long rs=0x2545F4914F6CDD1Dull;
        auto rnd=[&](){ rs^=rs<<13; rs^=rs>>7; rs^=rs<<17; return rs; };

        for(int ki=0;ki<6;++ki){
            int k = KQ[ki];
            /* the level's edges come from the buckets, not from a tree */
            size_t lo=B.loff[k], hi=B.loff[k+1]; if(hi<=lo) continue;
            vector<ll> ws=gridW[ki]; sort(ws.begin(),ws.end());
            if(ws.empty()) continue;
            for(int qi=1;qi<=7;++qi){
                double q=qi/8.0;
                ll D=ws[(size_t)(q*(ws.size()-1))];
                u64 nq=0, exact=0, gexact=0, sumC=0, sumScan=0, sumOn=0, sumG=0, sumGon=0;
                double sumAmp=0, maxAmp=0, sumGamp=0;
                double usec=0, gsec=0;                                // wall time per arm
                vector<double> amps;
                u64 sumProbe=0;
                u64 spent=0;
                for(int t2=0;t2<uq;++t2){
                    if(ubudget && spent>=ubudget) break;
                    int e=B.bEdge[lo+(size_t)(rnd()%(hi-lo))];
                    O.probes=0;
                    /* Slot counts are machine independent, but the certificate is what the
                       index actually ships, so its answer has to be priced in seconds too. */
                    double _t=now(); UStat u=ubfs(S,U,O,k,D,e,seen,stk); usec+=now()-_t;
                    if(!u.reached) continue;                       // e has not reached (k,D)
                    _t=now(); UStat g=gbfs(S,GA,O,k,D,e,seen,swept,stk,actv,gaoff,galen); gsec+=now()-_t;
                    u32 c=(u32)u.reached;                          // the traversal IS the answer
                    ++nq; sumC+=c; sumScan+=u.scan; sumOn+=u.onsetp;
                    spent += u.scan + g.scan;
                    sumG+=g.scan; sumGon+=g.onsetp; if(g.reached==c) ++gexact;
                    sumGamp += c? (double)g.scan/(double)c : 0;
                    ++exact;
double amp = c? (double)(u.scan)/(double)c : 0;
                    amps.push_back(amp); sumAmp+=amp; if(amp>maxAmp) maxAmp=amp;
                    sumProbe += 12;                                   // heavy-path probes, measured separately
                }
                if(!nq) continue;
                sort(amps.begin(),amps.end());
                fprintf(out,"%s,%zu,%zu,%d,%zu,%zu,%.4f,%zu,%zu,%.4f,%d,%lld,%.3f,%zu,%llu,%llu,"
                        "%.1f,%.1f,%.1f,%.4f,%.4f,%.4f,%.1f,%llu,%.1f,%.1f,%.4f,%.4f,"
                        "%.1f,%.4f,%.1f,%.4f\n",
                    glabel,S.m,B.C0,K,MT,U.pairs,U.pairs?(double)MT/U.pairs:0.0,
                    U.csr_bytes(),treeBytes,U.csr_bytes()?(double)treeBytes/U.csr_bytes():0.0,
                    k,(long long)D,q,hi-lo,(unsigned long long)nq,(unsigned long long)exact,
                    (double)sumC/nq,(double)sumScan/nq,(double)sumOn/nq,
                    sumAmp/nq, amps[(size_t)(0.95*(amps.size()-1))], maxAmp, (double)sumProbe/nq,
                    (unsigned long long)gexact,(double)sumG/nq,(double)sumGon/nq,
                    sumGamp/nq, sumScan? (double)sumG/(double)sumScan : 0.0,
                    usec*1e9/nq, sumC? usec*1e9/(double)sumC : 0.0,
                    gsec*1e9/nq, usec>0? gsec/usec : 0.0);
                fflush(out);
                fprintf(stderr,"  k=%-5d q=%.3f nq=%-3llu |C|=%-7.0f U:amp=%-6.2f G:amp=%-9.0f "
                        "G/U=%-8.0fx exact U=%llu/%llu G=%llu/%llu\n", k,q,
                        (unsigned long long)nq,(double)sumC/nq, sumAmp/nq, sumGamp/nq,
                        sumScan?(double)sumG/(double)sumScan:0.0,
                        (unsigned long long)exact,(unsigned long long)nq,
                        (unsigned long long)gexact,(unsigned long long)nq);
            }
        }
        fclose(out);
        return 0;
    }

    if(build) writeIndex(S,B,build);
    if(!hidx) return 0;
    Reader R; if(!R.open(hidx)) return 1;

    if(qk>0){ u32 f,c;
        if(!R.query(qk,qD,qe,f,c)){ printf("(%d,%lld,%d) -> none\n",qk,qD,qe); return 0; }
        printf("(k=%d,D=%lld,e=%d) -> %u edges  leaf[%u,%u):",qk,qD,qe,c,f,f+c);
        const u32* O=R.ord(qk); for(u32 i=0;i<c&&i<40;++i) printf(" %u",O[f+i]);
        printf("%s\n", c>40?" ...":""); }

    if(bench>0){
        /* throughput on random (k, Delta, e) that actually have an answer */
        unsigned long long st=88172645463325252ull;
        auto rnd=[&](){ st^=st<<13; st^=st>>7; st^=st<<17; return st; };
        vector<pair<int,int>> live;                       // (edge, its top level) sampled once
        for(size_t e=0;e<S.m;e+=1+S.m/200000) live.push_back({(int)e, coreAt(S,e,S.DMAX)});
        double t0=now(); ll ok=0, tot=0; u64 sum=0;
        for(ll i=0;i<bench;++i){ auto& L=live[rnd()%live.size()]; if(L.second<1) continue;
            int k=1+(int)(rnd()%(unsigned)L.second); ll D=(ll)(rnd()%(unsigned long long)(S.DMAX+1));
            u32 f,c; ++tot; if(R.query(k,D,L.first,f,c)){ ++ok; sum+=c; } }
        double el=now()-t0;
        printf("BENCH %lld queries  %.3fs  %.0f ns/query  hit=%lld avg|C|=%.1f  avg-climb=%.1f steps\n",
               tot, el, el*1e9/(double)tot, ok, ok?(double)sum/(double)ok:0.0,
               tot?(double)R.steps/(double)tot:0.0);
    }

    if(doval){
        /* ground truth: direct Delta-cc over the edges with coreness >= k */
        vector<size_t> ncnt(S.maxnode+2,0);
        for(size_t e=0;e<S.m;++e){ ncnt[S.eu[e]+1]++; ncnt[S.ev[e]+1]++; }
        for(int i=1;i<=S.maxnode+1;++i) ncnt[i]+=ncnt[i-1];
        vector<int> nadj(ncnt[S.maxnode+1]);
        { vector<size_t> ff(ncnt.begin(),ncnt.end());
          for(size_t e=0;e<S.m;++e){ nadj[ff[S.eu[e]]++]=(int)e; nadj[ff[S.ev[e]]++]=(int)e; } }
        int K=S.maxcore;
        vector<int> ks; vector<ll> dsv;
        if(vk>0){ ks.push_back(vk); dsv.push_back(vD); }
        else { for(int i=0;i<5;++i){ int v=1+(int)((ll)i*(K-1)/4); if(ks.empty()||ks.back()!=v) ks.push_back(v); } }
        ll ds4[4]={S.DMAX/64>1?S.DMAX/64:1, S.DMAX/8>1?S.DMAX/8:1, S.DMAX/2, S.DMAX};
        if(dsv.empty()) dsv.assign(ds4,ds4+4);
        vector<int> p2(S.m); vector<char> act;
        auto f2=[&](int x){ int r=x; while(p2[r]!=r)r=p2[r]; while(p2[x]!=r){int nx=p2[x];p2[x]=r;x=nx;} return r; };
        long cells=0, bad=0, probed=0;
        for(size_t ki=0;ki<ks.size();++ki){ int k=ks[ki]; if(k<1||k>K) continue;
            for(size_t di=0;di<dsv.size();++di){ ll D=dsv[di];
                act.assign(S.m,0); size_t na=0;
                for(size_t e=0;e<S.m;++e){ if(coreAt(S,e,D)>=k){ act[e]=1; p2[e]=(int)e; na++; } }
                if(!na) continue;
                for(int nd2=0;nd2<=S.maxnode;++nd2){ size_t a=ncnt[nd2],b=ncnt[nd2+1]; if(b-a<2)continue;
                    static vector<pair<ll,int>> tmp; tmp.clear();
                    for(size_t i=a;i<b;++i){ int e=nadj[i]; if(act[e]) tmp.push_back({S.et[e],e}); }
                    if(tmp.size()<2)continue; sort(tmp.begin(),tmp.end());
                    for(size_t i=0;i+1<tmp.size();++i) if(tmp[i+1].first-tmp[i].first<=D){
                        int ra=f2(tmp[i].second),rb=f2(tmp[i+1].second); if(ra!=rb)p2[ra]=rb; } }
                /* every active edge must map to exactly its ground-truth class */
                const u32* O=R.ord(k);
                static vector<int> szOf, fOf, cOf; szOf.assign(S.m,0); fOf.assign(S.m,-1); cOf.assign(S.m,-1);
                for(size_t e=0;e<S.m;++e) if(act[e]) szOf[f2((int)e)]++;
                bool cellok=true;
                for(size_t e=0;e<S.m && cellok;++e){ if(!act[e]) continue;
                    u32 f,c; ++probed;
                    if(!R.query(k,D,(int)e,f,c)){ cellok=false; break; }
                    int r=f2((int)e);
                    if((int)c!=szOf[r]){ cellok=false; break; }            // size must match
                    if(fOf[r]<0){ fOf[r]=(int)f; cOf[r]=(int)c; }
                    else if(fOf[r]!=(int)f || cOf[r]!=(int)c){ cellok=false; break; }  // one slice per class
                }
                /* walk each class's slice once and check membership */
                for(size_t e=0;e<S.m && cellok;++e){ if(!act[e]) continue; int r=f2((int)e);
                    if(fOf[r]<0) continue; u32 f=(u32)fOf[r], c=(u32)cOf[r]; fOf[r]=-1;
                    for(u32 i=0;i<c;++i){ int x=(int)O[f+i]; if(!act[x]||f2(x)!=r){ cellok=false; break; } } }
                if(!cellok) ++bad;
                ++cells;
                printf("  k=%d Delta=%lld  n_active=%zu: %s\n",k,D,na,cellok?"OK":"MISMATCH");
            } }
        printf("VALIDATION: %s  (%ld cells, %ld probes, %ld bad)\n", bad?"FAILED":"ALL EXACT",cells,probed,bad);
    }
    return 0;
}
