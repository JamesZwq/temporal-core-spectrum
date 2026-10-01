// community_build -- builds the community index of the paper: the merge pairs U.
//
// Input: the edge core index as a KCSSTRM2 stream (kcs --stream) and the graph it was
// built from.  For every k, the edges with core number at least k are sorted by timestamp
// at each endpoint; the sparse pair set links each edge to the nearest earlier and the
// nearest later edge at the endpoint whose onset is at most its own; Kruskal's algorithm
// over these pairs, by ascending weight max(onset, onset, gap), keeps the pairs that
// merge two communities (the merge set M_k).  U is the union of the merge sets over k,
// each pair stored once.
//
// The index is read sequentially (no mmap residency) into 6 bytes per breakpoint, and the
// pass of one k uses O(m) working space beyond the resident index and U.
//
//   community_build --strm=G.kcs.strm --graph=G.txt [--label=L] [--out=U.bin] [--maxk=K]
//
// stdout: one line  PAIRS,<label>,m=,B=,K=,Csum=,Mk_total=,U=,build_s=,rss_MB=
// --out writes U as little-endian pairs of 32-bit edge ids (a < b), edges numbered in
// timestamp order as in the stream.
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdint>
#include <climits>
#include <vector>
#include <algorithm>
#include <chrono>
#include <sys/mman.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <unordered_map>
#include <map>
#include <set>
#include <cmath>
#include <unordered_set>
/* the merge pairs are collected in an open-addressing set of 64-bit keys (a << 32 | b) */
struct U64Set { std::vector<uint64_t> t; uint64_t n=0, mask=0;
  void init(size_t cap){ size_t s=1; while(s<cap*2) s<<=1; t.assign(s, ~0ull); mask=s-1; n=0; }
  static uint64_t h(uint64_t x){ x^=x>>31; x*=0x9E3779B97F4A7C15ull; x^=x>>29; return x; }
  bool insert(uint64_t k){ if(n*2>=t.size()) grow(); uint64_t i=h(k)&mask; while(t[i]!=~0ull){ if(t[i]==k) return false; i=(i+1)&mask; } t[i]=k; ++n; return true; }
  void grow(){ std::vector<uint64_t> o; o.swap(t); t.assign(o.size()*2, ~0ull); mask=t.size()-1; n=0; for(uint64_t k: o) if(k!=~0ull) insert(k); }
};
using namespace std;
using ll = long long; using u32 = uint32_t; using u64 = uint64_t; using u16 = uint16_t;
static double now(){ return chrono::duration<double>(chrono::steady_clock::now()
                        .time_since_epoch()).count(); }

#if defined(__linux__)
static long rss_kb(){ FILE* f=fopen("/proc/self/statm","r"); if(!f) return -1;
    long sz=0,res=0; if(fscanf(f,"%ld %ld",&sz,&res)!=2) res=0; fclose(f);
    return res*(long)(sysconf(_SC_PAGESIZE)/1024); }
#else
#include <sys/resource.h>
static long rss_kb(){ struct rusage r; getrusage(RUSAGE_SELF,&r); return r.ru_maxrss/1024; }
#endif

struct Spectrum {
    vector<int> eu, ev, ebase, efin; vector<ll> et;
    vector<u32> curveD; vector<u16> curveV; vector<u64> coff;   /* fidx22: 6 bytes per breakpoint */
    ll DMAX=0; int maxnode=0, maxcore=0; size_t m=0, B=0;
};
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
        if(!*q2||*q2=='#'||*q2=='%'||*q2=='\r'||*q2=='\n') continue;
        char* e2=nullptr; ll b=strtoll(q2,&e2,10); if(e2==q2) continue;
        char* q3=e2; while(*q3==' '||*q3=='\t') ++q3;
        if(!*q3||*q3=='#'||*q3=='%'||*q3=='\r'||*q3=='\n') continue;
        char* e3=nullptr; ll t=strtoll(q3,&e3,10); if(e3==q3) continue;
        if(t<0) continue;
        if(a==b) continue;
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

/* fidx22: sequential reader over the .strm records (64 MB buffer), so no file page stays in the RSS */
struct SeqReader { int fd; vector<uint8_t> buf; size_t pos=0, len=0;
    SeqReader(int f, size_t cap=(size_t)1<<26):fd(f),buf(cap){}
    bool need(size_t n){ if(len-pos>=n) return true; if(pos){ memmove(buf.data(),buf.data()+pos,len-pos); len-=pos; pos=0; }
        while(len<n){ ssize_t r=::read(fd,buf.data()+len,buf.size()-len); if(r<=0) return false; len+=(size_t)r; } return true; }
    const uint8_t* take(size_t n){ if(!need(n)) return nullptr; const uint8_t* p=buf.data()+pos; pos+=n; return p; }
    size_t avail8(){ return (len-pos)/8; }
};
template<class F> static bool scanRecords(const char* strm, u64 boff, u64 groups, F&& f){
    int fd=::open(strm,O_RDONLY); if(fd<0){ perror("open .strm"); return false; }
    if(lseek(fd,(off_t)boff,SEEK_SET)<0){ perror("lseek .strm"); ::close(fd); return false; }
    SeqReader R(fd);
    for(u64 g=0;g<groups;++g){ const uint8_t* h=R.take(16); if(!h){ fprintf(stderr,"truncated .strm (group header)\n"); ::close(fd); return false; }
        ll d=g_i64(h); u64 c=g_u64(h+8);
        while(c){ if(!R.need(8)){ fprintf(stderr,"truncated .strm (records)\n"); ::close(fd); return false; }
            size_t n=R.avail8(); if(n>c) n=(size_t)c; const uint8_t* p=R.buf.data()+R.pos;
            for(size_t i=0;i<n;++i,p+=8) f(d,g_u32(p),g_u32(p+4));
            R.pos+=n*8; c-=n; } }
    ::close(fd); return true;
}
static bool parseStream(const char* strm, const char* graph, Spectrum& S){
    vector<GEdge> ge; int nc=0;
    if(!loadGraph(graph,ge,nc)) return false;
    int fd=::open(strm,O_RDONLY); if(fd<0){ perror("open .strm"); return false; }
    uint8_t hdr[96]; if(pread(fd,hdr,96,0)!=96){ fprintf(stderr,"short .strm header\n"); ::close(fd); return false; }
    if(memcmp(hdr,"KCSSTRM2",8)){ fprintf(stderr,"not a KCSSTRM2 file\n"); ::close(fd); return false; }
    u64 edges=g_u64(hdr+32), groups=g_u64(hdr+40), records=g_u64(hdr+48);
    ll  floor_=g_i64(hdr+56), dmax=g_i64(hdr+64);
    u64 soff=g_u64(hdr+72), boff=g_u64(hdr+88);
    if(edges!=ge.size()){ fprintf(stderr,"edge count mismatch: stream %llu vs graph %zu\n",(unsigned long long)edges,ge.size()); ::close(fd); return false; }
    if(dmax+1>(ll)0xFFFFFFFFll){ fprintf(stderr,"Dmax %lld does not fit the u32 breakpoint array\n",dmax); ::close(fd); return false; }
    S.m=edges; S.DMAX=dmax; S.maxnode=nc-1;
    S.eu.resize(edges); S.ev.resize(edges); S.et.resize(edges);
    for(size_t e=0;e<edges;++e){ S.eu[e]=ge[e].u; S.ev[e]=ge[e].v; S.et[e]=ge[e].t; }
    vector<GEdge>().swap(ge);
    vector<u32> top(edges);
    if(pread(fd,top.data(),4*edges,(off_t)soff)!=(ssize_t)(4*edges)){ fprintf(stderr,"short seed region\n"); ::close(fd); return false; }
    ::close(fd);
    for(size_t e=0;e<edges;++e){ u32 t=g_u32((const uint8_t*)&top[e]); top[e]=t; if(t>65535u){ fprintf(stderr,"core number %u does not fit u16\n",t); return false; } }
    /* pass 1: records per edge */
    vector<u32> cnt(edges,0);
    if(!scanRecords(strm,boff,groups,[&](ll d,u32 e,u32 vb){ (void)d; (void)vb; cnt[e]++; })) return false;
    S.coff.assign(edges+1,0);
    for(size_t e=0;e<edges;++e) S.coff[e+1]=S.coff[e]+cnt[e];
    S.B=S.coff[edges]; (void)records;
    if(S.B>(u64)0xFFFFFFFFull){ fprintf(stderr,"B=%zu does not fit the u32 cursor\n",S.B); return false; }
    /* pass 2: place each record at its final position; the stream lists an edge's records by decreasing Delta, the index keeps them increasing;
       the stored core number of a record is the value the edge holds just above the record's Delta = the previous record's value, or the top */
    S.curveD.assign(S.B,0); S.curveV.assign(S.B,0);
    vector<u32> seen(edges,0); vector<u16> prevV(edges);
    for(size_t e=0;e<edges;++e) prevV[e]=(u16)top[e];
    if(!scanRecords(strm,boff,groups,[&](ll d,u32 e,u32 vb){
        u64 pos=S.coff[e+1]-1-seen[e]; S.curveD[pos]=(u32)(d+1); S.curveV[pos]=prevV[e]; prevV[e]=(u16)vb; seen[e]++; })) return false;
    S.ebase.resize(edges); S.efin.resize(edges); S.maxcore=0;
    for(size_t e=0;e<edges;++e){ S.ebase[e]=seen[e]? (int)prevV[e] : (int)top[e]; S.efin[e]=(int)top[e]; if(S.efin[e]>S.maxcore) S.maxcore=S.efin[e]; }
    (void)floor_;
    return true;
}
struct Adj { vector<u64> off; vector<u32> nb; };
static void buildAdj(const Spectrum& S, Adj& A){
    size_t n=S.maxnode+1;
    vector<u32> deg(n+1,0);
    for(size_t e=0;e<S.m;++e){ deg[S.eu[e]]++; deg[S.ev[e]]++; }
    A.off.assign(n+1,0); for(size_t i=0;i<n;++i) A.off[i+1]=A.off[i]+deg[i];
    A.nb.assign(A.off[n],0);
    { vector<u64> f(A.off.begin(),A.off.end());
      for(size_t e=0;e<S.m;++e){ A.nb[f[S.eu[e]]++]=(u32)e; A.nb[f[S.ev[e]]++]=(u32)e; } }
    for(size_t x=0;x<n;++x)
        sort(A.nb.begin()+A.off[x], A.nb.begin()+A.off[x+1],
             [&](u32 a,u32 b){ return S.et[a]!=S.et[b] ? S.et[a]<S.et[b] : a<b; });
}

struct LLMap {
    vector<ll> key; vector<u32> val; vector<char> used; u64 mask=0;
    size_t n=0;
    void reset(){ n=0; if(key.empty()){ key.resize(1024); val.resize(1024); used.resize(1024); }
                  fill(used.begin(),used.end(),0); mask=key.size()-1; }
    void grow(){ vector<ll> ok(key); vector<char> ou(used); vector<u32> ov(val);
        size_t cap=key.size()*4;
        key.assign(cap,0); val.assign(cap,0); used.assign(cap,0); mask=cap-1; n=0;
        for(size_t i=0;i<ou.size();++i) if(ou[i]){ u64 j=hsh(ok[i])&mask;
            while(used[j]) j=(j+1)&mask; used[j]=1; key[j]=ok[i]; val[j]=ov[i]; ++n; } }
    static u64 hsh(ll x){ u64 z=(u64)x+0x9E3779B97F4A7C15ull;
        z=(z^(z>>30))*0xBF58476D1CE4E5B9ull; z=(z^(z>>27))*0x94D049BB133111EBull; return z^(z>>31); }
    bool insert(ll k){ if(2*(n+1) > key.size()) grow();
        u64 i=hsh(k)&mask;
        while(used[i]){ if(key[i]==k) return false; i=(i+1)&mask; }
        used[i]=1; key[i]=k; val[i]=0; ++n; return true; }
    void put(ll k,u32 v){ u64 i=hsh(k)&mask;
        while(used[i]){ if(key[i]==k){ val[i]=v; return; } i=(i+1)&mask; } }
    u32 get(ll k)const{ u64 i=hsh(k)&mask;
        while(used[i]){ if(key[i]==k) return val[i]; i=(i+1)&mask; } return 0; }
};

/* the accepted merges of one level, ascending weight, GLOBAL edge ids */
struct Merges { vector<ll> w; vector<u32> a, b; size_t cand=0;
    void clear(){ w.clear(); a.clear(); b.clear(); cand=0; } };

struct Scratch {
    vector<ll>  dk; vector<u32> lid, stamp; vector<u64> alive;
    vector<u32> rk2, rk3, act, stk, lft, touched, par, idx, rk; vector<ll> cw; vector<u32> ca, cb;
    vector<u64> bstart, at, pk, pk2; vector<ll> ws; vector<u32> csz; LLMap wm;
};

/* candidate scan + Kruskal exactly as hier_build.cpp; emits the merge list instead of a forest */
static void buildLevel(const Spectrum& S, const Adj& A, int k,
                       const vector<u32>& active, const vector<ll>& onset,
                       Scratch& sc, Merges& M){
    size_t nk=active.size(); M.clear();
    if(!nk) return;
    sc.touched.clear();
    for(size_t i=0;i<nk;++i){ u32 e=active[i]; sc.dk[e]=onset[i]; sc.lid[e]=(u32)i;
        sc.alive[e>>6] |= 1ull<<(e&63);
        for(int x : {S.eu[e], S.ev[e]})
            if(sc.stamp[x]!=(u32)k){ sc.stamp[x]=(u32)k; sc.touched.push_back((u32)x); } }
    sc.ca.clear(); sc.cb.clear(); sc.cw.clear();
    for(u32 x : sc.touched){
        u64 s0=A.off[x], s1=A.off[x+1]; if(s1-s0<2) continue;
        sc.act.clear();
        for(u64 i=s0;i<s1;++i){ u32 e=A.nb[i];
            if(sc.alive[e>>6]>>(e&63) & 1ull) sc.act.push_back(e); }
        size_t d=sc.act.size(); if(d<2) continue;
        sc.lft.assign(d,0xFFFFFFFFu);
        sc.stk.clear();
        for(size_t p=0;p<d;++p){
            while(!sc.stk.empty() && sc.dk[sc.act[sc.stk.back()]] > sc.dk[sc.act[p]]) sc.stk.pop_back();
            if(!sc.stk.empty()){ u32 li=sc.stk.back(); sc.lft[p]=li;
                u32 L=sc.act[li], R=sc.act[p];
                sc.ca.push_back(L); sc.cb.push_back(R);
                sc.cw.push_back(max(max(sc.dk[L],sc.dk[R]), S.et[R]-S.et[L])); }
            sc.stk.push_back((u32)p); }
        sc.stk.clear();
        for(size_t p=d;p-->0;){
            while(!sc.stk.empty() && sc.dk[sc.act[sc.stk.back()]] > sc.dk[sc.act[p]]) sc.stk.pop_back();
            if(!sc.stk.empty()){ u32 ri=sc.stk.back();
                if(sc.lft[ri]!=(u32)p){
                    u32 L=sc.act[p], R=sc.act[ri];
                    sc.ca.push_back(L); sc.cb.push_back(R);
                    sc.cw.push_back(max(max(sc.dk[L],sc.dk[R]), S.et[R]-S.et[L])); } }
            sc.stk.push_back((u32)p); }
    }
    size_t nc=sc.cw.size(); M.cand=nc;

    sc.idx.resize(nc);
    sc.wm.reset();
    sc.ws.clear();
    for(size_t i=0;i<nc;++i) if(sc.wm.insert(sc.cw[i])) sc.ws.push_back(sc.cw[i]);
    for(size_t i=0;i<nk;++i) if(sc.wm.insert(onset[i])) sc.ws.push_back(onset[i]);
    sort(sc.ws.begin(), sc.ws.end());
    for(size_t r=0;r<sc.ws.size();++r) sc.wm.put(sc.ws[r],(u32)r);
    { size_t nb=sc.ws.size(); sc.rk.resize(nc);
      for(size_t i=0;i<nc;++i) sc.rk[i]=sc.wm.get(sc.cw[i]);
      sc.bstart.assign(nb+1,0);
      for(size_t i=0;i<nc;++i) sc.bstart[sc.rk[i]+1]++;
      for(size_t b=0;b<nb;++b) sc.bstart[b+1]+=sc.bstart[b];
      sc.pk.resize(nc); sc.pk2.resize(nc); sc.rk2.resize(nc); sc.rk3.resize(nc);
      {
          int passes=0; for(u32 mx=(u32)(nb?nb-1:0); mx; mx>>=8) ++passes;
          if(passes<=1 || nc<(size_t)8e6){
              sc.at.assign(sc.bstart.begin(), sc.bstart.end());
              for(size_t i=0;i<nc;++i)
                  sc.pk[sc.at[sc.rk[i]]++] = ((u64)sc.lid[sc.ca[i]]<<32)|(u64)sc.lid[sc.cb[i]];
          } else {
              for(size_t i=0;i<nc;++i){
                  sc.pk[i]=((u64)sc.lid[sc.ca[i]]<<32)|(u64)sc.lid[sc.cb[i]];
                  sc.rk2[i]=sc.rk[i]; }
              for(int pass=0; pass<passes; ++pass){
                  int sh=8*pass; u64 cc[257]={0};
                  for(size_t i=0;i<nc;++i) cc[((sc.rk2[i]>>sh)&255)+1]++;
                  for(int t=0;t<256;++t) cc[t+1]+=cc[t];
                  for(size_t i=0;i<nc;++i){ u64 d=cc[(sc.rk2[i]>>sh)&255]++;
                      sc.pk2[d]=sc.pk[i]; sc.rk3[d]=sc.rk2[i]; }
                  sc.pk.swap(sc.pk2); sc.rk2.swap(sc.rk3);
              }
          }
      }
      for(size_t b=0;b<nb;++b){ u64 lo=sc.bstart[b], hi=sc.bstart[b+1];
          if(hi-lo>1) sort(sc.pk.begin()+lo, sc.pk.begin()+hi); } }

    sc.par.resize(nk); for(u32 i=0;i<nk;++i) sc.par[i]=i;
    sc.csz.assign(nk,1);
    auto findp=[&](u32 x){ while(sc.par[x]!=x){ sc.par[x]=sc.par[sc.par[x]]; x=sc.par[x]; } return x; };
    size_t bcur=0;
    for(size_t ii=0;ii<nc;++ii){
        while(bcur+1<sc.bstart.size() && sc.bstart[bcur+1]<=ii) ++bcur;
        u64 w=sc.pk[ii]; u32 a=(u32)(w>>32), b=(u32)w;
        u32 ra=findp(a), rb=findp(b); if(ra==rb) continue;
        M.w.push_back(sc.ws[bcur]); M.a.push_back(active[a]); M.b.push_back(active[b]);
        if(sc.csz[ra]<sc.csz[rb]) swap(ra,rb);
        sc.par[rb]=ra; sc.csz[ra]+=sc.csz[rb];
    }
    for(size_t i=0;i<nk;++i){ u32 e=active[i]; sc.dk[e]=-1; sc.alive[e>>6] &= ~(1ull<<(e&63)); }
}



int main(int argc,char** argv){
    const char* strm=nullptr; const char* graph=nullptr; const char* label="G"; const char* out=nullptr; int maxk=INT_MAX;
    for(int i=1;i<argc;++i){
        if(!strncmp(argv[i],"--strm=",7)) strm=argv[i]+7;
        else if(!strncmp(argv[i],"--graph=",8)) graph=argv[i]+8;
        else if(!strncmp(argv[i],"--label=",8)) label=argv[i]+8;
        else if(!strncmp(argv[i],"--out=",6)) out=argv[i]+6;
        else if(!strncmp(argv[i],"--maxk=",7)) maxk=atoi(argv[i]+7);
        else { fprintf(stderr,"unknown argument %s\n",argv[i]); return 1; }
    }
    if(!strm||!graph){ fprintf(stderr,"usage: community_build --strm=G.kcs.strm --graph=G.txt [--label=L] [--out=U.bin] [--maxk=K]\n"); return 1; }
    double t0=now(); Spectrum S;
    if(!parseStream(strm,graph,S)) return 1;
    int K=S.maxcore; if(maxk<K) K=maxk;
    fprintf(stderr,"[%s] edges=%zu B=%zu maxcore=%d Dmax=%lld parse=%.1fs rss=%ldMB\n",label,S.m,S.B,S.maxcore,S.DMAX,now()-t0,rss_kb()/1024);
    Adj A; buildAdj(S,A);
    vector<u32> active(S.m); for(u32 e=0;e<S.m;++e) active[e]=e;
    vector<u32> cur(S.m); for(size_t e=0;e<S.m;++e) cur[e]=(u32)S.coff[e];
    vector<ll> onset(S.m,0);
    Scratch sc; sc.dk.assign(S.m,-1); sc.lid.assign(S.m,0); sc.alive.assign(S.m/64+2,0); sc.stamp.assign(S.maxnode+2,0xFFFFFFFFu);
    Merges M; U64Set uset; uset.init(1u<<20); u64 Csum=0, mkTotal=0;
    double tb=now();
    for(int k=1;k<=K;++k){
        size_t w=0;
        for(size_t i=0;i<active.size();++i){ u32 e=active[i];
            if(S.efin[e]<k) continue;
            if(S.ebase[e]>=k) onset[w]=0;
            else { u64 c=cur[e]; while(c<S.coff[e+1] && S.curveV[c]<k) ++c; cur[e]=(u32)c; onset[w]=(c<S.coff[e+1])? (ll)S.curveD[c] : -1; }
            active[w++]=e; }
        active.resize(w); if(onset.size()<w) onset.resize(w);
        if(!w) break;
        buildLevel(S,A,k,active,onset,sc,M);
        Csum+=w;
        for(size_t i=0;i<M.w.size();++i){ u32 a=M.a[i], b=M.b[i]; if(a>b) swap(a,b); uset.insert(((u64)a<<32)|(u64)b); }
        mkTotal+=M.w.size();
        if(k%25==0||k==K) fprintf(stderr,"[%s] k=%d/%d nk=%zu Mk=%zu U=%llu | %.0fs rss=%ldMB\n",label,k,K,w,M.w.size(),(unsigned long long)uset.n,now()-t0,rss_kb()/1024);
    }
    double bs=now()-tb;
    printf("PAIRS,%s,m=%zu,B=%zu,K=%d,Csum=%llu,Mk_total=%llu,U=%llu,build_s=%.1f,rss_MB=%ld\n",label,S.m,S.B,K,(unsigned long long)Csum,(unsigned long long)mkTotal,(unsigned long long)uset.n,bs,rss_kb()/1024);
    if(out){
        FILE* f=fopen(out,"wb"); if(!f){ perror("open --out"); return 1; }
        for(uint64_t key: uset.t) if(key!=~0ull){ u32 ab[2]={(u32)(key>>32),(u32)(key&0xFFFFFFFFu)}; fwrite(ab,4,2,f); }
        fclose(f);
    }
    return 0;
}
