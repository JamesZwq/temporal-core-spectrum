#!/usr/bin/env python3
"""Figure 8: the Twitter full-span case study -- breakpoint mass, deepest label by radius,
and conditional onset by label.

Reads only experiments/results/twitter-case-study-fullspan/, the directory produced by the
full-span build of the Shao et al. retweet graph.  Section 8's numbers come from the same
place and were re-derived from fullspan_metadata.json: 2,131,270 indexed edges, 265 ambiguous
keys resolved to fact-checking, 234,224,439 breakpoints, max coreness 2,067.

Drawn at exactly the width it is printed at (\\textwidth), so the font sizes below are the
sizes on the page; the paper's body is 9pt.  Do not add bbox_inches='tight' to the save:
it crops the canvas below the slot and \\includegraphics then scales the type back up.
"""
import csv
from pathlib import Path

import matplotlib as mpl
import matplotlib.pyplot as plt


ROOT = Path(__file__).resolve().parents[2]
DATA = ROOT / "experiments/results/twitter-case-study-fullspan"
OUT = ROOT / "papers/temporal-spectrum/figures/twitter_fullspan.pdf"
DAY = 86400.0

mpl.rcParams.update(
    {
        "font.family": "serif",
        "font.size": 8,
        "axes.titlesize": 8,
        "axes.labelsize": 8,
        "xtick.labelsize": 7.6,
        "ytick.labelsize": 7.6,
        "legend.fontsize": 7.6,
        "pdf.fonttype": 42,
        "ps.fonttype": 42,
    }
)


def rows(name):
    with (DATA / name).open(newline="") as handle:
        return list(csv.DictReader(handle))


landmarks = rows("fullspan_breakpoint_landmarks.csv")
radius = rows("fullspan_radius_summary.csv")
onset = rows("fullspan_onset_summary.csv")

fig, axes = plt.subplots(1, 3, figsize=(506.295/72, 2.15))

# (a) Cumulative breakpoint mass.
ax = axes[0]
x = [max(float(r["delta_threshold"]) / DAY, 1.0 / DAY) for r in landmarks]
y = [100.0 * float(r["breakpoint_fraction_at_or_below"]) for r in landmarks]
ax.plot(x, y, color="black", marker="o", markersize=2.6, linewidth=1.2)
base_x = 381.0 / DAY
base_y = 100.0 * next(
    float(r["breakpoint_fraction_at_or_below"])
    for r in landmarks
    if r["delta_threshold"] == "381"
)
ax.scatter([base_x], [base_y], color="#b2182b", marker="D", s=22, zorder=3)
ax.annotate(
    "381 s: 0.542%",
    xy=(base_x, base_y),
    xytext=(0.002, 18),
    textcoords="data",
    arrowprops={"arrowstyle": "-", "lw": 0.7, "color": "#b2182b"},
    color="#b2182b",
)
ax.set_xscale("log")
ax.set_ylim(-2, 104)
ax.set_xlabel(r"radius $\Delta$ (days)")
ax.set_ylabel("cumulative breakpoints (%)")
ax.set_title("(a) 99.46% of changes occur later", loc="left")
ax.grid(axis="y", color="0.88", linewidth=0.5)

# (b) Which label reaches the deepest level.
ax = axes[1]
rr = [r for r in radius if float(r["delta"]) > 0]
x = [float(r["delta"]) / DAY for r in rr]
claim = [float(r["max_claim_core"]) for r in rr]
fact = [float(r["max_nonclaim_core"]) for r in rr]
ax.plot(x, claim, color="black", linewidth=1.3, label="claim")
ax.plot(x, fact, color="#2166ac", linewidth=1.3, linestyle="--", label="fact-checking")
ax.axvspan(8, 16, color="0.88", zorder=0)
ax.text(11.3, 18, "reversal\nband", ha="center", va="bottom", color="0.25")
ax.set_xscale("log")
ax.set_yscale("log")
ax.set_xlabel(r"radius $\Delta$ (days)")
ax.set_ylabel("maximum coreness")
ax.set_title("(b) top label reverses at 8--16 days", loc="left")
ax.legend(frameon=False, loc="upper left")
ax.grid(axis="y", color="0.88", linewidth=0.5)

# (c) Median onset by label.
ax = axes[2]
for label, color, marker, linestyle in [
    ("claim", "black", "o", "-"),
    ("fact_checking", "#2166ac", "s", "--"),
]:
    selected = [
        r
        for r in onset
        if r["label"] == label and r["onset_median"] not in ("", None)
    ]
    xx = [int(r["k"]) for r in selected]
    yy = [float(r["onset_median"]) / DAY for r in selected]
    ax.plot(
        xx,
        yy,
        color=color,
        marker=marker,
        markersize=2.8,
        linewidth=1.2,
        linestyle=linestyle,
        label=label.replace("_", "-"),
    )
ax.set_xscale("log", base=2)
ax.set_yscale("log")
ax.set_xlabel("target level $k$")
ax.set_ylabel("median onset (days)")
ax.set_title("(c) labels reach levels at different radii", loc="left")
ax.legend(frameon=False, loc="lower right")
ax.grid(axis="y", color="0.88", linewidth=0.5)

for ax in axes:
    ax.spines["top"].set_visible(False)
    ax.spines["right"].set_visible(False)

fig.subplots_adjust(left=0.075, right=0.995, bottom=0.22, top=0.88, wspace=0.43)
fig.tight_layout(pad=0.3)
fig.savefig(OUT, metadata={"CreationDate": None})
