# Indexing Temporal Edge Cores and Communities at Every Time Scale

Code for the paper. The temporal (k, Δ)-core counts, at each endpoint of an edge, the
edges within Δ of its timestamp. This repository builds two exact indexes over all values
of Δ at once and answers queries at any Δ:

- the **edge core index**: for each edge, its core number at Δ = 0 and one breakpoint
  per increase; it answers point queries (the core number at a given Δ) and onset
  queries (the smallest Δ at which the core number reaches a given k);
- the **community index**: the merge pairs U, one pair of edges per merge of two
  communities, each pair stored once across all k; it answers community queries
  (the community of an edge at a given k and Δ).

## Files

| file | what it does |
|---|---|
| `src/kcs.cpp` | builds the edge core index by one downward sweep over Δ: `Sweep` (no filter, `--nofilter`), `Sweep+` (strict support filter, `--hist`), `Sweep*` (certificates, default) |
| `src/stream_tool.cpp` | turns the construction stream into the edge-major query layout |
| `src/qtime.cpp` | point queries, onset queries, and every edge's core number at one Δ |
| `src/community_build.cpp` | builds the merge pairs U from the edge core index |
| `src/hier_index.cpp` | community queries over the merge pairs, timed against a traversal of the graph |
| `src/kcs_stream_format.hpp`, `src/kcs_index_format.hpp` | the on-disk layouts |

## Building

```sh
make
```

Requires a C++17 compiler. Every program is single-threaded. Peak memory is reported
from `/proc` on Linux.

## Input

One interaction per line, whitespace separated:

```
u v t
```

`u` and `v` are integer vertex ids and `t` is an integer timestamp. The graph is
undirected and may hold parallel edges at different timestamps.

## Running

```sh
bin/kcs graph.txt --stream graph.kcs.strm                  # Sweep*: the edge core index
bin/kcs graph.txt --stream graph.plus.strm --hist          # Sweep+
bin/kcs graph.txt --stream graph.sweep.strm --nofilter     # Sweep
bin/stream_tool finalize graph.kcs.strm graph.kcsidx3      # query layout
bin/qtime graph.kcsidx3 full <Delta>                       # every edge's core number at one Delta
bin/qtime graph.kcsidx3 batch                              # random point and onset queries
bin/community_build --strm=graph.kcs.strm --graph=graph.txt --out=U.bin
bin/hier_index --strm=graph.kcs.strm --graph=graph.txt --usweep=cells.csv --uq=60
```

`community_build` writes U as pairs of 32-bit edge ids and prints its size, build time,
and peak memory. `hier_index --usweep` rebuilds U level by level and, for a grid of
(k, Δ) cells, answers the same community queries with the merge pairs and with a
traversal of the graph.

## Baseline and data

The fixed-Δ baseline is EdgeCD (WSDM 2025); its code is the authors' own:
<https://gitlab.com/tgpublic/tgkd>.

The ten graphs of the paper are public and are not included: Amazon Home-and-Kitchen
(Amazon Reviews 2023) and Amazon Books (the 2014 release), Flickr (growth data) and
YouTube, Epinions ratings, DBLP coauthorship, Wikipedia talk, HEP-PH coauthorship,
Brightkite, and the Enron email corpus, each converted to the `u v t` format above.

## Citation

Wenqian Zhang, Yi Ding, Zebin Chen, Jingyi Song, Dong Wen, and Zhengyi Yang.
Indexing Temporal Edge Cores and Communities at Every Time Scale.
