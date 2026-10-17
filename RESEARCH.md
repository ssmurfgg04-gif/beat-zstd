# RESEARCH.md: what we test, and where each idea comes from

The premise under test: "if you rebuilt zstd today with 2024-2026 research,
what would actually help?" The GPU track is out of scope (runners are
CPU-only), so the heterogeneous/probe-and-route idea gets tested as a
CPU-only router.

## Papers and what we take from each

1. **Optimal tables for asymmetric numeral systems** - Steiner, De Vita,
   Bezati, arXiv:2504.18541 (2025). Proves table spreads for tANS that are
   optimal in discrepancy, giving the strongest proven bound on entropy
   loss; includes a deadline-scheduling construction.
   - We implement their earliest-deadline-first construction ("edf") in a
     tANS codec faithful to zstd's FSE table mechanics (same interval
     tiling), and compare against a clone of zstd's step-walk spread
     ("fse"), a balanced even-spacing spread, and a random control.
   - What would confirm the paper's value: edf consistently below fse on
     real streams, by a measurable margin.
   - What would temper it: quantization loss dominating layout loss on
     real data, and the ordering being noisy across table sizes.

2. **GPX: An Adaptive Heterogeneous (Multi-Tier) Architecture for
   High-Ratio Compression** - arXiv:2610.02236 (2026). Probe-and-route:
   a <15us structural decision probe selects per-block transforms; reports
   dominating zstd levels 10-14 on Silesia; beam sequence parser worth
   +0.084% alone.
   - CPU-only probe-and-route: our bzrouter (probe = order-0/order-1
     entropy + 8-byte shingle repeat rate; routes = level, dictionary,
     byte-split/delta pre-filter).
   - Beam parsing: our lzmini tests beam width 4/8/16 vs greedy/lazy/DP
     under one fixed cost model with repcodes.

3. **Match-length-dependent windows (WZIP/WLZ4)** - variable-window
   matching reported at up to +16% over LZ4HC. Out of scope for v1; noted
   as future work.

4. **FastLanes** - layout-matched encodings. The byte-split (plane split)
   pre-filter in bzrouter is the cheap CPU version of this idea for
   16/32-bit data (Silesia mr, x-ray, sao).

5. **Dictionary training (COVER/FastCOVER line)** - known 3-4x gains on
   small data. We reproduce with ZDICT_trainFromBuffer on feed-like JSON
   records and 4KB text chunks, and make the router dictionary-aware
   (tiny+dict routes to the dict path).

6. **AVX-512 VBMI classification / SIMD dispatch** - runner CPUs report
   their flags in every run; standard runners lack AVX-512, so SIMD
   dispatch is recorded as environment info, not benchmarked (zstd already
   does BMI2/AVX2 runtime dispatch internally).

7. **AEDS (2-state/5-state generalization of tANS)** - no public
   implementation found to test; the golden-ratio dominant-symbol case is
   included in the tANS synthetic suite as a boundary probe.

## Honest framing

- zstd is the result of a thousand small, correct decisions over a decade.
  Any single upgrade idea should be expected to move a fraction of a
  percent, not tens of percent. The interesting question is which
  fractions are real, and whether stacking them + routing beats stock
  levels in a Pareto sense.
- GPU numbers (500 GB/s on RTX 4090 etc.) are real but irrelevant on
  CPU-only runners; excluded by design.
- Where we cannot find a public artifact for a claim, we say so.
