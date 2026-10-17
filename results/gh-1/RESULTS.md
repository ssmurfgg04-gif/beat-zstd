# beat-zstd results (gh-1)

CPU: model name	: AMD EPYC 9V45 96-Core Processor | avx2 | avx512bw | avx512f | bmi2 | sse4_2 | 4
zstd: v1.5.7 (pinned release, static)

## Experiment C: router vs fixed levels (mixed corpus, 15 files, 246.30 MB in)

| approach | total out (MB) | ratio | total ctime (s) | total dtime (s) |
|---|---|---|---|---|
| -L1 | 87.43 | 0.3550 | 0.4 | 0.1 |
| -L3 | 80.79 | 0.3280 | 0.6 | 0.1 |
| -L6 | 75.53 | 0.3066 | 1.6 | 0.1 |
| -L9 | 73.19 | 0.2971 | 2.4 | 0.1 |
| -L12 | 71.83 | 0.2917 | 5.0 | 0.1 |
| -L16 | 68.74 | 0.2791 | 24.6 | 0.1 |
| -L19 | 66.37 | 0.2695 | 59.4 | 0.1 |
| router | 66.64 | 0.2705 | 45.4 | 0.1 |
| zstd --adapt (silesia only: 202.12 in) | 63.01 | 0.3117 | 0.6 | n/a |

Router total: 66.64 MB. Fixed levels that are both smaller AND faster: none.
Router vs -L19: size +0.40%, time -23.5%.

## Experiment C: dictionary on small files

### feed_small (feed-like JSON records)

| level | avg size nodict | avg size dict | dict gain | avg cspeed nodict | avg cspeed dict |
|---|---|---|---|---|---|
| 1 | 502.4 | 289.0 | 42.5% | 419 | 716 |
| 3 | 500.7 | 262.0 | 47.7% | 569 | 1039 |
| 6 | 488.0 | 256.3 | 47.5% | 719 | 1080 |
| 12 | 487.3 | 211.0 | 56.7% | 681 | 929 |
| 19 | 484.9 | 202.8 | 58.2% | 678 | 890 |
| router(dict) | - | 212.0 | - | - | - |

### text4k (4KB dickens chunks)

| level | avg size nodict | avg size dict | dict gain | avg cspeed nodict | avg cspeed dict |
|---|---|---|---|---|---|
| 1 | 2048.9 | 1886.0 | 8.0% | 756 | 593 |
| 3 | 2041.9 | 1620.7 | 20.6% | 862 | 584 |
| 6 | 1990.7 | 1562.1 | 21.5% | 921 | 537 |
| 12 | 1984.1 | 1473.8 | 25.7% | 839 | 528 |
| 19 | 1967.0 | 1414.7 | 28.1% | 838 | 562 |
| router(dict) | - | 1563.1 | - | - | - |

## Experiment A: tANS table spreads (real file bytes (order-0))

| L | fse | edf | balanced | random | edf vs fse |
|---|---|---|---|---|---|
| 8 | 1.126276 | 1.138061 | 1.141311 | 1.144382 | 0.011786 |
| 10 | 0.105610 | 0.113059 | 0.121756 | 0.127972 | 0.007449 |
| 12 | 0.026895 | 0.028133 | 0.028526 | 0.030053 | 0.001238 |

(mean excess bits/symbol over order-0 entropy; lower is better; all roundtrips verified)

## Experiment A: tANS table spreads (LZ literal streams)

| L | fse | edf | balanced | random | edf vs fse |
|---|---|---|---|---|---|
| 8 | 1.015365 | 1.029377 | 1.033779 | 1.035916 | 0.014012 |
| 10 | 0.110368 | 0.118046 | 0.127174 | 0.129984 | 0.007679 |
| 12 | 0.030031 | 0.030919 | 0.031719 | 0.033370 | 0.000887 |

(mean excess bits/symbol over order-0 entropy; lower is better; all roundtrips verified)

## Experiment B: parsers (fixed LZ cost model, bits/byte)

| file | greedy | lazy | dp | beam4 | beam8 | beam16 | beam16 vs lazy |
|---|---|---|---|---|---|---|---|---|
| dickens | 1.0385 | 1.0385 | 1.0347 | 1.0345 | 1.0345 | 1.0345 | -0.385% |
| feed.json | 1.0207 | 1.0207 | 1.0726 | 1.0052 | 1.0037 | 1.0035 | -1.685% |
| mozilla | 0.8197 | 0.8197 | 1.0414 | 0.7705 | 0.7636 | 0.7617 | -7.076% |
| mr | 0.9528 | 0.9528 | 1.0660 | 0.8581 | 0.8410 | 0.8366 | -12.196% |
| nci | 0.5321 | 0.5321 | 0.9791 | 0.4058 | 0.3758 | 0.3652 | -31.366% |
| ooffice | 0.9835 | 0.9835 | 1.0354 | 0.9660 | 0.9603 | 0.9593 | -2.461% |
| osdb | 1.0580 | 1.0580 | 1.0628 | 1.0539 | 1.0538 | 1.0538 | -0.397% |
| precomp.zst | 1.1249 | 1.1249 | 1.1249 | 1.1249 | 1.1249 | 1.1249 | +0.000% |
| random.bin | 1.1250 | 1.1250 | 1.1250 | 1.1250 | 1.1250 | 1.1250 | +0.000% |
| reymont | 1.0373 | 1.0373 | 1.0449 | 1.0265 | 1.0263 | 1.0262 | -1.070% |
| samba | 0.8273 | 0.8273 | 0.9641 | 0.7819 | 0.7767 | 0.7751 | -6.310% |
| sao | 1.0893 | 1.0893 | 1.0977 | 1.0860 | 1.0859 | 1.0859 | -0.312% |
| webster | 1.0126 | 1.0126 | 1.0129 | 1.0031 | 1.0027 | 1.0026 | -0.988% |
| x-ray | 1.0801 | 1.0801 | 1.0788 | 1.0791 | 1.0791 | 1.0791 | -0.093% |
| xml | 0.9525 | 0.9525 | 1.0153 | 0.9272 | 0.9255 | 0.9252 | -2.866% |
| **mean** | 0.9770 | 0.9770 | 1.0504 | 0.9498 | 0.9453 | 0.9439 | -4.480% |

(dp optimizes the offset-only model (no repcode pricing); greedy/lazy/beam include repcode pricing; beam is exact-DP-like only within its width)

## Experiment C: pre-filters on 16/32-bit binary files

| file | level | plain (MB) | delta | split2 | split4 | best | best vs plain |
|---|---|---|---|---|---|---|---|
| mr | 12 | 3.12 | 3.51 | 2.84 | 3.06 | split2 | -8.91% |
| mr | 19 | 2.96 | 2.96 | 2.70 | 2.96 | split2 | -8.91% |
| ooffice | 12 | 2.70 | 3.33 | 3.22 | 3.67 | plain | +0.00% |
| ooffice | 19 | 2.47 | 3.10 | 3.03 | 3.55 | plain | +0.00% |
| osdb | 12 | 3.07 | 3.45 | 3.77 | 4.54 | plain | +0.00% |
| osdb | 19 | 2.96 | 3.28 | 3.60 | 4.38 | plain | +0.00% |
| sao | 12 | 4.96 | 5.47 | 5.46 | 5.75 | plain | +0.00% |
| sao | 19 | 4.77 | 5.21 | 5.34 | 5.65 | plain | +0.00% |
| x-ray | 12 | 5.07 | 5.92 | 4.40 | 4.47 | split2 | -13.10% |
| x-ray | 19 | 4.89 | 4.53 | 4.32 | 4.39 | split2 | -11.66% |

## Notes

- Every tANS and lzmini run verifies a full decode roundtrip before being reported.
- Router rules are hand-set (README); filter choice is empirical per file on a 128KB sample.
- See raw CSVs alongside this file for per-file detail.
