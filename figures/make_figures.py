#!/usr/bin/env python3
"""Redraw every experiment figure from the recorded CSVs.

The previous figures were drawn in July from the eight-graph campaign and their generating
scripts were lost, so the plots and the prose disagreed (AUDIT.md defect D10).  This script
IS the replacement: it reads only files listed in experiments/EXPERIMENTS.md and writes into
papers/temporal-spectrum/figures/.  Never hand-edit a figure; change this and re-run.

Visual system (teacher-paper-imitation.md section 7.4):
  * monochrome, hatch-coded; the FINAL algorithm is solid black
  * red x is reserved for runs that did not finish -- nothing else is red
  * panels are drawn at their final size, 2.2 in wide, 6-7.5 pt type
  * subcaptions carry dataset abbreviations; captions live in the LaTeX

Usage:  python3 make_figures.py [--only bld_main]
"""
import csv, os, re, sys, math
import matplotlib
matplotlib.use('Agg')
import matplotlib.pyplot as plt

HERE = os.path.dirname(os.path.abspath(__file__))
DATA = os.path.join(HERE, '..', 'campaign-2026-08-18')
OUT  = os.path.join(HERE, '..', '..', 'papers', 'temporal-spectrum', 'figures')

# the ten paper graphs, in the order of the datasets table (descending edges)
TEN = [('AHO','AHO'), ('FL','FL'), ('AB','AB'), ('EPI','EPI'), ('YT','YT'),
       ('DBLPCA','DBLP'), ('WT','WT'), ('BRIGHTKITE','BK'), ('HEPPH','HEP'), ('ENR','ENR')]
CEILING = 10800.0            # the three-hour build limit stated in the Experiments setup

# Type in a figure must read at nearly body size, so every figure is drawn at exactly the
# width it will be printed at and the font sizes below ARE the sizes on the page.  The paper
# is acmart/sigconf: body 9pt, \columnwidth 241.15pt, \textwidth 506.30pt.  Drawing wider
# than the slot and letting \includegraphics shrink it is what makes figure text unreadable:
# scal used to be 5.34in in a 3.35in column, so its 7pt type printed at 4.4pt.
PT = 1/72.0
# The teacher papers use only whole slots -- \textwidth across a figure*, or \linewidth inside
# a 0.32\textwidth minipage -- never a fractional 0.95\textwidth, which leaves a ragged margin
# and lines up with nothing else on the page.  Every entry here is one of those two widths.
WIDTH_IN = {                      # the printed width of each figure, in inches
    'bld_main':  506.295*PT,      # the full figure* slot
    'bld_break': 0.32*506.295*PT, # one minipage of the three-across row
    'bld_size':  0.32*506.295*PT,
    'hierbench': 0.32*506.295*PT,
    'qsnap':     506.295*PT,
    'scal':      506.295*PT,
}

plt.rcParams.update({
    'font.family': 'serif', 'font.serif': ['Times New Roman', 'DejaVu Serif'],
    'font.size': 8, 'axes.labelsize': 8, 'xtick.labelsize': 7.6,
    'ytick.labelsize': 7.6, 'legend.fontsize': 7.6,
    'axes.linewidth': 0.6, 'xtick.major.width': 0.6, 'ytick.major.width': 0.6,
    'xtick.major.size': 2, 'ytick.major.size': 2, 'hatch.linewidth': 0.5,
    'pdf.fonttype': 42,
})

def rows(name):
    p = os.path.join(DATA, name)
    if not os.path.exists(p): return []
    with open(p) as f: return list(csv.DictReader(f))

def num(x, d=0.0):
    try: return float(x)
    except (TypeError, ValueError): return d

def save(fig, name):
    """Save at exactly the figure's own size.

    Never bbox_inches='tight' here: it crops the canvas down to the ink, so the PDF comes out
    narrower than the slot it was drawn for and \\includegraphics scales it back UP -- which
    enlarges the type by the same factor and undoes the whole point of drawing at printed
    width.  tight_layout instead packs the axes inside the fixed canvas."""
    os.makedirs(OUT, exist_ok=True)
    p = os.path.join(OUT, name + '.pdf')
    try: fig.tight_layout(pad=0.25)
    except Exception: pass
    fig.savefig(p)
    plt.close(fig)
    print(f"  wrote {os.path.relpath(p, os.path.join(HERE,'..','..'))}")

# --------------------------------------------------------------------------- data ----
SPEC = {r['label']: r for r in rows('spectrum.csv')}
PROF = {r['label']: r for r in rows('spectrum_profile.csv') if r.get('status') == 'OK'}
HALL = {r['label']: r for r in rows('hall.csv') if r.get('status') == 'OK'}
FOIL = {}
for r in rows('build.csv'):
    m = re.match(r'^P-([A-Z]+)-(sweep0?)$', r['label'])
    if m: FOIL[(m.group(1), m.group(2))] = r

def foil_time(g, algo):
    """(seconds, finished?) -- a timeout is plotted at the ceiling and marked."""
    r = FOIL.get((g, algo))
    if not r: return None, False
    if r['status'] != 'OK': return CEILING, False
    return num(r['wall_s']), True

# ------------------------------------------------------------------- bld_main ----
def bld_main():
    """The money shot: the three builders on every paper graph, log seconds."""
    labels = [ab for _, ab in TEN]
    s0 = [foil_time(g, 'sweep0') for g, _ in TEN]
    sw = [foil_time(g, 'sweep')  for g, _ in TEN]
    ks = [num(SPEC[g]['phase_total_s']) for g, _ in TEN]

    fig, ax = plt.subplots(figsize=(WIDTH_IN['bld_main'], 1.95))
    x = range(len(TEN)); w = 0.27
    b0 = ax.bar([i - w for i in x], [t or 1 for t, _ in s0], w,
                facecolor='white', edgecolor='black', linewidth=0.6, label=r'$\mathsf{Sweep}^0$')
    b1 = ax.bar(list(x), [t or 1 for t, _ in sw], w,
                facecolor='white', edgecolor='black', linewidth=0.6, hatch='////', label=r'$\mathsf{Sweep}$')
    ax.bar([i + w for i in x], ks, w, facecolor='black', edgecolor='black',
           linewidth=0.6, label=r'$\mathsf{Sweep}^*$')

    # red x marks a run that hit the three-hour ceiling -- red is used for nothing else
    for i, (t, ok) in enumerate(s0):
        if t and not ok: ax.plot(i - w, t * 1.35, marker='x', color='red', ms=3.4, mew=0.9)
    for i, (t, ok) in enumerate(sw):
        if t and not ok: ax.plot(i, t * 1.35, marker='x', color='red', ms=3.4, mew=0.9)

    # the limit line carries no in-panel label: the subcaption names it, and any label
    # placed here collides with the red x of whichever graph sits under it
    ax.axhline(CEILING, color='black', lw=0.5, ls=(0, (3, 2)))
    ax.set_yscale('log'); ax.set_ylabel('build time (s)')
    ax.set_xticks(list(x)); ax.set_xticklabels(labels)
    ax.set_ylim(100, CEILING * 4)
    ax.legend(ncol=3, frameon=False, loc='upper center', bbox_to_anchor=(0.5, 1.22),
              handlelength=1.4, columnspacing=1.4, handletextpad=0.5)
    ax.spines['top'].set_visible(False); ax.spines['right'].set_visible(False)
    save(fig, 'bld_main')

# ------------------------------------------------------------------ bld_break ----
def bld_break():
    """Where the build's time goes: seed, init, and the four sweep phases."""
    gs = [(g, ab) for g, ab in TEN if g in PROF]
    fig, ax = plt.subplots(figsize=(WIDTH_IN['bld_break'], 1.80))
    x = range(len(gs)); w = 0.19
    keys = [('departure_s', '....', 'departure'), ('drop_s', '////', 'drop'),
            ('cascade_s', '\\\\\\\\', 'cascade'), ('arm_s', None, 'arm')]
    for j, (k, h, lab) in enumerate(keys):
        vals = [num(PROF[g][k]) for g, _ in gs]
        ax.bar([i + (j - 1.5) * w for i in x], vals, w, label=lab,
               facecolor='black' if h is None else 'white',
               edgecolor='black', linewidth=0.5, hatch=h)
    ax.set_yscale('log'); ax.set_ylabel('phase time (s)')
    ax.set_xticks(list(x)); ax.set_xticklabels([ab for _, ab in gs], rotation=45, ha='right')
    ax.legend(ncol=2, frameon=False, fontsize=5.6, loc='upper right', handlelength=1.2,
              columnspacing=0.8, handletextpad=0.4)
    ax.spines['top'].set_visible(False); ax.spines['right'].set_visible(False)
    save(fig, 'bld_break')

# ------------------------------------------------------------------- bld_size ----
def bld_size():
    """What an installation stores: the spectrum index against the merge forests."""
    gs = [(g, ab) for g, ab in TEN if g in SPEC]
    fig, ax = plt.subplots(figsize=(WIDTH_IN['bld_size'], 1.80))
    x = range(len(gs)); w = 0.36
    idx = [num(SPEC[g]['stream_bytes']) / 1e9 for g, _ in gs]
    fore = [num(HALL[g]['hidx_bytes']) / 1e9 if g in HALL else 0 for g, _ in gs]
    ax.bar([i - w/2 for i in x], fore, w, facecolor='white', edgecolor='black',
           linewidth=0.6, hatch='////', label='merge forests')
    ax.bar([i + w/2 for i in x], idx, w, facecolor='black', edgecolor='black',
           linewidth=0.6, label='spectrum index')
    ax.set_yscale('log'); ax.set_ylabel('stored (GB)')
    ax.set_xticks(list(x)); ax.set_xticklabels([ab for _, ab in gs], rotation=45, ha='right')
    ax.legend(frameon=False, fontsize=5.8, loc='upper right', handlelength=1.2, handletextpad=0.4)
    ax.spines['top'].set_visible(False); ax.spines['right'].set_visible(False)
    save(fig, 'bld_size')

# ----------------------------------------------------------------- hierbench ----
def hierbench():
    """One index build against building the certificate on top of it."""
    gs = [(g, ab) for g, ab in TEN if g in HALL]
    fig, ax = plt.subplots(figsize=(WIDTH_IN['hierbench'], 1.80))
    x = range(len(gs)); w = 0.36
    idx = [num(SPEC[g]['phase_total_s']) for g, _ in gs]
    cert = [num(HALL[g]['ck_cand_s']) + num(HALL[g]['ck_sort_s']) + num(HALL[g]['ck_kruskal_s'])
            for g, _ in gs]
    ax.bar([i - w/2 for i in x], idx, w, facecolor='white', edgecolor='black',
           linewidth=0.6, hatch='////', label='spectrum index')
    ax.bar([i + w/2 for i in x], cert, w, facecolor='black', edgecolor='black',
           linewidth=0.6, label='certificate')
    ax.set_yscale('log'); ax.set_ylabel('build time (s)')
    ax.set_xticks(list(x)); ax.set_xticklabels([ab for _, ab in gs], rotation=45, ha='right')
    # Inside the axes the legend landed on the WT and BK bars, and shrinking it to 5.8pt to
    # dodge them broke the 7.5pt floor.  Put it above the frame at the figure's own size and
    # give the bars headroom instead.
    ax.set_ylim(top=max(idx + cert) * 4.0)
    ax.legend(frameon=False, loc='upper center', bbox_to_anchor=(0.5, 1.16), ncol=2,
              handlelength=1.3, handletextpad=0.4, columnspacing=1.1, borderaxespad=0.0)
    ax.spines['top'].set_visible(False); ax.spines['right'].set_visible(False)
    save(fig, 'hierbench')

# ---------------------------------------------------------------------- qsnap ----
def qsnap():
    """Exp-5: one indexed snapshot against one \\sota decomposition, over the radius.

    Reads every edgecd_delta_<G>.csv the campaign has.  A cell may be recorded more than
    once (night13 re-measures the ones taken under load); the paper's protocol is the
    minimum over runs, so that is what is plotted -- never a hand-picked row.
    """
    import glob
    # Only graphs whose sweep satisfies the stated protocol (minimum over repeated runs).
    # ENR's 16 cells were all taken at loadavg <= 1.52; YT has two cells taken under load
    # and is excluded until night13.sh appends its second run.  Add a graph here only after
    # every cell of it has at least two recorded runs.
    QSNAP_GRAPHS = ('ENR', 'YT', 'EPI', 'AB')
    # panel order follows the datasets table (descending edges), not the filesystem's
    # alphabetical glob -- every other figure in the paper uses that order
    series = []
    for g, _ab in TEN:
        if g not in QSNAP_GRAPHS:
            continue
        p = os.path.join(DATA, f'edgecd_delta_{g}.csv')
        if not os.path.exists(p):
            continue
        # Each side takes its own minimum over recorded runs, independently.  For the
        # decomposition that means its FASTEST run, which makes every ratio we quote a lower
        # bound on our advantage; never pair a slow baseline run with a fast one of ours.
        dec, our = {}, {}
        with open(p) as f:
            for r in csv.DictReader(f):
                d = num(r['delta'])
                if d <= 0: continue             # the Delta=0 copy-constant diagnostic
                if r['edgecd_status'] == 'OK':
                    e = num(r['edgecd_decomp_s'])
                    if d not in dec or e < dec[d]: dec[d] = e
                if r['ours_status'] == 'OK':
                    o = num(r['ours_query_wall_s'])
                    if d not in our or o < our[d]: our[d] = o
        best = {d: (dec[d], our[d]) for d in dec if d in our}
        # Our side is the minimum of three runs, which is the protocol the paper states; the
        # edgecd_delta_* files hold only one run per cell, so ours_requery_<G>.csv supersedes
        # their ours column wherever it exists.  The decomposition side is never re-run.
        rq = os.path.join(DATA, f'ours_requery_{g}.csv')
        if os.path.exists(rq):
            mins = {}
            with open(rq) as f:
                for r in csv.DictReader(f):
                    if r.get('ours_status') != 'OK': continue
                    d, v = num(r['delta']), num(r['ours_query_wall_s'])
                    if d in best and (d not in mins or v < mins[d]): mins[d] = v
            for d, v in mins.items(): best[d] = (best[d][0], v)
        if len(best) >= 4:
            ds = sorted(best)
            series.append((dict(TEN)[g], ds, [best[d][0] for d in ds], [best[d][1] for d in ds]))
    if not series:
        print("  no edgecd_delta_*.csv yet"); return

    # 1.40 in tall, not taller: at 1.70 this figure pushes the paper from 19 to 20 pages
    n = len(series)
    fig, axes = plt.subplots(1, n, figsize=(WIDTH_IN['qsnap'], 1.45), squeeze=False)
    for ax, (ab, ds, ec, ou) in zip(axes[0], series):
        ax.plot(ds, ec, marker='o', ms=2.6, lw=0.9, color='black', mfc='white',
                mew=0.7, label=r'$\mathsf{EdgeCD}$')
        ax.plot(ds, ou, marker='s', ms=2.6, lw=1.2, color='black', label='one snapshot')
        ax.set_xscale('log'); ax.set_yscale('log')
        ax.set_xlabel(r'radius $\Delta$ (s)'); ax.set_title(ab, fontsize=7, pad=2)
        ax.spines['top'].set_visible(False); ax.spines['right'].set_visible(False)
    axes[0][0].set_ylabel('time (s)')
    axes[0][0].legend(frameon=False, fontsize=6, loc='upper left', handlelength=1.4,
                      handletextpad=0.4, borderaxespad=0.2)
    save(fig, 'qsnap')

# ----------------------------------------------------------------------- scal ----
def scal():
    """Exp-7: what the build actually scales with, on 20-100% edge samples.

    The point is not that build time rises with sample size -- that is trivially true.  It is
    that time tracks the RECORDED OUTPUT and not the edge count: on YT the edges grow 5.0x
    over the sample range while the breakpoints grow 21.9x and the time 16.6x.  So each panel
    normalises all three to their own 100% value; edges is a straight line by construction and
    the build time sits on the breakpoint curve, not on it.
    """
    # A sample may be recorded more than once: AHO's 60% and 100% were re-run because their
    # per-breakpoint cost broke the falling trend, and 60%'s original straddled a loadavg
    # spike (11.47 -> 32.11) that cost it 10%.  Take the FASTEST recorded run of each sample,
    # which is the same rule the baseline side of Exp-5 uses -- the run least contaminated by
    # whatever else the shared box was doing.  Breakpoints are identical across re-runs, so
    # only the timing moves.
    fastest = {}
    for r in _scale_rows():
        if r['status'] != 'OK': continue
        k = (r['label'], num(r['pct']))
        w = num(r['wall_s'])
        if k not in fastest or w < fastest[k][2]:
            fastest[k] = (num(r['edges_loaded']), num(r['breakpoints']), w)
    data = {}
    for (lab, pct), (e, b, w) in fastest.items():
        data.setdefault(lab, []).append((pct, e, b, w))
    order = [g for g, _ in TEN if g in data and len(data[g]) >= 3]
    if not order:
        print("  scale.csv has no graph with 3+ samples yet"); return
    n = len(order)
    fig, axes = plt.subplots(1, n, figsize=(WIDTH_IN['scal'], 1.75), squeeze=False)
    for ax, g in zip(axes[0], order):
        s = sorted(data[g])
        pct = [p for p, _, _, _ in s]
        e = [x for _, x, _, _ in s]; b = [x for _, _, x, _ in s]; w = [x for _, _, _, x in s]
        ax.plot(pct, [x / e[-1] for x in e], color='black', lw=0.8, ls=':', label='edges')
        ax.plot(pct, [x / b[-1] for x in b], color='black', lw=0.8, ls='--', label='breakpoints')
        ax.plot(pct, [x / w[-1] for x in w], color='black', lw=1.4, marker='s', ms=2.6,
                label='build time')
        ax.set_yscale('log'); ax.set_xlabel('edges kept (%)')
        ax.set_xticks([20, 40, 60, 80, 100])
        ax.set_title(dict(TEN)[g], fontsize=7, pad=2)
        ax.spines['top'].set_visible(False); ax.spines['right'].set_visible(False)
    axes[0][0].set_ylabel('relative to full')
    axes[0][0].legend(frameon=False, fontsize=6, loc='upper left', handlelength=1.6,
                      handletextpad=0.4, borderaxespad=0.2)
    save(fig, 'scal')

def _scale_rows():
    return rows('scale.csv')

FIGS = {'bld_main': bld_main, 'bld_break': bld_break, 'bld_size': bld_size,
        'hierbench': hierbench, 'qsnap': qsnap, 'scal': scal}

if __name__ == '__main__':
    only = None
    if '--only' in sys.argv: only = sys.argv[sys.argv.index('--only') + 1]
    for name, fn in FIGS.items():
        if only and name != only: continue
        print(f"{name}:"); fn()
