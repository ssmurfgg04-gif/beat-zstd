#!/usr/bin/env bash
# beat-zstd self-test: fast quality gate that runs BEFORE the expensive
# experiments, in CI and locally. Fails loudly on:
#   - any build warning-as-error regression (builds with -Wall, checks exit)
#   - any tANS spread roundtrip failure (embedded, aborts on first failure)
#   - any lzmini parser decode/verify failure (embedded, per block)
#   - any bzrouter route/filter/dict roundtrip or inverse-filter failure
#   - summarize.py crashing on a real mini results dir
# Runtime target: < 2 minutes. The heavy Silesia run stays in run_all.sh.
set -euo pipefail
cd "$(dirname "$0")/.."
T=$(mktemp -d)
trap 'rm -rf "$T"' EXIT
fail() { echo "SELFTEST FAIL: $1"; exit 1; }

echo "== selftest: build (warnings visible)"
make -j2 all build/zstd-cli > "$T/build.log" 2>&1 || { cat "$T/build.log"; fail "build"; }
if grep -E "(src|deps)/.*(warning|error)" "$T/build.log" | grep -v "jobserver"; then fail "new compiler warnings"; fi
echo "build OK"

echo "== selftest: deterministic test data"
python3 scripts/gen_json.py "$T/corpus/extra" 0.5 200 >/dev/null
# synthetic 16-bit sensor-like file (interleaved shorts, smooth-ish) + noise file
export TDIR="$T"
python3 - << 'PYEOF'
import struct, random, os
random.seed(42)
T = os.environ["TDIR"]
os.makedirs(T + "/data", exist_ok=True)
with open(T + "/data/synth16.bin", "wb") as f:
    x = y = 0
    for i in range(2_000_000):
        x += random.randint(-9, 9); y += random.randint(-9, 9)
        f.write(struct.pack("<hh", x, y))
with open(T + "/data/noise.bin", "wb") as f:
    f.write(bytes(random.randrange(256) for _ in range(1 << 18)))
with open(T + "/data/text.txt", "wb") as f:
    w = ["the","quick","brown","fox","entropy","table","spread","beam","parse","route"]
    f.write(" ".join(random.choice(w) for _ in range(400_000)).encode())
PYEOF
echo "test data OK"

echo "== selftest: experiment A (tANS roundtrips on 3 streams x 4 spreads x 3 L)"
./build/tans_bench --csv "$T/data/text.txt" "$T/data/synth16.bin" "$T/data/noise.bin" > "$T/tans.csv" 2> "$T/tans.err" || { cat "$T/tans.err"; fail "tans"; }
[ "$(wc -l < "$T/tans.csv")" -ge 30 ] || fail "tans rows missing"
echo "tans OK ($(($(wc -l < "$T/tans.csv") - 1)) roundtrip-verified rows)"

echo "== selftest: experiment B (all parsers verify on 3 files)"
: > "$T/lzmini.csv"
for p in greedy lazy dp beam4 beam8 beam16; do
  for f in text.txt synth16.bin noise.bin; do
    ./build/lzmini "$T/data/$f" --parser "$p" --cap 8 >> "$T/lzmini.csv" 2>> "$T/lzmini.err" || { cat "$T/lzmini.err"; fail "lzmini $p $f"; }
  done
done
[ "$(wc -l < "$T/lzmini.csv")" -eq 18 ] || fail "lzmini rows missing"
echo "lzmini OK (18 verified parses)"

echo "== selftest: experiment C (probe, bench, dict train, route, filters)"
: > "$T/probe.csv"
for f in text.txt synth16.bin noise.bin; do ./build/bzrouter probe "$T/data/$f" >> "$T/probe.csv"; done
[ "$(wc -l < "$T/probe.csv")" -eq 3 ] || fail "probe rows"
./build/bzrouter bench "$T/data/synth16.bin" --levels "1,6,12" >> "$T/bench.csv" 2> "$T/bench.err" || fail "bench"
./build/bzrouter train "$T/corpus/extra/feed_small" "$T/feed.dict" 2> "$T/train.err" || { cat "$T/train.err"; fail "train"; }
[ -s "$T/feed.dict" ] || fail "dict empty"
./build/bzrouter bench "$T/corpus/extra/feed_small/rec00000.json" --levels "12" --dict "$T/feed.dict" >> "$T/bench.csv" 2>> "$T/bench.err" || fail "bench dict"
./build/bzrouter route "$T/data/synth16.bin" >> "$T/route.csv" 2> "$T/route.err" || { cat "$T/route.err"; fail "route v1"; }
./build/bzrouter route "$T/data/synth16.bin" --v2 >> "$T/route.csv" 2>> "$T/route.err" || fail "route v2"
./build/bzrouter route "$T/corpus/extra/feed_small/rec00000.json" --dict "$T/feed.dict" --v2 >> "$T/route.csv" 2>> "$T/route.err" || fail "route dict"
./build/bzrouter route "$T/data/noise.bin" --v2 >> "$T/route.csv" 2>> "$T/route.err" || fail "route noise"
./build/bzrouter filterbench "$T/data/synth16.bin" 12 >> "$T/filter.csv" 2> "$T/filter.err" || fail "filterbench"
echo "router OK (all routes roundtrip + inverse-filter verified)"

echo "== selftest: summarize.py on the mini results"
mkdir -p "$T/res"
cp "$T"/tans.csv "$T/res/tans.csv"
cp "$T"/lzmini.csv "$T/res/lzmini.csv"
cp "$T"/probe.csv "$T"/bench.csv "$T"/route.csv "$T"/filter.csv "$T/res/"
echo "AMD EPYC test | avx2 | 4" > "$T/res/cpu.txt"
python3 scripts/summarize.py "$T/res" > "$T/RESULTS.md" || fail "summarize"
grep -q "Experiment C" "$T/RESULTS.md" || fail "summarize output incomplete"
echo "summarize OK"
echo "SELFTEST PASS"
