# Beyond a Fixed Radius: Indexing Temporal Cores and Communities

Code and measured results for the temporal core spectrum: an exact index over the
temporal (k, Δ)-core that makes the radius Δ a query dimension instead of an input.

One build answers every radius. From the same index a query can ask for a
fixed-radius core, the least radius at which an edge reaches a level, a
radius-robust score, or the community around a queried interaction at a queried
scale — none of which needs another decomposition.

## What is here

```
src/        the builders and the index
figures/    every figure in the paper, and the script that draws it
results/    the CSV behind every number in the paper
```

### `src/`

| file | what it is |
|---|---|
| `kcs.cpp` | the three downward sweeps — `Sweep⁰` (unfiltered), `Sweep` (strict support filter), and `Sweep*` (relaxed certificate filter). Selected by flag; see below. |
| `hier_index.cpp` | the pair certificate: `Merges` builds it, `CommunitySearch` walks it, and a third arm walks the original graph for comparison. |
| `kcs_pack.cpp` | converts a builder's output stream to the binary index form. |
| `kcs_verify.cpp` | checks a binary index against the stream it came from. |
| `kcs_stream_format.hpp`, `kcs_format.hpp`, `kcs_reader.hpp`, `kcs_index_format.hpp` | the on-disk formats. |

### Input format

One interaction per line, whitespace separated:

```
u v t
```

`u` and `v` are integer vertex ids and `t` is an integer timestamp. The graph is
undirected and may carry parallel edges at different timestamps. Both directions
may appear; duplicates are collapsed.

## Building

```sh
make            # builds kcs, hier_index, kcs_pack, kcs_verify into bin/
```

Requires a C++17 compiler. Everything is single-threaded by design — the paper's
timings are single-threaded and the algorithms are not parallelised.

## Running

Build the full spectrum of a graph:

```sh
bin/kcs data/enron.txt --relax                 # Sweep*, the relaxed certificate filter
bin/kcs data/enron.txt                         # Sweep,  the strict support filter
bin/kcs data/enron.txt --nofilter              # Sweep0, no filter
```

Build the pair certificate over a finished spectrum and sweep community queries
over it:

```sh
bin/hier_index --strm=enron.kcs.strm --graph=data/enron.txt \
               --usweep=out.csv --label=ENR --uq=60
```

`--usweep` writes one row per (level, radius-quantile) cell: the answer size, the
slots and index probes each arm reads, and the wall time of each arm.

## Figures

```sh
cd figures
python3 make_running_example.py     # Figures 1-3, from runex.py
python3 make_figures.py             # the experiment plots
```

`runex.py` holds the paper's 18-edge running example and recomputes its spectrum
and certificate from scratch; the figures are drawn from it rather than placed by
hand. Its assertions pin the result against the real builder
(`κ_max=4`, `C₀=62`, `44` breakpoints, `|U|=26`).

Every figure is drawn at exactly the width it is printed at, so the type size in
the source is the type size on the page.

## Results

`results/` carries the CSVs the paper's numbers are read from. The ones the paper
cites by name:

| file | what it holds |
|---|---|
| `usweep4.csv` | the certificate sweep: 10 graphs × 42 cells, sizes, slot counts and per-arm timings |
| `scale.csv` | builds on 20–100% uniform edge samples |
| `qtime.csv` | query timings |
| `build.csv`, `build2.csv`, `build_detail.csv` | full-span builds, per-phase breakdown |
| `linegraph.csv` | Σₓ C(dₓ,2) per graph, against |U| |

## Baseline

The fixed-radius state of the art is EdgeCD (WSDM'25). Its code is the authors'
own and is not vendored here: <https://gitlab.com/tgpublic/tgkd>

## Datasets

All ten graphs are public. They are not vendored here — each is a `u v t` file
derived from its published source:

Amazon Home-and-Kitchen and Books (McAuley et al.), Flickr and YouTube (Mislove
et al.), Epinions (Massa and Avesani), DBLP coauthorship, Wikipedia talk
(Paranjape et al.), Brightkite (Cho et al.), HEP-PH citations (Leskovec et al.),
and the Enron email corpus (Klimt and Yang).
