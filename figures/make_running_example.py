#!/usr/bin/env python3
"""Draw the paper's running-example figures from one source.

Figure 1  the temporal graph itself, with each interaction's timestamp on its edge
Figure 2  the spectrum index: one row per interaction, one cell per stored entry
Figure 3  the same graph twice -- the certificate on it, and one query over the certificate

Both come from `runex.py`, which holds the eighteen-edge input, recomputes the spectrum and
the certificate, and carries Figure 1's own geometry.  Drawing them together is the point:
before this, Figure 1's generator had been lost and Figure 3 had to scrape its coordinates out
of the PDF, so nothing guaranteed the two pictures lined up.

Usage:  python3 make_running_example.py [--only fig1|fig2|fig3]
"""
import os, subprocess, sys
import runex as R

HERE = os.path.dirname(os.path.abspath(__file__))
OUT = os.path.join(HERE, '..', '..', 'papers', 'temporal-spectrum', 'figures')
SC = 4.0                                   # draw big, print small: the output is vector
# Type sizes are in the figure's own points, and the figure prints at essentially 1:1
# (natural width 242.9pt into a 241.1pt column), so these ARE the sizes on the page.
# The paper's body is 9pt; figure text sits just under it, never below 7.5.
F_VERTEX, F_SUB, F_TS, F_CONTOUR, F_NOTE = 8.0, 5.4, 7.5, 8.0, 7.5
R_VERTEX = 8.6                             # the circle has to hold an 8pt label
W, H = R.PAGE_W * SC, R.PAGE_H * SC
BRIDGE = {10, 11}                          # the two edges through v10

def xy(p):  return (p[0] * SC, p[1] * SC)
V  = {v: xy(p) for v, p in R.V.items()}
L  = [xy(p) for p in R.EDGE_LABEL]

# The certificate panels carry no coreness contours, so the band Figure 1 reserved for them
# is dead space.  Crop the page to the drawing and shift it up: two panels of full Figure 1
# height push the paper from 19 pages to 20 for nothing.
PANEL_TOP, PANEL_H = 12.0, 80.0

def head_svg(extra="", page_h=None, shift=0.0):
    hh = (page_h if page_h is not None else R.PAGE_H) * SC
    sh = -shift * SC
    return f'''<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 {-sh:.0f} {W:.0f} {hh:.0f}"
     width="{W:.0f}" height="{hh:.0f}"
     font-family="Linux Libertine, Libertine, Times New Roman, serif">
  <rect x="0" y="{-sh:.0f}" width="{W:.0f}" height="{hh:.0f}" fill="#fff"/>
  <style>
    .e{{stroke:#000;stroke-width:{0.5*SC:.2f};fill:none}}
    .eb{{stroke:{R.GREY};stroke-width:{0.5*SC:.2f};fill:none;stroke-dasharray:{0.9*SC:.1f},{0.9*SC:.1f}}}
    .v{{fill:#fff;stroke:#000;stroke-width:{0.95*SC:.2f}}}
    .vl{{font-size:{F_VERTEX*SC:.1f}px;fill:#000;text-anchor:middle;font-style:italic}}
    .sub{{font-size:{F_SUB*SC:.1f}px}}
    .ts{{font-size:{F_TS*SC:.1f}px;fill:#333;text-anchor:middle;paint-order:stroke;stroke:#fff;stroke-width:{1.6*SC:.1f}px;stroke-linejoin:round}}
    .ct{{fill:none;stroke-width:{0.55*SC:.2f};stroke-dasharray:{1.6*SC:.1f},{1.2*SC:.1f}}}
    .ctl{{font-size:{F_CONTOUR*SC:.1f}px;text-anchor:middle}}
    .note{{font-size:{F_NOTE*SC:.1f}px;fill:{R.GREY};text-anchor:middle;font-style:italic}}
{extra}  </style>
'''

def contours():
    out = []
    for _tag, x0, y0, x1, y1, lab, lx, ly, col in R.CONTOURS:
        out.append(f'<rect class="ct" stroke="{col}" x="{x0*SC:.1f}" y="{y0*SC:.1f}" '
                   f'width="{(x1-x0)*SC:.1f}" height="{(y1-y0)*SC:.1f}" rx="{3.2*SC:.1f}"/>')
        out.append(f'<text class="ctl" fill="{col}" x="{lx*SC:.1f}" y="{ly*SC:.1f}">{lab}</text>')
    return "\n  ".join(out)

def edges():
    return "\n  ".join(
        f'<line class="{"eb" if i in BRIDGE else "e"}" x1="{V[u][0]:.1f}" y1="{V[u][1]:.1f}" '
        f'x2="{V[v][0]:.1f}" y2="{V[v][1]:.1f}"/>'
        for i, (u, v, _t) in enumerate(R.E))

def vertices():
    return "\n  ".join(
        f'<circle class="v" cx="{x:.1f}" cy="{y:.1f}" r="{R_VERTEX*SC:.1f}"/>'
        f'<text class="vl" x="{x:.1f}" y="{y + 2.9*SC:.1f}">v<tspan class="sub" '
        f'dy="{1.6*SC:.1f}">{v}</tspan></text>'
        for v, (x, y) in V.items())

def bridge_note():
    bx, by = (V[4][0] + V[6][0]) / 2, max(V[10][1], V[4][1]) + 8.4 * SC
    return (f'<text class="note" x="{V[10][0]:.1f}" y="{by:.1f}">bridge via '
            f'v<tspan class="sub" dy="{1.0*SC:.1f}">10</tspan></text>')

def fig1():
    ts = "\n  ".join(
        f'<text class="ts" x="{L[i][0]:.1f}" y="{L[i][1] + 2.6*SC:.1f}">{R.T[i]}</text>'
        for i in range(R.m))
    return head_svg() + f'''  {contours()}
  {edges()}
  {ts}
  {vertices()}
  {bridge_note()}
</svg>
'''

def _base(faint):
    """The graph of Figure 1, either at full strength or held back so an overlay can lead."""
    col = "#c9c9c9" if faint else "#000"
    w = 0.45 if faint else 0.5
    out = [f'<line stroke="{col}" stroke-width="{w*SC:.2f}" '
           f'{"stroke-dasharray=\"%.1f,%.1f\"" % (0.9*SC, 0.9*SC) if i in BRIDGE else ""} '
           f'x1="{V[u][0]:.1f}" y1="{V[u][1]:.1f}" x2="{V[v][0]:.1f}" y2="{V[v][1]:.1f}"/>'
           for i, (u, v, _t) in enumerate(R.E)]
    vc = "#c9c9c9" if faint else "#000"
    for v, (x, y) in V.items():
        out.append(f'<circle fill="#fff" stroke="{vc}" stroke-width="{0.95*SC:.2f}" '
                   f'cx="{x:.1f}" cy="{y:.1f}" r="{R_VERTEX*SC:.1f}"/>')
        out.append(f'<text class="vl" fill="{vc}" x="{x:.1f}" y="{y + 2.9*SC:.1f}">'
                   f'v<tspan class="sub" dy="{1.6*SC:.1f}">{v}</tspan></text>')
    return "\n  ".join(out)

def fig_hier():
    """Figure 3: the certificate cuts the number of index probes a query has to make.

    Both walks answer a community query the same way -- pop an interaction, ask the spectrum
    index for each candidate's onset at the queried level, keep it if the onset is at most
    the queried radius.  The probe is identical on both sides: one binary search into that
    interaction's own breakpoint list.  What differs is HOW MANY probes each walk has to
    make, and that is the whole of the advantage.

      * On the graph, reaching an interaction's neighbours means going through an endpoint,
        and the endpoint's incidences that are active at (k, Delta) cannot be known without
        probing every one of them.  The walk therefore pays the FULL DEGREE of every endpoint
        it passes through, however small the answer is.
      * On the certificate, an interaction's neighbours are its own arcs.  The walk pays the
        ANSWER plus the arcs leaving it, and nothing else.

    The query drawn is level 1, radius 2, from v4v10@12.  Both walks return the same two
    interactions; the certificate makes 4 probes and the graph makes 9.  The five extra ones
    are exactly v1v4@3, v2v4@4, v3v4@5, v6v8@21 and v6v9@23 -- everything else hanging off
    the endpoints v4 and v6, none of it related to the answer.  Both walks are simulated here
    by the same rule hier_index.cpp's ubfs and gbfs use, and the assertions below pin the
    counts and the swept endpoint set.

    On this 18-edge example the ratio is only 9/4.  The real magnitude is in usweep4.csv,
    where the certificate reads 2.01 to 13.31 slots per returned interaction over all 420
    cells while the graph walk reads 2.0 to 13,538, and it goes in the caption because no
    drawing of an 18-edge graph will show a 2,536x gap.

    An earlier version of this figure drew all 50 incident pairs against U's 26 arcs.  That
    comparison was a straw man: nobody stores the line graph, the real baseline is the
    adjacency list, and against an adjacency list U is 1.48x to 2.91x LARGER.  Recorded so
    the mistake is not repeated -- the certificate does not win on what it stores, it wins on
    how many probes a walk over it costs.

    Everything is computed in runex.py and checked against the real builder (|U|=26, MT=57,
    C0=62, kappa_max=4, B=44).
    """
    # Figure 1 reserves a band top and bottom for its coreness contours; this figure has
    # none, so that band is dead space.  Crop to the drawing's own ink -- vertices y in
    # [16.9, 84.5] once the r=7.2 circle and the label descender are counted, edge dots y in
    # [29.9, 80.7] -- and keep the scale and the width at Figure 1's, which is what makes
    # both panels read as the graph the reader already saw.
    VTOP, VBOT = 13.5, 89.5
    PANH = VBOT - VTOP
    TOPH, BOTH = 14.0, 14.5
    GAP = 21.0
    PW = R.PAGE_W * 2 + GAP
    PH = PANH + TOPH + BOTH
    F = 8.0
    E0, KQ, DQ = 10, 1, 2                  # start v4v10@12, level 1, radius 2

    # --- the two walks, counted exactly as hier_index.cpp counts them -------------------
    adjU = {}
    for a, b in R.U:
        adjU.setdefault(a, []).append(b); adjU.setdefault(b, []).append(a)

    def walk_cert():
        """ubfs: one probe for the start, then one per neighbour reached over an arc."""
        seen, stack, probed, arcs = {E0}, [E0], {E0}, []
        while stack:
            a = stack.pop()
            for f in adjU.get(a, []):
                if f in seen: continue
                probed.add(f)
                o = R.onset(f, KQ)
                ok = o is not None and o <= DQ and \
                     max(R.onset(a, KQ), o, abs(R.T[a] - R.T[f])) <= DQ
                arcs.append((a, f, ok))
                if ok: seen.add(f); stack.append(f)
        return seen, probed, arcs

    def walk_graph():
        """gbfs: every incidence of every endpoint the walk passes through gets a probe."""
        seen, stack, swept, probed = {E0}, [E0], set(), {E0}
        while stack:
            a = stack.pop()
            for x in (R.E[a][0], R.E[a][1]):
                if x not in swept:
                    swept.add(x); probed.update(R.inc[x])
                act = sorted([b for b in R.inc[x]
                              if R.onset(b, KQ) is not None and R.onset(b, KQ) <= DQ],
                             key=lambda i: R.T[i])
                if a not in act: continue
                i = act.index(a)
                for j in range(i, len(act) - 1):
                    if R.T[act[j+1]] - R.T[act[j]] > DQ: break
                    if act[j+1] not in seen: seen.add(act[j+1]); stack.append(act[j+1])
                for j in range(i, 0, -1):
                    if R.T[act[j]] - R.T[act[j-1]] > DQ: break
                    if act[j-1] not in seen: seen.add(act[j-1]); stack.append(act[j-1])
        return seen, probed, swept

    ans_u, pro_u, arcs_u = walk_cert()
    ans_g, pro_g, swept_g = walk_graph()
    assert ans_u == ans_g, "the two walks must return the same community"
    assert (len(ans_u), len(pro_u), len(pro_g)) == (2, 4, 9)
    assert swept_g == {4, 10, 6}

    def panel(ox, probed, arcs=None, rings=()):
        """Figure 1's drawing, with each interaction shaded by what the walk did to it."""
        X = lambda x: ox + x
        Y = lambda y: TOPH + y - VTOP
        g = []
        for i, (u, v, _t) in enumerate(R.E):
            d = ' stroke-dasharray="1.1,1.1"' if i in BRIDGE else ''
            g.append(f'<line stroke="#d8d8d8" stroke-width="0.75"{d} '
                     f'x1="{X(R.V[u][0]):.1f}" y1="{Y(R.V[u][1]):.1f}" '
                     f'x2="{X(R.V[v][0]):.1f}" y2="{Y(R.V[v][1]):.1f}"/>')
        for v, (x, y) in R.V.items():
            on = v in rings
            g.append(f'<circle fill="#fff" stroke="{R.ORANGE if on else "#d8d8d8"}" '
                     f'stroke-width="{1.6 if on else 0.85}" '
                     f'cx="{X(x):.1f}" cy="{Y(y):.1f}" r="7.2"/>')
            g.append(f'<text class="vl" fill="{R.ORANGE if on else "#b8b8b8"}" '
                     f'x="{X(x):.1f}" y="{Y(y)+2.6:.1f}">'
                     f'v<tspan class="s2" dy="1.5">{v}</tspan></text>')
        if arcs is not None:                       # the stored certificate, then what was read
            for a, b in R.U:
                g.append(f'<line stroke="#cdd8e6" stroke-width="0.8" '
                         f'x1="{X(R.EDGE_LABEL[a][0]):.1f}" y1="{Y(R.EDGE_LABEL[a][1]):.1f}" '
                         f'x2="{X(R.EDGE_LABEL[b][0]):.1f}" y2="{Y(R.EDGE_LABEL[b][1]):.1f}"/>')
            for a, b, ok in arcs:
                st = (f'stroke="{R.BLUE}" stroke-width="1.5"' if ok else
                      f'stroke="{R.ORANGE}" stroke-width="1.1" stroke-dasharray="2.2,1.7"')
                g.append(f'<line {st} stroke-linecap="round" '
                         f'x1="{X(R.EDGE_LABEL[a][0]):.1f}" y1="{Y(R.EDGE_LABEL[a][1]):.1f}" '
                         f'x2="{X(R.EDGE_LABEL[b][0]):.1f}" y2="{Y(R.EDGE_LABEL[b][1]):.1f}"/>')
        for i in range(R.m):
            if i in ans_u:      fill, edge, r = R.BLUE, R.BLUE, 2.6
            elif i in probed:   fill, edge, r = "#fff", R.ORANGE, 2.4
            else:               fill, edge, r = "#fff", "#c4c4c4", 1.7
            g.append(f'<circle fill="{fill}" stroke="{edge}" stroke-width="1.0" '
                     f'cx="{X(R.EDGE_LABEL[i][0]):.1f}" cy="{Y(R.EDGE_LABEL[i][1]):.1f}" '
                     f'r="{r}"/>')
        return "\n  ".join(g)

    b = []
    b.append(f'<text class="pan" x="0" y="9">(a)  walking the graph: '
             f'<tspan fill="{R.ORANGE}">{len(pro_g)} probes</tspan></text>')
    b.append(panel(0.0, pro_g, rings=swept_g))
    b.append(f'<text class="pan" x="{R.PAGE_W+GAP:.1f}" y="9">(b)  walking the certificate: '
             f'<tspan fill="{R.BLUE}">{len(pro_u)} probes</tspan></text>')
    b.append(panel(R.PAGE_W + GAP, pro_u, arcs=arcs_u))

    yb = TOPH + PANH + 10.0
    b.append(f'<text class="note3" x="{R.PAGE_W/2:.1f}" y="{yb:.1f}">every interaction at '
             f'each endpoint it passes through</text>')
    b.append(f'<text class="note3" x="{R.PAGE_W+GAP+R.PAGE_W/2:.1f}" y="{yb:.1f}">'
             f'the answer, and the arcs leaving it</text>')

    joined = "\n  ".join(b)
    return f'''<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 {PW:.1f} {PH:.1f}"
     width="{PW:.1f}" height="{PH:.1f}"
     font-family="Linux Libertine, Libertine, Times New Roman, serif">
  <rect width="{PW:.1f}" height="{PH:.1f}" fill="#fff"/>
  <style>
    .pan{{font-size:{F}px;fill:#000;font-weight:bold}}
    .vl{{font-size:7.0px;text-anchor:middle;font-style:italic}}
    .tick{{font-size:6.6px;fill:#444;text-anchor:middle;font-style:italic}}
    .s2{{font-size:4.8px}}
    .note3{{font-size:{F}px;fill:#444;text-anchor:middle}}
  </style>
  {joined}
</svg>
''', PH, PW

def fig_index():
    """Figure 2: the stored index itself -- one row per interaction, one cell per entry.

    The cells are generated from runex.breakpoints(), not transcribed: all eighteen rows were
    checked against what the lost original printed and every one matches, which is what makes
    a redraw safe.  Shading encodes the coreness a breakpoint raises the edge to, so the row
    reads as a staircase without needing a plot.
    """
    PW, PH = 506.295, 168.0                # the whole figure* slot it is printed into.
    # A fractional slot (this was 0.8\textwidth) is not the house idiom and leaves a
    # ragged margin that lines up with nothing else on the page.
    F = 8.0                                # body is 9pt; figure text sits just under it
    ROW, CELL_H, CW = 12.9, 10.6, 25.0
    SHADE = {1: ("#fff", "#000"), 2: ("#e2e2e2", "#000"),
             3: ("#ababab", "#000"), 4: ("#4d4d4d", "#fff")}
    out = []

    def block(rows, x_lab, x0, y0, gap_after=None):
        y = y0
        for n, i in enumerate(rows):
            if gap_after is not None and n == gap_after: y += ROW * 0.62
            u, v, _t = R.E[i]
            lab = (f'v<tspan class="s2" dy="{1.6:.1f}">{v}</tspan>'
                   f'<tspan dy="{-1.6:.1f}">v</tspan><tspan class="s2" dy="{1.6:.1f}">{u}</tspan>'
                   if i == 11 else
                   f'v<tspan class="s2" dy="{1.6:.1f}">{u}</tspan>'
                   f'<tspan dy="{-1.6:.1f}">v</tspan><tspan class="s2" dy="{1.6:.1f}">{v}</tspan>')
            out.append(f'<text class="lab" x="{x_lab:.1f}" y="{y + CELL_H*0.72:.1f}">{lab}</text>')
            cells = [("1", 1)] + [(f"{d}:{c}", c) for d, c in R.breakpoints(i)]
            for k, (txt, c) in enumerate(cells):
                fill, ink = SHADE[c]
                x = x0 + k * CW
                out.append(f'<rect x="{x:.1f}" y="{y:.1f}" width="{CW:.1f}" '
                           f'height="{CELL_H:.1f}" fill="{fill}" stroke="#000" '
                           f'stroke-width="0.55"/>')
                out.append(f'<text class="cell" fill="{ink}" x="{x + CW/2:.1f}" '
                           f'y="{y + CELL_H*0.72:.1f}">{txt}</text>')
            y += ROW
        return y

    block(list(range(10)), 30.0, 46.0, 15.0)
    block(list(range(10, 18)), 163.0, 179.0, 15.0, gap_after=2)

    out.append(f'<text class="key" x="272.0" y="18.0">base c'
               f'<tspan class="s2" dy="1.6">e</tspan><tspan dy="-1.6">(0)</tspan></text>')
    out.append(f'<text class="key" x="272.0" y="29.0">breakpoints (Δ'
               f'<tspan class="s2" dy="1.6">up</tspan><tspan dy="-1.6"> : c), shaded by c</tspan></text>')

    # the inset: one stored row read back as the staircase it encodes
    bx0, bx1, by0, by1 = 288.0, 392.0, 52.0, 124.0
    steps = R.breakpoints(9)                       # v4v5
    n = len(steps) + 1
    seg = (bx1 - bx0) / n
    lo, hi = by1 - 8.0, by0 + 10.0
    lvl = [lo - (lo - hi) * j / (n - 1) for j in range(n)]
    out.append(f'<line class="ax" x1="{bx0:.1f}" y1="{by0:.1f}" x2="{bx0:.1f}" y2="{by1:.1f}"/>')
    out.append(f'<line class="ax" x1="{bx0:.1f}" y1="{by1:.1f}" x2="{bx1:.1f}" y2="{by1:.1f}"/>')
    for j in range(n):
        x_a, x_b = bx0 + j * seg, bx0 + (j + 1) * seg
        out.append(f'<line class="st" x1="{x_a:.1f}" y1="{lvl[j]:.1f}" '
                   f'x2="{x_b:.1f}" y2="{lvl[j]:.1f}"/>')
        if j:
            out.append(f'<line class="ri" x1="{x_a:.1f}" y1="{lvl[j-1]:.1f}" '
                       f'x2="{x_a:.1f}" y2="{lvl[j]:.1f}"/>')
            d, c = steps[j-1]
            out.append(f'<text class="cell" x="{x_a:.1f}" y="{lvl[j] - 3.4:.1f}">{d}:{c}</text>')
    out.append(f'<text class="key" x="{bx0 - 2:.1f}" y="{by0 - 4:.1f}">'
               f'c<tspan class="s2" dy="1.6">e</tspan><tspan dy="-1.6">(Δ)</tspan></text>')
    out.append(f'<text class="key" x="{bx1 - 6:.1f}" y="{by1 + 11:.1f}">Δ</text>')
    out.append(f'<text class="note2" x="{(bx0+bx1)/2:.1f}" y="{by1 + 20:.1f}">decoded staircase '
               f'of v<tspan class="s2" dy="1.6">4</tspan><tspan dy="-1.6">v</tspan>'
               f'<tspan class="s2" dy="1.6">5</tspan></text>')

    out.append(f'<text class="sum" x="152.0" y="160.0">{R.m} base + {R.B} breakpoints '
               f'= m+B = {R.m + R.B} stored entries</text>')

    body = "\n  ".join(out)
    return f'''<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 {PW} {PH}"
     width="{PW}" height="{PH}"
     font-family="Linux Libertine, Libertine, Times New Roman, serif">
  <rect width="{PW}" height="{PH}" fill="#fff"/>
  <style>
    .lab{{font-size:{F}px;fill:#000;text-anchor:end;font-style:italic}}
    .cell{{font-size:{F-0.4}px;text-anchor:middle}}
    .key{{font-size:{F}px;fill:#000;text-anchor:start}}
    .note2{{font-size:{F}px;fill:#555;text-anchor:middle}}
    .sum{{font-size:{F+0.6}px;fill:#000;text-anchor:middle}}
    .s2{{font-size:{F-2.4}px}}
    .ax{{stroke:#000;stroke-width:0.9}}
    .st{{stroke:#000;stroke-width:2.0}}
    .ri{{stroke:#000;stroke-width:0.8;stroke-dasharray:1.8,1.6}}
  </style>
  {body}
</svg>
''', PH, PW

def emit(name, svg, page_h=None, page_w=None):
    svg_path = os.path.join(OUT, name + '.svg')
    open(svg_path, 'w').write(svg)
    chrome = "/Applications/Google Chrome.app/Contents/MacOS/Google Chrome"
    if not os.path.exists(chrome):
        print(f"  wrote {svg_path} (no Chrome: PDF not refreshed)"); return
    # Print through an HTML wrapper whose @page IS Figure 1's page, and never crop: cropping
    # trims to the ink, and two figures cropped differently no longer scale alike once LaTeX
    # fits both to \columnwidth.
    html = os.path.join(OUT, f'.{name}.print.html')
    open(html, 'w').write(
        f"<!doctype html><meta charset='utf-8'>"
        f"<style>@page{{size:{page_w or R.PAGE_W}pt {page_h or R.PAGE_H}pt;margin:0}}"
        f"html,body{{margin:0;padding:0}}svg{{display:block;width:{page_w or R.PAGE_W}pt;"
        f"height:{page_h or R.PAGE_H}pt}}</style>" + svg)
    pdf = os.path.join(OUT, name + '.pdf')
    subprocess.run([chrome, "--headless", "--disable-gpu", "--no-pdf-header-footer",
                    f"--print-to-pdf={pdf}", f"file://{html}"], capture_output=True)
    os.remove(html)
    print(f"  wrote {os.path.basename(pdf)}")

if __name__ == "__main__":
    only = sys.argv[sys.argv.index('--only') + 1] if '--only' in sys.argv else None
    print(f"  m={R.m} kappa_max={R.K} C0={R.C0} B={R.B} |U|={len(R.U)}")
    if only in (None, 'fig1'): emit('running_graph', fig1())
    if only in (None, 'fig3'):
        svg, ph, pw = fig_hier()
        emit('hier_fig', svg, ph, pw)
    if only in (None, 'fig2'):
        svg, ph, pw = fig_index()
        emit('indexfig', svg, ph, pw)
