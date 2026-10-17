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

### Run gh-1 headline findings (real Silesia, AMD EPYC 9V45, zstd v1.5.7)

- **Router v1 (hand-set rules, no corpus fitting): no fixed zstd level is
  both smaller AND faster.** 66.64 MB total vs -L19's 66.37 MB (+0.40%)
  at 24% less compression time. It sits on the Pareto frontier between
  -L16 and -L19.
- **Filters beat zstd outright on 16-bit files:** split2 (byte-plane split)
  + zstd -12 beats plain zstd -19 by 8.9% on mr and 13.1% on x-ray. The
  router found these wins by itself via a 128KB sample probe.
- **Dictionaries reproduced:** 58% smaller feed-like JSON records, 28%
  smaller 4KB text chunks.
- **Optimal tANS tables (arXiv:2504.18541) did NOT beat zstd's heuristic
  spread on real data.** The EDF construction lost to the FSE step-walk at
  every table size (L=8: +0.0118 bits/sym worse, L=12: +0.0012 worse).
  Honest negative result: discrepancy-optimality does not translate to
  real-world bits. zstd's heuristic is empirically the best of the four
  spreads tested, at every L.
- **Beam search parsing beat lazy parsing by 4.5% mean bits/byte** under a
  fixed LZ cost model with repcodes (nci: -31%, mr: -12%, mozilla: -7%).
  Far larger than GPX's +0.084% claim, but against a simple lazy parser;
  zstd's tuned optimal parser would close much of the gap.
- zstd --adapt landed at L3-L6 territory (63.0 MB / 0.6s on Silesia) -
  fast but far from the ratio frontier.

Run gh-2 adds: shingle-probe bug fix (random data was faking a 14% repeat
rate, costing the router a wasted level-17 pass on noise), a sample-
calibrated router v2 (GPX-style: the codec probes itself on a 1MB sample
at levels 6/12/19 and routes), and results committed to the repo.

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
