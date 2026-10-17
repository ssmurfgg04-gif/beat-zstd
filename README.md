# beat-zstd

Research question: can anything we build beat heavily optimized zstd, on
zstd's own benchmark corpus (Silesia), on plain CPU GitHub runners?

This is not a "rewrite zstd" project. It is a controlled test of the
individual upgrade ideas from the 2024-2026 compression literature, plus a
probe-and-route architecture (the GPX idea, CPU-only). Every claim gets a
measurement. We publish wins and losses the same way.

Baseline: zstd v1.5.7, pinned release, static build, stock settings.

## Experiments

| id | question | tool |
|---|---|---|
| A | Do provably-optimal tANS table spreads (arXiv:2504.18541) measurably beat zstd's heuristic FSE spread on real data? | `src/tans_bench.c` |
| B | Does beam-search parsing beat greedy/lazy/optimal parsing under a fixed LZ cost model with repcodes? | `src/lzmini.c` |
| C | Does a cheap probe-and-route layer (level + dictionary + byte-split/delta pre-filter) beat every fixed zstd level on a mixed corpus? | `src/bzrouter.c` |

All experiments verify full decode roundtrips before reporting numbers.
See RESEARCH.md for the paper trail.

## Results

Latest run's `RESULTS.md` lands in `results/gh-*/` after the bench workflow
runs. Read it before believing anything in the README.

## Method notes

- Silesia (211 MB, 12 files) is the primary corpus, plus extras: an
  incompressible random file, an already-zstd-compressed file, a 32 MB
  feed-like JSON stream, small JSON records, and 4 KB text chunks.
- Timing is single-threaded per operation on GitHub-hosted runners; CPU
  model and SIMD flags are recorded with every run. Runner-to-runner noise
  exists; treat small speed deltas with suspicion, size deltas are exact.
- The tANS experiment holds normalized counts constant across spreads, so
  any size difference is pure table layout. The parser experiment holds
  the cost model constant, so any difference is pure parsing.
- lzmini's cost model is deliberately simple. Its absolute ratio is NOT
  comparable to zstd; the parser deltas are.
- Router rules are hand-set (below), not fitted to the corpus. The filter
  choice is empirical per file (compress a 128 KB sample 4 ways at level
  6, pick the smallest, plain wins ties within 1%).

Router rules:
```
tiny (<4KB) + dict available -> level 12 + dict
h0 > 7.85 and shingle < 2%   -> level 1        (incompressible)
size >= 8MB                  -> level 19
shingle > 55%                -> level 17
shingle > 30%                -> level 12
h1 < 6.5 (texty)             -> level 16
else                         -> level 6
```

## Layout

```
src/tans_bench.c   experiment A (entropy table spreads)
src/lzmini.c       experiment B (parsers)
src/bzrouter.c     experiment C (probe/route/dict/filters)
scripts/corpus.sh  Silesia + extras, with mirrors and fallback
scripts/run_all.sh orchestrates everything, writes results/<run>/
scripts/summarize.py RESULTS.md generator
.github/workflows/bench.yml  runner bench
```

## Reproduce locally

```
make
bash scripts/corpus.sh corpus
bash scripts/run_all.sh local
```

Heavy runs belong on the runner (that is the point). The local 2-core box
is only for smoke tests.
