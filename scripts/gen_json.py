#!/usr/bin/env python3
"""Generate a feed-like JSON corpus: one big stream file + a small-records dir.
Deterministic (seeded), so local and runner results are comparable."""
import json, os, random, sys

def main():
    outdir = sys.argv[1] if len(sys.argv) > 1 else "corpus/extra"
    big_mb = float(sys.argv[2]) if len(sys.argv) > 2 else 32.0
    nsmall = int(sys.argv[3]) if len(sys.argv) > 3 else 2000
    os.makedirs(outdir + "/feed_small", exist_ok=True)
    random.seed(20261017)

    users = ["alice", "bob", "carol", "dave", "eve", "frank", "grace", "heidi", "ivan", "judy"]
    acts = ["push", "pull", "star", "fork", "issue", "comment", "merge", "watch", "release", "tag"]
    repos = ["facebook/zstd", "tencent/libco", "chen3feng/muduo", "arjaythedev/tdsc",
             "ssmurfgg04-gif/beat-zstd", "ssmurfgg04-gif/coro-arena", "django/django",
             "pytorch/pytorch", "rust-lang/rust", "torvalds/linux"]
    branches = ["main", "dev", "feat-router", "fix-ans-tables", "bench-cleanup"]
    words = ["the", "quick", "brown", "fox", "jumps", "over", "lazy", "dog", "entropy",
             "table", "spread", "beam", "parse", "ratio", "speed", "corpus", "probe", "route"]

    def rec(i):
        return {
            "id": 900000 + i, "user": random.choice(users), "action": random.choice(acts),
            "repo": random.choice(repos),
            "ts": 1790000000 + i * 37,
            "score": random.randint(0, 100),
            "meta": {
                "branch": random.choice(branches),
                "sha": "%08x" % random.getrandbits(32),
                "msg": " ".join(random.choices(words, k=random.randint(3, 8))),
            },
        }

    # big single stream (many records, one file)
    target = int(big_mb * 1048576)
    with open(outdir + "/feed.json", "w") as f:
        i, sz = 0, 0
        while sz < target:
            s = json.dumps(rec(i), separators=(",", ":")) + "\n"
            f.write(s)
            sz += len(s)
            i += 1

    # small records: a few records per file (2-6KB), like individual API payloads
    for k in range(nsmall):
        nrec = random.randint(4, 10)
        with open("%s/feed_small/rec%05d.json" % (outdir, k), "w") as f:
            for j in range(nrec):
                f.write(json.dumps(rec(1000000 + k * 16 + j), separators=(",", ":")) + "\n")
    print("generated %s/feed.json (%.1f MB) and %d small records" % (outdir, big_mb, nsmall))

if __name__ == "__main__":
    main()
