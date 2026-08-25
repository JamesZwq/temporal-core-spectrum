#!/usr/bin/env python3
"""Figure 9: Philip S. Yu and twelve high-frequency DBLP coauthors, at radius 2 and radius 20.

Reads only experiments/results/dblp-case-study/ego.json.  The pair scores it draws are the
maximum coreness over each pair's temporal coauthorship edges, and the companion table in
Section 8 comes from the same file; onset_summary.csv there carries the 41,064 edges that
reach level 30 with onsets from 1 to 37 years.

Drawn at exactly the width it is printed at (0.92\\textwidth), so the font sizes below are
the sizes on the page; the paper's body is 9pt.  Do not add bbox_inches='tight' to the save:
it crops the canvas below the slot and \\includegraphics then scales the type back up.
"""

import argparse
import json
import math
import os
import tempfile
from pathlib import Path

os.environ.setdefault("MPLCONFIGDIR", str(Path(tempfile.gettempdir()) / "edgecore-matplotlib"))
os.environ.setdefault("XDG_CACHE_HOME", str(Path(tempfile.gettempdir()) / "edgecore-cache"))

import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt
from matplotlib.patches import Circle


ROOT = Path(__file__).resolve().parents[1]
DEFAULT_DATA = ROOT / "experiments/results/dblp-case-study/ego.json"
DEFAULT_OUTPUT = ROOT / "papers/temporal-spectrum/figures/casestudy.pdf"

SHORT_NAME = {
    "Ming-Syan Chen": "M.-S. Chen",
    "Charu C. Aggarwal": "Aggarwal",
    "Kun-Lung Wu": "K.-L. Wu",
    "Wei Fan": "Fan",
    "Jiawei Han": "Han",
    "De-Nian Yang": "Yang",
    "Chuan Shi": "Shi",
    "Sihong Xie": "Xie",
    "Senzhang Wang": "S. Wang",
    "Xifeng Yan": "Yan",
    "Bing Liu": "Liu",
    "Ke Wang": "K. Wang",
}


def parse_args():
    parser = argparse.ArgumentParser()
    parser.add_argument("--data", type=Path, default=DEFAULT_DATA)
    parser.add_argument("--output", type=Path, default=DEFAULT_OUTPUT)
    return parser.parse_args()


def shade(value, low=8, high=47):
    ratio = max(0.0, min(1.0, (value - low) / (high - low)))
    gray = 0.93 - 0.82 * ratio
    return (gray, gray, gray)


def text_color(value, low=8, high=47):
    return "white" if (value - low) / (high - low) > 0.55 else "#1a1a1a"


def main():
    args = parse_args()
    data = json.loads(args.data.read_text())
    ego = data["ego"]
    coauthors = data["coauthors"]

    position = {ego: (0.0, 0.0)}
    for i, coauthor in enumerate(coauthors):
        angle = math.pi / 2 - 2 * math.pi * i / len(coauthors)
        position[coauthor["name"]] = (math.cos(angle), math.sin(angle))

    plt.rcParams.update({
        "font.family": "serif",
        "font.size": 8,
        "pdf.fonttype": 42,
        "ps.fonttype": 42,
    })
    figure, axes = plt.subplots(1, 2, figsize=(506.295/72, 2.75))   # the whole figure* slot
    panels = [
        ("c_narrow", data["narrow_radius_years"], r"$c_e=10$--$19$"),
        ("c_wide", data["wide_radius_years"], r"$c_e=33$--$46$"),
    ]

    for panel, (key, radius, subtitle) in enumerate(panels):
        axis = axes[panel]
        axis.set_xlim(-1.66, 1.66)
        axis.set_ylim(-1.86, 1.6)
        axis.set_aspect("equal")
        axis.axis("off")

        for coauthor in coauthors:
            x, y = position[coauthor["name"]]
            axis.plot([0, x], [0, y], color="#c8ccd0", linewidth=0.6, zorder=1)
        for first, second in data["coauthor_links"]:
            x1, y1 = position[first]
            x2, y2 = position[second]
            axis.plot([x1, x2], [y1, y2], color="#dfe2e5", linewidth=0.5, zorder=1)

        for coauthor in coauthors:
            name = coauthor["name"]
            value = coauthor[key]
            x, y = position[name]
            axis.add_patch(Circle((x, y), 0.20, facecolor=shade(value),
                                  edgecolor="#111111", linewidth=0.9, zorder=3))
            axis.text(x, y, str(value), ha="center", va="center", fontsize=6.6,
                      color=text_color(value), fontweight="bold", zorder=4)
            axis.text(x * 1.34, y * 1.34, SHORT_NAME[name], ha="center", va="center",
                      fontsize=6.0, color="#33383d", zorder=4)

        axis.add_patch(Circle((0, 0), 0.26, facecolor="white", edgecolor="#9b2226",
                              linewidth=2.0, zorder=5))
        axis.text(0, 0, "Yu", ha="center", va="center", fontsize=7.2,
                  color="#9b2226", fontweight="bold", zorder=6)
        axis.set_title(f"({chr(97 + panel)}) $\\Delta={radius}$ years", fontsize=7.4)
        axis.text(0, -1.72, subtitle, ha="center", va="center", fontsize=6.9,
                  color="#33383d", fontstyle="italic")

    figure.subplots_adjust(left=0.01, right=0.99, top=0.90, bottom=0.02, wspace=0.05)
    args.output.parent.mkdir(parents=True, exist_ok=True)
    figure.tight_layout(pad=0.3)
    figure.savefig(args.output)
    plt.close(figure)
    print(args.output)


if __name__ == "__main__":
    main()
