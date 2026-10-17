#!/usr/bin/env python3
"""Summarize beat-zstd CSVs into RESULTS.md. Honest numbers, no spin."""
import csv, os, sys, glob

RES = sys.argv[1] if len(sys.argv) > 1 else "results/run"
out = []


def rows(name):
    p = os.path.join(RES, name)
    if not os.path.exists(p):
        return []
    with open(p) as f:
        return [r for r in csv.reader(f) if r]


def load_csv(path):
    with open(path) as f:
        return [r for r in csv.reader(f) if r]


def fmt_mb(b):
    return "%.2f" % (b / 1048576.0)


def main():
    cpu = open(os.path.join(RES, "cpu.txt")).read().strip() if os.path.exists(os.path.join(RES, "cpu.txt")) else "?"
    out.append("# beat-zstd results (%s)" % os.path.basename(RES))
    out.append("")
    out.append("CPU: %s" % cpu.replace("\n", " | "))
    out.append("zstd: v1.5.7 (pinned release, static)")
    out.append("")

    # ---------- router vs fixed levels ----------
    bench = rows("bench.csv")
    route = rows("route.csv")
    adapt = rows("adapt.csv")
    if bench:
        # mixed corpus = files big enough to be interesting; small corpora handled separately
        mixed_files = set()
        for r in bench:
            if r[2] == "nodict" and r[0] not in mixed_files:
                mixed_files.add(r[0])
        # split: small corpora have many files; treat corpora with >10 files as "small"
        from collections import Counter, defaultdict
        cnt = Counter(r[0] for r in bench)
        small = {f for f, c in cnt.items() if c > 0 and f.startswith(("rec", "chunk"))}
        mixed = {f for f in mixed_files if f not in small}

        by_level = defaultdict(lambda: [0, 0.0, 0.0, 0.0])  # level -> size, ctime_ms, dtime_ms
        for r in bench:
            if r[0] not in mixed or r[2] != "nodict":
                continue
            lvl = r[1]
            by_level[lvl][0] += int(r[3])
            by_level[lvl][1] += float(r[4])
            by_level[lvl][2] += float(r[6])
        # total input size from probe.csv
        probe = rows("probe.csv")
        pin = defaultdict(int)
        for p in probe:
            if p[0] in mixed:
                pin[p[0]] = int(p[1])
        # small corpora input sizes from bench dict rows aren't reliable; mixed only:
        tot_in = sum(pin.values())

        routetot = [0, 0.0, 0.0]
        routefiles = set()
        for r in route:
            if r[0] in mixed:
                routetot[0] += int(r[5])
                routetot[1] += float(r[6])
                routetot[2] += float(r[8])
                routefiles.add(r[0])

        out.append("## Experiment C: router vs fixed levels (mixed corpus, %d files, %s MB in)" %
                   (len(mixed), fmt_mb(tot_in)))
        out.append("")
        out.append("| approach | total out (MB) | ratio | total ctime (s) | total dtime (s) |")
        out.append("|---|---|---|---|---|")
        best_ratio = None
        pareto = []
        for lvl in sorted(by_level, key=int):
            s, ct, dt = by_level[lvl][0], by_level[lvl][1] / 1000.0, by_level[lvl][2] / 1000.0
            pareto.append(("-L%s" % lvl, s, ct, dt))
        if routetot[0]:
            pareto.append(("router", routetot[0], routetot[1] / 1000.0, routetot[2] / 1000.0))
        for name, s, ct, dt in pareto:
            out.append("| %s | %s | %.4f | %.1f | %.1f |" % (name, fmt_mb(s), s / tot_in if tot_in else 0, ct, dt))
        if adapt:
            asz = sum(int(r[2]) for r in adapt)
            at = sum(int(r[3]) for r in adapt) / 1000.0
            ain = sum(pin.get(os.path.basename(r[0]), 0) for r in adapt)
            out.append("| zstd --adapt (silesia only: %s in) | %s | %.4f | %.1f | n/a |" %
                       (fmt_mb(ain), fmt_mb(asz), asz / ain if ain else 0, at))
        out.append("")
        # verdict: is router pareto-dominated by any single level? does it beat L19 ratio?
        if routetot[0] and tot_in:
            better = [n for (n, s, ct, dt) in pareto if n.startswith("-L") and s <= routetot[0] and ct <= routetot[1] / 1000.0]
            l19 = by_level.get("19", [0, 0, 0, 0])
            out.append("Router total: %s MB. Fixed levels that are both smaller AND faster: %s." %
                       (fmt_mb(routetot[0]), ", ".join(better) if better else "none"))
            out.append("Router vs -L19: size %+.2f%%, time %+.1f%%." %
                       (100.0 * (routetot[0] - l19[0]) / l19[0] if l19[0] else 0,
                        100.0 * (routetot[1] / 1000.0 - l19[1] / 1000.0) / (l19[1] / 1000.0) if l19[1] else 0))
        out.append("")

        # ---------- dictionary track ----------
        out.append("## Experiment C: dictionary on small files")
        out.append("")
        for corp, key in (("feed_small (feed-like JSON records)", "rec"), ("text4k (4KB dickens chunks)", "chunk")):
            sfiles = [r for r in bench if r[0].startswith(key)]
            if not sfiles:
                continue
            lvls = sorted({r[1] for r in sfiles}, key=int)
            out.append("### %s" % corp)
            out.append("")
            out.append("| level | avg size nodict | avg size dict | dict gain | avg cspeed nodict | avg cspeed dict |")
            out.append("|---|---|---|---|---|---|")
            for lvl in lvls:
                nd = [int(r[3]) for r in sfiles if r[1] == lvl and r[2] == "nodict"]
                dd = [int(r[3]) for r in sfiles if r[1] == lvl and r[2] == "dict"]
                if not nd or not dd:
                    continue
                ndn = [float(r[7]) for r in sfiles if r[1] == lvl and r[2] == "nodict"]
                ddn = [float(r[7]) for r in sfiles if r[1] == lvl and r[2] == "dict"]
                an, ad = sum(nd) / len(nd), sum(dd) / len(dd)
                out.append("| %s | %.1f | %.1f | %.1f%% | %.0f | %.0f |" %
                           (lvl, an, ad, 100.0 * (an - ad) / an, sum(ndn) / len(ndn), sum(ddn) / len(ddn)))
            rr = [int(r[5]) for r in route if r[0].startswith(key)]
            if rr:
                out.append("| router(dict) | - | %.1f | - | - | - |" % (sum(rr) / len(rr)))
            out.append("")

    # ---------- tans ----------
    for name, title in (("tans.csv", "real file bytes (order-0)"), ("tans_lit.csv", "LZ literal streams"), ("tans_synth.csv", "synthetic distributions")):
        t = rows(name)
        if not t:
            continue
        data = t[1:] if t[0] and t[0][0] == "stream" else t
        # mean excess per (L, spread)
        agg = defaultdict(lambda: [0.0, 0])
        for r in data:
            if len(r) < 6:
                continue
            key = (r[1], r[2])
            agg[key][0] += float(r[5])
            agg[key][1] += 1
        if not agg:
            continue
        out.append("## Experiment A: tANS table spreads (%s)" % title)
        out.append("")
        out.append("| L | fse | edf | balanced | random | edf vs fse |")
        out.append("|---|---|---|---|---|---|")
        Ls = sorted({k[0] for k in agg}, key=int)
        spreads = ["fse", "edf", "balanced", "random"]
        for L in Ls:
            vals = []
            for sp in spreads:
                v = agg.get((L, sp))
                vals.append("%.6f" % (v[0] / v[1]) if v else "-")
            f = agg.get((L, "fse")); e = agg.get((L, "edf"))
            d = ("%.6f" % ((e[0] / e[1]) - (f[0] / f[1]))) if (f and e) else "-"
            out.append("| %s | %s | %s | %s | %s | %s |" % (L, vals[0], vals[1], vals[2], vals[3], d))
        out.append("")
        out.append("(mean excess bits/symbol over order-0 entropy; lower is better; all roundtrips verified)")
        out.append("")

    # ---------- lzmini ----------
    lz = rows("lzmini.csv")
    if lz:
        per = defaultdict(dict)
        size = {}
        for r in lz:
            if len(r) < 7:
                continue
            per[r[0]][r[1]] = float(r[4])
            size[r[0]] = int(r[2])
        out.append("## Experiment B: parsers (fixed LZ cost model, bits/byte)")
        out.append("")
        parsers = ["greedy", "lazy", "dp", "beam4", "beam8", "beam16"]
        out.append("| file | " + " | ".join(parsers) + " | beam16 vs lazy |")
        out.append("|---" * (len(parsers) + 3) + "|")
        for f in sorted(per):
            row = [("%.4f" % per[f][p]) if p in per[f] else "-" for p in parsers]
            if "beam16" in per[f] and "lazy" in per[f]:
                d = 100.0 * (per[f]["beam16"] - per[f]["lazy"]) / per[f]["lazy"]
                row.append("%+.3f%%" % d)
            else:
                row.append("-")
            out.append("| %s | %s | %s |" % (f, " | ".join(row[:-1]), row[-1]))
        # means
        mrow = []
        for p in parsers:
            vals = [per[f][p] for f in per if p in per[f]]
            mrow.append("%.4f" % (sum(vals) / len(vals)) if vals else "-")
        wsum = sum(size.values())
        dvals = [100.0 * (per[f]["beam16"] - per[f]["lazy"]) / per[f]["lazy"] for f in per if "beam16" in per[f] and "lazy" in per[f]]
        dstr = ("%+.3f%%" % (sum(dvals) / len(dvals))) if dvals else "-"
        out.append("| **mean** | %s | %s | %s |" % (" | ".join(mrow[:-1]), mrow[-1] if len(mrow) > 1 else "-", dstr))
        out.append("")
        out.append("(dp optimizes the offset-only model (no repcode pricing); greedy/lazy/beam include repcode pricing; beam is exact-DP-like only within its width)")
        out.append("")

    # ---------- filters ----------
    fl = rows("filter.csv")
    if fl:
        out.append("## Experiment C: pre-filters on 16/32-bit binary files")
        out.append("")
        out.append("| file | level | plain (MB) | delta | split2 | split4 | best | best vs plain |")
        out.append("|---|---|---|---|---|---|---|---|")
        agg = defaultdict(dict)
        for r in fl:
            if len(r) < 8:
                continue
            agg[(r[0], r[2])][r[1]] = int(r[3])
        for (f, lvl), d in sorted(agg.items()):
            plain = d.get("plain", 0)
            best_name, best_sz = "plain", plain
            for k in ("delta", "split2", "split4"):
                if k in d and d[k] < best_sz:
                    best_name, best_sz = k, d[k]
            out.append("| %s | %s | %s | %s | %s | %s | %s | %+.2f%% |" %
                       (f, lvl, fmt_mb(plain),
                        fmt_mb(d.get("delta", 0)), fmt_mb(d.get("split2", 0)), fmt_mb(d.get("split4", 0)),
                        best_name, 100.0 * (best_sz - plain) / plain if plain else 0))
        out.append("")

    out.append("## Notes")
    out.append("")
    out.append("- Every tANS and lzmini run verifies a full decode roundtrip before being reported.")
    out.append("- Router rules are hand-set (README); filter choice is empirical per file on a 128KB sample.")
    out.append("- See raw CSVs alongside this file for per-file detail.")
    open(os.path.join(RES, "RESULTS.md"), "w").write("\n".join(out) + "\n")
    print("\n".join(out))


if __name__ == "__main__":
    main()
