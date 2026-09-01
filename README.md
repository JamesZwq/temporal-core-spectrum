# Beyond a Fixed Radius: Indexing Temporal Cores and Communities

Code and measured results for the temporal core spectrum: an exact index over the
temporal (k, Δ)-core that makes the radius Δ a query dimension instead of an input.

One build answers every radius. From the same index a query can ask for an
edge's coreness at one radius, the least radius at which an edge reaches a
level, a complete snapshot, or the community around a queried interaction at a
queried scale. None of these queries needs another decomposition.

## What is here

```
src/        the builders and the index
figures/    every figure in the paper, and the script that draws it
results/    the CSV behind every number in the paper
```

### `src/`

| file | what it is |
|---|---|
| `kcs.cpp` | the three downward sweeps: `Sweep⁰` (unfiltered), `Sweep` (strict support filter), and `Sweep*` (the two-certificate scheduler). Selected by flag; see below. |
| `hier_index.cpp` | the pair certificate: `Merges` builds it, `CommunitySearch` walks it, and a third arm walks the original graph for comparison. |
| `stream_tool.cpp` | verifies the construction stream and transposes it into the edge-major KCSIDX3 layout; it never recomputes coreness. |
| `kcs_pack.cpp` | converts a text or finalized edge-major index to the compact `.kcsb` form; it never recomputes coreness. |
| `kcs_verify.cpp` | differentially checks a compact `.kcsb` index against the builder's text export. |
| `kcs_stream_format.hpp`, `kcs_format.hpp`, `kcs_reader.hpp`, `kcs_index_format.hpp` | the on-disk formats. |

## One logical index, several physical layouts

The paper's **edge core index** is one logical object. For each temporal edge,
it contains the coreness at the minimum indexed radius and the sorted list of
later breakpoints. `Sweep⁰`, `Sweep`, and `Sweep*` differ only in how they
compute those records. Once a builder finishes, the records can be serialized
in several layouts without running another decomposition:

```text
                             same base values and breakpoints
                                           |
builder --stream  --> KCSSTRM2 (.strm) ----+---- finalize / transpose
                                           |              |
builder --dump    --> text (.kcs_index)     |          KCSIDX3
                                           |              |
                                           +---- kcs_pack-+
                                                       |
                                                  KCSB (.kcsb)
```

| layout | purpose | organization |
|---|---|---|
| **KCSSTRM2** (`.strm`) | bounded-memory construction output and compact persisted form | one seed value per edge, followed by radius-major groups of changed edges |
| **KCSIDX3** | checked edge-major interchange and query layout used by the query-timing harness | one directory entry per edge and one fixed-width record per breakpoint |
| **KCSB** (`.kcsb`) | compact, memory-mapped query layout | width-packed per-edge breakpoint arrays; `kcs_reader.hpp` answers point and onset queries in `O(log b_e)` time |

Finalization is an external sort and transpose from radius-major events to
edge-major lists. It does not run the temporal-core algorithm again and does
not change any base value or breakpoint. `kcs_pack` likewise changes only the
physical encoding. Its two accepted inputs, text and KCSIDX3, must produce the
same `.kcsb` bytes.

This distinction explains two columns in the released results:

- `results/spectrum.csv:stream_bytes` is the size of the self-contained,
  compressed KCSSTRM2 file written during construction.
- `results/qtime.csv:index_bytes` is the size of the KCSIDX3 file used by the
  stable query-timing reader.

These byte counts describe two serializations of the same logical index, not
two algorithms or two computed spectra. KCSIDX3 is larger because it includes
an edge directory, fixed-width records, checksums, and a 96-bit prefix field
used by the generic artifact reader. The paper's point and onset queries read
the same ordered per-edge breakpoint list in either edge-major layout.

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
make            # builds kcs, hier_index, stream_tool, kcs_pack, and kcs_verify
```

Requires a C++17 compiler. Everything is single-threaded by design — the paper's
timings are single-threaded and the algorithms are not parallelised.

## Running

Build the full spectrum of a graph:

```sh
bin/kcs graph.txt --stream graph.kcs.strm                    # Sweep*, default
bin/kcs graph.txt --stream graph.sweep.strm --hist           # Sweep, strict filter
bin/kcs graph.txt --stream graph.sweep0.strm --nofilter      # Sweep⁰, no filter
```

To materialize the two edge-major layouts from the default stream:

```sh
bin/stream_tool verify graph.kcs.strm
bin/stream_tool finalize graph.kcs.strm graph.kcsidx3
bin/kcs_pack graph.kcsidx3 --graph graph.txt -o graph.kcsb
```

The first command validates the KCSSTRM2 structure and checksums. Finalization
validates the stream again while transposing its records, and `kcs_pack`
validates KCSIDX3 before writing and then rereads every packed edge and
breakpoint. Thus the serialization pipeline fails loudly if any stage changes
or corrupts the logical index.

The optional text path is useful for inspection and differential checking:

```sh
bin/kcs graph.txt --dump
bin/kcs_pack graph.txt.kcs_index -o graph.kcsb
bin/kcs_verify graph.txt.kcs_index graph.kcsb
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
| `qtime.csv` | query timings on the KCSIDX3 edge-major layout |
| `spectrum.csv` | full-span Sweep* construction, work counters, and KCSSTRM2 size |
| `build.csv`, `build2.csv`, `build_detail.csv` | full-span builds and per-phase breakdown |
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
