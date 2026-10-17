#!/usr/bin/env bash
# beat-zstd full experiment run. Usage: scripts/run_all.sh [runid]
# Writes results/<runid>/*.csv and RESULTS.md.
set -u
cd "$(dirname "$0")/.."
RUNID=${1:-run-$(date +%Y%m%d-%H%M%S)}
RES="results/$RUNID"
mkdir -p "$RES"

echo "== beat-zstd run $RUNID =="
grep -m1 "model name" /proc/cpuinfo > "$RES/cpu.txt" 2>/dev/null || true
grep -m1 flags /proc/cpuinfo | grep -o "avx2\|avx512f\|avx512bw\|bmi2\|sse4_2" | sort -u >> "$RES/cpu.txt" 2>/dev/null || true
nproc >> "$RES/cpu.txt"
cat "$RES/cpu.txt"

# ---- corpus ----
bash scripts/corpus.sh corpus || true

LEVELS="1,3,6,9,12,16,19"
BENCH_FILES=""
for f in corpus/silesia/*; do
  case "$f" in *.zip|*.txt) ;; *) BENCH_FILES="$BENCH_FILES $f";; esac
done
[ -f corpus/extra/random.bin ] && BENCH_FILES="$BENCH_FILES corpus/extra/random.bin"
[ -f corpus/extra/precomp.zst ] && BENCH_FILES="$BENCH_FILES corpus/extra/precomp.zst"
[ -f corpus/extra/feed.json ] && BENCH_FILES="$BENCH_FILES corpus/extra/feed.json"
echo "bench files: $BENCH_FILES"

# ---- experiment A: tANS table spreads ----
echo "== A: tans =="
./build/tans_bench --synth > "$RES/tans_synth.csv" 2>/dev/null
# file bytes (order-0) - need --csv flag on files
./build/tans_bench --csv $BENCH_FILES > "$RES/tans.csv" 2>"$RES/tans.err"
# LZ literal streams (zstd-like literal distributions)
for f in $BENCH_FILES; do
  b=$(basename "$f")
  ./build/lzmini "$f" --parser lazy --cap 16 --dump-literals "$RES/lit-$b.lit" >/dev/null 2>&1
done
./build/tans_bench --csv "$RES"/lit-*.lit > "$RES/tans_lit.csv" 2>>"$RES/tans.err"

# ---- experiment B: parsers ----
echo "== B: lzmini =="
for f in $BENCH_FILES; do
  for p in greedy lazy dp beam4 beam8 beam16; do
    ./build/lzmini "$f" --parser "$p" --cap 16 >> "$RES/lzmini.csv" 2>>"$RES/lzmini.err"
  done
done

# ---- experiment C: router / dict / filters ----
echo "== C: router =="
for f in $BENCH_FILES; do
  ./build/bzrouter probe "$f" >> "$RES/probe.csv"
done

if [ -d corpus/extra/feed_small ] && [ "$(ls corpus/extra/feed_small | wc -l)" -gt 100 ]; then
  ./build/bzrouter train corpus/extra/feed_small "$RES/feed.dict" 2>>"$RES/dict.err"
fi
if [ -d corpus/extra/text4k ] && [ "$(ls corpus/extra/text4k | wc -l)" -gt 100 ]; then
  ./build/bzrouter train corpus/extra/text4k "$RES/text.dict" 2>>"$RES/dict.err"
fi

# level sweep on the mixed corpus
for f in $BENCH_FILES; do
  ./build/bzrouter bench "$f" --levels "$LEVELS" >> "$RES/bench.csv" 2>>"$RES/bench.err"
done

# dict benches on small-file corpora (sample 150 files each)
for corp in feed_small text4k; do
  d="corpus/extra/$corp"
  dict="$RES/${corp%_small*}.dict"; [ "$corp" = "text4k" ] && dict="$RES/text.dict"; [ "$corp" = "feed_small" ] && dict="$RES/feed.dict"
  if [ -d "$d" ] && [ -f "$dict" ]; then
    ls "$d" | shuf --random-source=<(yes) | head -150 | while read -r fn; do
      ./build/bzrouter bench "$d/$fn" --levels "1,3,6,12,19" >> "$RES/bench.csv" 2>>"$RES/bench.err"
      ./build/bzrouter bench "$d/$fn" --levels "1,3,6,12,19" --dict "$dict" >> "$RES/bench.csv" 2>>"$RES/bench.err"
    done
  fi
done

# router decisions on the mixed corpus (v1 = hand-set rules, v2 = sample-calibrated)
for f in $BENCH_FILES; do
  ./build/bzrouter route "$f" >> "$RES/route.csv" 2>>"$RES/route.err"
  ./build/bzrouter route "$f" --v2 >> "$RES/route.csv" 2>>"$RES/route.err"
done
# router on small files with dict (same samples)
for corp in feed_small text4k; do
  d="corpus/extra/$corp"
  dict="$RES/${corp%_small*}.dict"; [ "$corp" = "text4k" ] && dict="$RES/text.dict"; [ "$corp" = "feed_small" ] && dict="$RES/feed.dict"
  if [ -d "$d" ] && [ -f "$dict" ]; then
    ls "$d" | shuf --random-source=<(yes) | head -150 | while read -r fn; do
      ./build/bzrouter route "$d/$fn" --dict "$dict" >> "$RES/route.csv" 2>>"$RES/route.err"
      ./build/bzrouter route "$d/$fn" --dict "$dict" --v2 >> "$RES/route.csv" 2>>"$RES/route.err"
    done
  fi
done

# zstd --adapt competitor on silesia files (CLI)
if [ -x build/zstd ]; then
  for f in corpus/silesia/*; do
    case "$f" in *.zip|*.txt) continue;; esac
    s0=$(date +%s%N)
    build/zstd -q -f "$f" -o /tmp/adapt.zst --adapt=min=1,max=19 2>/dev/null || build/zstd -q -f "$f" -o /tmp/adapt.zst --adapt 2>/dev/null
    s1=$(date +%s%N)
    sz=$(stat -c %s /tmp/adapt.zst 2>/dev/null || echo 0)
    echo "$f,adapt,$sz,$(( (s1 - s0) / 1000000 ))" >> "$RES/adapt.csv"
    rm -f /tmp/adapt.zst
  done
fi

# filters on binary-ish silesia files at two levels
for f in corpus/silesia/mr corpus/silesia/x-ray corpus/silesia/sao corpus/silesia/osdb corpus/silesia/ooffice; do
  [ -f "$f" ] || continue
  ./build/bzrouter filterbench "$f" 12 >> "$RES/filter.csv" 2>>"$RES/filter.err"
  ./build/bzrouter filterbench "$f" 19 >> "$RES/filter.csv" 2>>"$RES/filter.err"
done

# ---- summarize ----
python3 scripts/summarize.py "$RES"
echo "done: $RES/RESULTS.md"
