#!/usr/bin/env bash
# Build the benchmark corpus: Silesia (zstd's own benchmark corpus) + extras.
# Tries several Silesia mirrors; falls back to a locally assembled corpus.
set -u
CORPUS=${1:-corpus}
mkdir -p "$CORPUS/silesia" "$CORPUS/extra"

try_silesia() {
  local ok=1
  for url in \
    "https://mattmahoney.net/dc/silesia.zip" \
    "http://sun.aei.polsl.pl/~sdeor/silesia.zip" \
    "https://sun.aei.polsl.pl/~sdeor/index/silesia.zip"; do
    echo "trying $url"
    if curl -sfL --retry 2 --max-time 600 -o /tmp/silesia.zip "$url"; then
      if unzip -q -o /tmp/silesia.zip -d "$CORPUS/silesia"; then
        ok=0; break
      fi
    fi
  done
  # github mirror as last resort (slow but reliable)
  if [ $ok -ne 0 ]; then
    echo "trying github mirror MiloszKrajewski/SilesiaCorpus"
    if git clone --depth 1 https://github.com/MiloszKrajewski/SilesiaCorpus /tmp/silrepo 2>/dev/null; then
      cp /tmp/silrepo/*.??? "$CORPUS/silesia/" 2>/dev/null || cp /tmp/silrepo/* "$CORPUS/silesia/" 2>/dev/null || true
      ok=0
    fi
  fi
  return $ok
}

if [ "${SKIP_SILESIA:-0}" = "1" ]; then
  echo "SKIP_SILESIA=1: building local mini corpus"
  mkdir -p "$CORPUS/silesia"
  [ -f "$CORPUS/silesia/dickens" ] || head -c 4000000 /usr/bin/* 2>/dev/null | head -c 4000000 > "$CORPUS/silesia/dickens"
  [ -f "$CORPUS/silesia/mozilla" ] || (cat /usr/bin/* 2>/dev/null | head -c 8000000) > "$CORPUS/silesia/mozilla"
  [ -f "$CORPUS/silesia/mr" ] || head -c 2000000 /dev/urandom > "$CORPUS/silesia/mr"
  [ -f "$CORPUS/silesia/xml" ] || python3 -c "
import random
random.seed(3)
w = ['alpha','beta','gamma','delta','epsilon']
with open('$CORPUS/silesia/xml','w') as f:
    for i in range(30000):
        f.write('<row id=\"%d\"><k>%s</k><v>%d</v></row>\n' % (i, random.choice(w), random.randint(0,999)))
"
elif [ -f "$CORPUS/silesia/dickens" ]; then
  echo "silesia already present"
else
  if try_silesia; then echo "silesia downloaded"; else echo "SILESIA DOWNLOAD FAILED - using fallback corpus"; fi
fi

# verify silesia files exist
nfiles=$(ls "$CORPUS/silesia" 2>/dev/null | grep -cv '^$' || true)
echo "silesia files: $nfiles"

# extras: incompressible, pre-compressed, feed-like JSON, small records
[ -f "$CORPUS/extra/random.bin" ] || head -c 8388608 /dev/urandom > "$CORPUS/extra/random.bin"
[ -f "$CORPUS/extra/precomp.zst" ] || head -c 8388608 "$CORPUS/silesia/mozilla" 2>/dev/null | build/zstd -19 -q -o "$CORPUS/extra/precomp.zst" || \
  head -c 8388608 /dev/urandom | build/zstd -19 -q -o "$CORPUS/extra/precomp.zst" 2>/dev/null || \
  head -c 8388608 /dev/urandom > "$CORPUS/extra/precomp.zst"
[ -f "$CORPUS/extra/feed.json" ] || python3 scripts/gen_json.py "$CORPUS/extra" 32 2000

# 4KB text chunks for the dictionary track (from dickens if available)
if [ -f "$CORPUS/silesia/dickens" ] && [ ! -d "$CORPUS/extra/text4k" ]; then
  mkdir -p "$CORPUS/extra/text4k"
  split -b 4096 -d -a 5 "$CORPUS/silesia/dickens" "$CORPUS/extra/text4k/chunk"
fi

echo "corpus ready:"
du -sh "$CORPUS"/* 2>/dev/null
exit 0
