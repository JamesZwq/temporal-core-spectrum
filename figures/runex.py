#!/usr/bin/env python3
"""The paper's running example: the input, the spectrum, the certificate, and the geometry.

One module so that Figures 1, 2 and 3 are drawn from a single source.  Before this existed the
generating scripts for Figures 1 and 2 had been lost (AUDIT.md defect D10), the figures could
only be edited by hand, and Figure 3 had to recover their coordinates by scraping
`pdftotext -bbox` output -- which meant three pictures of one example with no shared truth.

The eighteen temporal edges are K5 on v1..v5 at timestamps 0-9, the bridge v4v10@12 and
v6v10@14, and K4 on v6..v9 at 20-26.  Everything computed here was checked against the real
builder on exactly this input:

    kappa_max=4   C0=62   breakpoints=44 at six radii   MT=57   |U|=26   dedup=2.1923

and 44 and 62 are the numbers Table 2 and Section 4 already quote for this example.

The geometry is the geometry of the original Figure 1, recovered once from its PDF and frozen
here, so a redraw lands on top of the picture readers have already seen.
"""

# ------------------------------------------------------------------- the input ----
E = [(1,2,0),(1,3,1),(2,3,2),(1,4,3),(2,4,4),(3,4,5),(1,5,6),(2,5,7),(3,5,8),(4,5,9),
     (4,10,12),(6,10,14),(6,7,20),(6,8,21),(7,8,22),(6,9,23),(7,9,24),(8,9,26)]
m = len(E)
T = [e[2] for e in E]
DMAX = max(T)
inc = {}
for i, (u, v, t) in enumerate(E):
    inc.setdefault(u, []).append(i); inc.setdefault(v, []).append(i)

# --------------------------------------------------------------- the spectrum ----
def _coreness(D):
    """c_e(D) for every edge.  A side's support counts e itself, as Section 3 defines it."""
    c = [0]*m; cur = set(range(m)); k = 1
    while cur:
        changed = True
        while changed:
            changed = False
            for i in list(cur):
                u, v, t = E[i]
                if any(sum(1 for j in inc[x] if j in cur and abs(T[j]-t) <= D) < k
                       for x in (u, v)):
                    cur.discard(i); changed = True
        for i in cur: c[i] = k
        if not cur: break
        k += 1
    return c

CD = {D: _coreness(D) for D in range(DMAX+1)}
CFULL = CD[DMAX]
K = max(CFULL)
C0 = sum(CFULL)

def onset(i, k):
    """Delta_k(e): the least radius at which edge i holds level k."""
    for D in range(DMAX+1):
        if CD[D][i] >= k: return D
    return None

def breakpoints(i):
    """The strict steps of edge i's staircase, as (radius, new value), increasing in radius."""
    out = []
    for D in range(1, DMAX+1):
        if CD[D][i] > CD[D-1][i]: out.append((D, CD[D][i]))
    return out

B = sum(len(breakpoints(i)) for i in range(m))

# ------------------------------------------------------------- the certificate ----
def certificate():
    """U = the union over levels of the merges Kruskal accepts on the sparse candidate set."""
    U, MT = set(), 0
    for k in range(1, K+1):
        a = {i: onset(i, k) for i in range(m) if CFULL[i] >= k}
        P = set()
        for x, lst in inc.items():
            s = sorted([i for i in lst if i in a], key=lambda i: (T[i], i))
            for forward in (True, False):
                st = []
                for idx in (range(len(s)) if forward else range(len(s)-1, -1, -1)):
                    i = s[idx]
                    while st and a[s[st[-1]]] > a[i]: st.pop()
                    if st: P.add((min(s[st[-1]], i), max(s[st[-1]], i)))
                    st.append(idx)
        w = {p: max(a[p[0]], a[p[1]], abs(T[p[0]]-T[p[1]])) for p in P}
        par = list(range(m))
        def find(x):
            while par[x] != x: par[x] = par[par[x]]; x = par[x]
            return x
        for p in sorted(P, key=lambda p: (w[p], p)):
            ra, rb = find(p[0]), find(p[1])
            if ra != rb: par[ra] = rb; MT += 1; U.add(p)
    return sorted(U), MT

U, MT = certificate()

def community(start, k, D):
    """Algorithm 4: walk U from `start`, admitting an arc only if both tests pass."""
    if CFULL[start] < k or onset(start, k) > D: return set(), [], []
    adj = {}
    for a, b in U: adj.setdefault(a, []).append(b); adj.setdefault(b, []).append(a)
    seen, lvl, rad = {start}, [], []
    stack = [start]
    while stack:
        a = stack.pop()
        for f in adj.get(a, []):
            if f in seen: continue
            if CFULL[f] < k: lvl.append((a, f)); continue
            w = max(onset(a, k), onset(f, k), abs(T[a]-T[f]))
            if w > D: rad.append((a, f, w)); continue
            seen.add(f); stack.append(f)
    return seen, lvl, rad

assert (K, C0, B, MT, len(U)) == (4, 62, 44, 57, 26), \
    f"does not match the verified builder run: {(K, C0, B, MT, len(U))}"

# ----------------------------------------------------------------- the geometry ----
# Figure 1's own coordinates, in its page's points (top-left origin, page 242.51 x 114.531).
# Vertices are the centres of its v-labels; EDGE_LABEL is where it printed each timestamp.
PAGE_W, PAGE_H = 242.51, 114.531
V = {1:(62.18,24.07), 2:(37.84,41.76), 3:(47.14,69.93), 4:(77.23,70.38), 5:(86.53,41.32),
     10:(128.51,61.71), 6:(169.25,39.86), 7:(206.71,40.31), 8:(169.25,77.32), 9:(206.71,77.32)}
EDGE_LABEL = [(53.32,31.80),(58.35,38.04),(42.87,57.07),(66.78,38.04),(66.58,63.37),
              (62.57,71.38),(74.74,33.92),(73.28,42.76),(58.55,63.37),(82.26,57.07),
              (103.33,67.27),(149.34,52.23),
              (188.35,41.31),(169.62,60.04),(196.59,51.80),(180.11,51.80),(207.08,60.04),
              (188.35,78.77)]

# the three coreness contours Figure 1 draws, as (x0, y0, x1, y1) with their label anchors
CONTOURS = [
    ("early", 23.0, 14.9, 100.8, 85.2, "coreness 4",            62.4, 107.7, "#1f5fa8"),
    ("late", 156.1, 33.6, 220.9, 93.6, "coreness 3",           198.6, 107.7, "#1f5fa8"),
    ("all",   19.5, 10.8, 223.3, 98.4, "coreness 2 (the whole graph)",
                                                               121.1,   6.2, "#c2531a"),
]
BLUE, ORANGE, GREY = "#1f5fa8", "#c2531a", "#8a8a8a"

if __name__ == "__main__":
    print(f"  m={m}  kappa_max={K}  C0={C0}  B={B}  m+B={m+B}  MT={MT}  |U|={len(U)}")
    s, lv, rd = community(0, 3, 5)
    print(f"  query k=3 D=5 from v1v2: |C|={len(s)}  level-rejects={len(lv)}  radius-rejects={len(rd)}")
