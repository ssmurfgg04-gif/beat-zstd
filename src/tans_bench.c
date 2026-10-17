/*
 * tans_bench - Experiment A of beat-zstd
 *
 * Question: does the *table spread* of a tabled ANS (tANS) coder measurably
 * change compressed size? zstd's FSE uses a heuristic step-walk spread.
 * arXiv:2504.18541 ("Optimal tables for asymmetric numeral systems",
 * Steiner, De Vita, Bezati, 2025) proves optimal spreads exist (min
 * discrepancy) and gives a deadline-scheduling construction.
 *
 * Method: one tANS codec, faithful to FSE's table construction
 * (decode newState = (j << k) - N, dyadic intervals), with the spread as
 * the only variable. All spreads share the SAME normalized counts, so any
 * size difference is purely the table layout.
 *
 * Spreads:
 *   fse      - clone of zstd's FSE_buildCTable spread walk
 *   edf      - arXiv:2504.18541 deadline-scheduling (earliest deadline first)
 *   balanced - per-symbol even spacing (van der Corput style) + probing
 *   random   - control (Fisher-Yates shuffle, fixed seed)
 *
 * Streams: raw file bytes (order-0 model), plus synthetic distributions.
 * Roundtrip is verified for every single run; a failed roundtrip aborts.
 *
 * Build: cc -O2 -o tans_bench tans_bench.c
 * Usage: tans_bench <file>...          (byte streams, L in {8,10,12})
 *        tans_bench --synth            (synthetic distributions)
 *        tans_bench --csv              (CSV output)
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <math.h>
#include <time.h>

#define MAXSYM 256
#define MAXLOG 12
#define MAXN (1u << MAXLOG)

typedef struct { uint16_t u; uint16_t base; uint8_t k; } EncEntry; static const char *curSpreadName = "?";

static uint8_t spread_tab[MAXN];
static uint8_t dec_sym[MAXN];
static uint8_t dec_nb[MAXN];
static uint16_t dec_base[MAXN];
static uint32_t norm[MAXSYM];     /* normalized counts, lowprob => count 1 + flag */
static uint8_t lowprob[MAXSYM];
static uint32_t hist[MAXSYM];
static uint32_t nSym, tableLog, tableSize;
static uint32_t posStart[MAXSYM]; /* start index in flat enc arena */
static EncEntry enc[MAXN];

/* ---------- spreads ---------- */

/* zstd FSE_buildCTable spread clone: step walk with low-prob tail */
static void spread_fse(void) {
    static uint8_t used[MAXN];
    uint32_t step = (tableSize >> 1) + (tableSize >> 3) + 3;
    uint32_t mask = tableSize - 1;
    uint32_t high = tableSize - 1;
    memset(used, 0, tableSize);
    memset(spread_tab, 0, tableSize);
    for (uint32_t s = 0; s < MAXSYM; s++) {
        if (norm[s] == 0) continue;
        if (lowprob[s]) { spread_tab[high] = (uint8_t)s; used[high] = 1; high--; }
    }
    uint32_t position = 0;
    for (uint32_t s = 0; s < MAXSYM; s++) {
        if (norm[s] == 0 || lowprob[s]) continue;
        for (uint32_t i = 0; i < norm[s]; i++) {
            if (used[position]) { fprintf(stderr, "fse spread collision\n"); exit(1); }
            spread_tab[position] = (uint8_t)s;
            used[position] = 1;
            do { position = (position + step) & mask; } while (position > high);
        }
    }
    for (uint32_t i = 0; i < tableSize; i++)
        if (!used[i]) { fprintf(stderr, "fse hole\n"); exit(1); }
}

/* arXiv:2504.18541: occurrence l of symbol s is a job released at
 * D(s,l) = min{m : (n_s/N)(m+1) >= l} with deadline D(s,l+1).
 * Schedule positions 0..N-1 by earliest-deadline-first. */
static void spread_edf(void) {
    /* next occurrence index per symbol */
    static uint32_t occ[MAXSYM];
    memset(occ, 0, sizeof(occ));
    memset(spread_tab, 0xFF, tableSize);
    uint32_t placed = 0;
    for (uint32_t m = 0; m < tableSize; m++) {
        int best = -1;
        uint32_t bestD = 0xFFFFFFFF;
        for (uint32_t s = 0; s < MAXSYM; s++) {
            if (norm[s] == 0 || occ[s] >= norm[s]) continue;
            uint32_t l = occ[s];
            /* release D(s,l) = ceil(l*N/n) - 1, clamped at 0 */
            uint32_t rel = (l == 0) ? 0 : ((uint32_t)((uint64_t)l * tableSize + norm[s] - 1) / norm[s]) - 1;
            if ((int64_t)m < (int64_t)rel) continue;
            /* deadline = release of occurrence l+1 */
            uint32_t l2 = l + 1;
            uint32_t dl = ((uint64_t)l2 * tableSize + norm[s] - 1) / norm[s];
            if (dl < bestD || (dl == bestD && (best < 0 || s < (uint32_t)best))) {
                bestD = dl; best = (int)s;
            }
        }
        if (best < 0) {
            /* no released job: pick any symbol with remaining occurrences */
            for (uint32_t s = 0; s < MAXSYM; s++)
                if (norm[s] && occ[s] < norm[s]) { best = (int)s; break; }
            if (best < 0) { fprintf(stderr, "edf stuck\n"); exit(1); }
        }
        spread_tab[m] = (uint8_t)best;
        occ[best]++;
        placed++;
    }
    if (placed != tableSize) { fprintf(stderr, "edf size\n"); exit(1); }
}

/* balanced: each symbol's slots at ideal positions (2i+1)*N/(2n), collisions
 * resolved by nearest free slot. Symbols processed by descending count. */
static void spread_balanced(void) {
    static uint8_t used[MAXN];
    memset(used, 0, tableSize);
    memset(spread_tab, 0xFF, tableSize);
    uint32_t order[MAXSYM]; uint32_t nOrd = 0;
    for (uint32_t s = 0; s < MAXSYM; s++) if (norm[s]) order[nOrd++] = s;
    /* descending count, tie by symbol id */
    for (uint32_t i = 0; i < nOrd; i++)
        for (uint32_t j = i + 1; j < nOrd; j++) {
            uint32_t a = order[i], b = order[j];
            if (norm[b] > norm[a] || (norm[b] == norm[a] && b < a)) { order[i] = b; order[j] = a; }
        }
    for (uint32_t oi = 0; oi < nOrd; oi++) {
        uint32_t s = order[oi], n = norm[s];
        for (uint32_t i = 0; i < n; i++) {
            double ideal = ((double)(2 * i + 1)) * tableSize / (2.0 * n);
            int32_t p = (int32_t)ideal;
            if (p >= (int32_t)tableSize) p = tableSize - 1;
            /* probe outward for a free slot */
            for (int32_t d = 0; d < (int32_t)tableSize; d++) {
                int32_t q = p + d;
                if (q >= 0 && q < (int32_t)tableSize && !used[q]) { p = q; goto placed; }
                q = p - d;
                if (q >= 0 && q < (int32_t)tableSize && !used[q]) { p = q; goto placed; }
            }
            fprintf(stderr, "balanced stuck\n"); exit(1);
        placed:
            used[p] = 1; spread_tab[p] = (uint8_t)s;
        }
    }
}

static uint64_t rngState = 0x9E3779B97F4A7C15ull;
static uint64_t rng(void) { rngState ^= rngState << 13; rngState ^= rngState >> 7; rngState ^= rngState << 17; return rngState; }

static void spread_random(void) {
    uint32_t idx = 0;
    for (uint32_t s = 0; s < MAXSYM; s++)
        for (uint32_t i = 0; i < norm[s]; i++) spread_tab[idx++] = (uint8_t)s;
    if (idx != tableSize) { fprintf(stderr, "rand size\n"); exit(1); }
    for (uint32_t i = tableSize - 1; i > 0; i--) {
        uint32_t j = (uint32_t)(rng() % (i + 1));
        uint8_t t = spread_tab[i]; spread_tab[i] = spread_tab[j]; spread_tab[j] = t;
    }
}

/* ---------- table build (identical for all spreads; FSE-faithful) ----------
 * decode: for table position u holding symbol s (occurrence index i in
 * ascending-u order), j = n_s + i, k = L - floor(log2 j),
 * base = (j << k) - N; decoding reads k bits b and sets x' = base + b.
 * Those intervals tile [0, N). Encoding inverts: given x', find j, emit
 * b = x' - base in k bits, move to position u = posList[s][j - n_s]. */
static void build_tables(void) {
    static uint32_t occ[MAXSYM];
    memset(occ, 0, sizeof(occ));
    /* encoder arena: contiguous per symbol, prefix-summed starts */
    uint32_t encIdx = 0;
    for (uint32_t s = 0; s < MAXSYM; s++) { posStart[s] = encIdx; encIdx += norm[s]; }
    for (uint32_t u = 0; u < tableSize; u++) {
        uint8_t s = spread_tab[u];
        uint32_t i = occ[s]++;
        uint32_t j = norm[s] + i;
        uint32_t k = tableLog - (31 - __builtin_clz(j));
        uint16_t base = (uint16_t)((j << k) - tableSize);
        EncEntry *e = &enc[posStart[s] + i];
        e->u = (uint16_t)u;
        e->k = (uint8_t)k;
        e->base = base;
        dec_sym[u] = s;
        dec_nb[u] = (uint8_t)k;
        dec_base[u] = base;
    }

    /* DEBUG: full interval-tiling self check for every symbol */
    for (uint32_t s = 0; s < MAXSYM; s++) {
        if (!norm[s]) continue;
        uint32_t n = norm[s], K = 31 - __builtin_clz(n);
        uint32_t split = n * (1u << (tableLog - K)) - tableSize;
        uint32_t kA_ = tableLog - K - 1, kB_ = tableLog - K;
        for (uint32_t x = 0; x < tableSize; x++) {
            uint32_t j, k;
            if (x < split) { j = (1u << (K + 1)) + (x >> kA_); k = kA_; }
            else { j = n + ((x - split) >> kB_); k = kB_; }
            EncEntry *e = &enc[posStart[s] + (j - n)];
            if ((int32_t)(x - e->base) < 0 || (uint32_t)(x - e->base) >= (1u << e->k)) {
                fprintf(stderr, "SELFCHECK fail s=%u x=%u L=%u n=%u j=%u k=%u e.k=%u e.base=%u e.u=%u\n",
                        s, x, tableLog, n, j, k, e->k, e->base, e->u);
                exit(1);
            }
            if (e->k != k) {
                fprintf(stderr, "K MISMATCH s=%u x=%u L=%u j=%u k=%u e.k=%u\n", s, x, tableLog, j, k, e->k);
                exit(1);
            }
        }
    }
    for (uint32_t s = 0; s < MAXSYM; s++)
        if (occ[s] != norm[s]) {
            fprintf(stderr, "build mismatch s=%u occ=%u norm=%u L=%u spread=%s\n", s, occ[s], norm[s], tableLog, curSpreadName);
            exit(1);
        }
}

/* ---------- bit I/O (LSB-first forward writer, backward reader) ---------- */
static uint8_t *bitBuf; static size_t bitPos; static uint64_t bitAcc; static uint32_t bitN;
static uint64_t totalBits;

static void bw_init(uint8_t *buf) { bitBuf = buf; bitPos = 0; bitAcc = 0; bitN = 0; totalBits = 0; }
static void bw_push(uint32_t v, uint32_t k) {
    bitAcc |= ((uint64_t)(v & ((k < 64) ? ((1u << k) - 1) : ~0u))) << bitN;
    bitN += k; totalBits += k;
    while (bitN >= 8) { bitBuf[bitPos++] = (uint8_t)(bitAcc & 0xFF); bitAcc >>= 8; bitN -= 8; }
}
static size_t bw_finish(void) {
    if (bitN) { bitBuf[bitPos++] = (uint8_t)(bitAcc & ((1u << bitN) - 1)); bitAcc = 0; bitN = 0; }
    return bitPos;
}

typedef struct { const uint8_t *buf; size_t len; size_t byteIdx; uint64_t acc; uint32_t avail; } BitR;
static void br_init(BitR *r, const uint8_t *buf, size_t len, uint64_t total) {
    r->buf = buf; r->len = len; r->byteIdx = len; r->acc = 0; r->avail = 0;
    uint32_t pad = (uint32_t)(len * 8 - total);
    if (len == 0) return;
    r->byteIdx--;
    r->acc = r->buf[r->byteIdx];
    r->avail = 8 - (uint32_t)pad; /* valid bits are the low bitN bits */
    if (pad >= 8) { fprintf(stderr, "pad overflow\n"); exit(1); }
}
static uint32_t br_read(BitR *r, uint32_t k) {
    while (r->avail < k + 8 && r->byteIdx > 0) {
        r->byteIdx--;
        r->acc = (r->acc << 8) | r->buf[r->byteIdx];
        r->avail += 8;
    }
    if (r->avail < k) { fprintf(stderr, "bit underrun (L=%u spread=%s)\n", tableLog, curSpreadName); exit(1); }
    r->avail -= k;
    return (uint32_t)((r->acc >> r->avail) & ((1u << k) - 1));
}

/* ---------- codec ---------- */
static uint8_t *outBytes; static size_t outCap;
static uint32_t encFinalX;

/* encode stream (len symbols) reversed; returns payload bits */
static uint64_t encode(const uint8_t *stream, size_t len) {
    /* per-symbol split params for interval lookup */
    /* encoder state x' -> (j, k, b) via two-region split */
    static uint32_t split[MAXSYM], K_[MAXSYM], n_[MAXSYM], kA[MAXSYM], kB[MAXSYM];
    for (uint32_t s = 0; s < MAXSYM; s++) {
        if (!norm[s]) continue;
        uint32_t n = norm[s], K = 31 - __builtin_clz(n);
        n_[s] = n; K_[s] = K;
        split[s] = n * (1u << (tableLog - K)) - tableSize;
        kA[s] = tableLog - K - 1; /* small intervals (x' < split) */
        kB[s] = tableLog - K;     /* big intervals */
    }
    bw_init(outBytes);
    uint32_t x = tableSize - 1;
    for (size_t ti = len; ti-- > 0; ) {
        uint8_t s = stream[ti];
        uint32_t j, k, base;
        if (x < split[s]) {
            j = (1u << (K_[s] + 1)) + (x >> kA[s]);
            k = kA[s];
        } else {
            j = n_[s] + ((x - split[s]) >> kB[s]);
            k = kB[s];
        }
        EncEntry *e = &enc[posStart[s] + (j - n_[s])];
        base = e->base;
        if ((int32_t)(x - base) < 0 || (uint32_t)(x - base) >= (1u << e->k)) {
            fprintf(stderr, "enc interval bug s=%u x=%u L=%u spread=%s n=%u K=%u split=%u j=%u k=%u e.k=%u e.base=%u e.u=%u\n",
                    s, x, tableLog, curSpreadName, n_[s], K_[s], split[s], j, k, e->k, e->base, e->u); exit(1);
        }
        bw_push(x - base, e->k);
        x = e->u;
    }
    encFinalX = x;
    bw_finish();
    /* header: nsyms(8) x0(4) payloadbits(8) payloadbytes(4) */
    return totalBits;
}

static void decode(uint8_t *dst, size_t len, uint32_t x0, uint64_t payloadBits, size_t payloadBytes) {
    BitR r; br_init(&r, outBytes, payloadBytes, payloadBits);
    uint32_t x = x0;
    for (size_t i = 0; i < len; i++) {
        uint8_t s = dec_sym[x];
        dst[i] = s;
        uint32_t k = dec_nb[x];
        uint32_t b = k ? br_read(&r, k) : 0;
        x = dec_base[x] + b;
        if (x >= tableSize) { fprintf(stderr, "dec state oob\n"); exit(1); }
    }
}

/* ---------- normalization (shared across spreads) ---------- */
static int normalize_from_hist(uint32_t total) {
    /* largest-remainder rounding of p_s*N; seen symbols forced >= 1 */
    memset(lowprob, 0, sizeof(lowprob));
    uint32_t tgt = tableSize;
    static double frac[MAXSYM];
    uint32_t assigned = 0;
    for (uint32_t s = 0; s < MAXSYM; s++) {
        if (!hist[s]) { norm[s] = 0; continue; }
        double exact = (double)hist[s] * tgt / total;
        norm[s] = (uint32_t)exact;
        if (norm[s] == 0) norm[s] = 1; /* provisional: force 1 */
        frac[s] = exact - norm[s];
        assigned += norm[s];
    }
    /* correct sum: remove from symbols with largest positive frac deficit,
     * add to those with largest frac surplus */
    while (assigned > tgt) {
        int best = -1; double bv = -1e18;
        for (uint32_t s = 0; s < MAXSYM; s++)
            if (norm[s] > 1 && frac[s] > bv) { bv = frac[s]; best = (int)s; }
        if (best < 0) { fprintf(stderr, "norm remove stuck\n"); exit(1); }
        norm[best]--; frac[best] -= 1.0; assigned--;
    }
    while (assigned < tgt) {
        int best = -1; double bv = -1e18;
        for (uint32_t s = 0; s < MAXSYM; s++)
            if (hist[s] && frac[s] > bv) { bv = frac[s]; best = (int)s; }
        if (best < 0) { for (uint32_t s = 0; s < MAXSYM; s++) if (hist[s]) { best = (int)s; break; } }
        norm[best]++; frac[best] += 1.0; assigned++;
    }
    /* mark symbols whose true share is under 1 slot as lowprob (FSE-ish) */
    for (uint32_t s = 0; s < MAXSYM; s++)
        if (hist[s] && (double)hist[s] * tgt / total < 1.0) lowprob[s] = 1;
    return 0;
}

static double entropy0(void) {
    double total = 0; for (uint32_t s = 0; s < MAXSYM; s++) total += hist[s];
    double H = 0;
    for (uint32_t s = 0; s < MAXSYM; s++)
        if (hist[s]) { double p = hist[s] / total; H -= p * log2(p); }
    return H;
}

/* ---------- one run ---------- */
typedef void (*SpreadFn)(void);
static const char *spreadNames[] = { "fse", "edf", "balanced", "random" };
static SpreadFn spreadFns[] = { spread_fse, spread_edf, spread_balanced, spread_random };
#define NSPREADS 4

static uint8_t *verifyBuf;

static void run_stream(const char *name, const uint8_t *data, size_t len, int csv) {
    /* histogram */
    memset(hist, 0, sizeof(hist));
    for (size_t i = 0; i < len; i++) hist[data[i]]++;
    uint32_t seen = 0; for (uint32_t s = 0; s < MAXSYM; s++) if (hist[s]) seen++;
    double H = entropy0();
    for (int L = 8; L <= 12; L += 2) {
        if ((1u << L) < seen) continue; /* table must fit alphabet */
        tableLog = (uint32_t)L; tableSize = 1u << L;
        normalize_from_hist((uint32_t)(len > 0xFFFFFFFFu ? 0xFFFFFFFFu : len));
        for (int sp = 0; sp < NSPREADS; sp++) {
            curSpreadName = spreadNames[sp];
            spreadFns[sp]();
            build_tables();
            uint64_t bits = encode(data, len);
            memset(verifyBuf, 0, len);
            decode(verifyBuf, len, encFinalX, bits, bitPos);
            if (memcmp(verifyBuf, data, len) != 0) {
                fprintf(stderr, "ROUNDTRIP FAIL %s L=%d spread=%s\n", name, L, spreadNames[sp]);
                exit(1);
            }
            double bps = (double)bits / len;
            double excess = bps - H;
            if (csv) {
                printf("%s,%d,%s,%.6f,%.6f,%.6f\n", name, L, spreadNames[sp], bps, H, excess);
            } else {
                printf("%-28s L=%-2d %-8s bits/sym=%9.6f H=%9.6f excess=%+9.6f (%+7.1f ppm)\n",
                       name, L, spreadNames[sp], bps, H, excess, excess / (H + 1e-12) * 1e6);
            }
        }
    }
}

/* ---------- synthetic streams ---------- */
static uint8_t *synBuf;
static void synth_zipf(size_t len, double a) { /* over 64 symbols */
    for (size_t i = 0; i < len; i++) {
        double u = (double)(rng() % 1000000) / 1000000.0;
        double cum = 0; uint32_t s = 63;
        for (uint32_t k = 1; k <= 64; k++) { cum += 1.0 / pow((double)k, a); if (u < cum / 6.39) { s = k - 1; break; } }
        synBuf[i] = (uint8_t)s;
    }
}
static void synth_uniform(size_t len) { for (size_t i = 0; i < len; i++) synBuf[i] = (uint8_t)(rng() & 0xFF); }
static void synth_geometric(size_t len, double p) {
    for (size_t i = 0; i < len; i++) {
        double u = (double)(rng() % 1000000) / 1000000.0;
        uint32_t k = (uint32_t)(log(1.0 - u) / log(1.0 - p));
        synBuf[i] = (uint8_t)(k & 0xFF);
    }
}
static void synth_golden(size_t len) { /* dominant symbol at 0.61803 */
    for (size_t i = 0; i < len; i++) {
        double u = (double)(rng() % 1000000) / 1000000.0;
        if (u < 0.61803) synBuf[i] = 0;
        else synBuf[i] = (uint8_t)(1 + (rng() % 255));
    }
}

/* ---------- main ---------- */
int main(int argc, char **argv) {
    int csv = 0, synth = 0;
    const char *files[64]; int nfiles = 0;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--csv")) csv = 1;
        else if (!strcmp(argv[i], "--synth")) synth = 1;
        else files[nfiles++] = argv[i];
    }
    if (!csv && !synth)
        printf("%-28s %-4s %-8s %9s %9s %9s\n", "stream", "L", "spread", "bits/sym", "entropy", "excess");
    if (csv) printf("stream,L,spread,bits_per_sym,entropy,excess\n");

    outCap = 512u << 20; /* generous */
    outBytes = malloc(outCap);
    verifyBuf = malloc(512u << 20);
    synBuf = malloc(32u << 20);
    if (!outBytes || !verifyBuf || !synBuf) { fprintf(stderr, "oom\n"); return 1; }

    if (synth) {
        size_t len = 16u << 20;
        synth_zipf(len, 1.2);   run_stream("synth_zipf_1.2", synBuf, len, csv);
        synth_zipf(len, 0.8);   run_stream("synth_zipf_0.8", synBuf, len, csv);
        synth_uniform(len);     run_stream("synth_uniform", synBuf, len, csv);
        synth_geometric(len, 0.25); run_stream("synth_geometric", synBuf, len, csv);
        synth_golden(len);      run_stream("synth_golden618", synBuf, len, csv);
        return 0;
    }
    if (nfiles == 0) { fprintf(stderr, "usage: tans_bench [--csv] <file>... | --synth\n"); return 1; }
    for (int i = 0; i < nfiles; i++) {
        FILE *f = fopen(files[i], "rb");
        if (!f) { fprintf(stderr, "open %s\n", files[i]); return 1; }
        fseek(f, 0, SEEK_END); long sz = ftell(f); fseek(f, 0, SEEK_SET);
        if (sz < 4096) { /* tiny: skip with note */
            fprintf(stderr, "skip tiny %s (%ld B)\n", files[i], sz); fclose(f); continue;
        }
        size_t cap = 128u << 20; size_t len = (size_t)sz < cap ? (size_t)sz : cap;
        uint8_t *data = malloc(len);
        if (fread(data, 1, len, f) != len) { fprintf(stderr, "read %s\n", files[i]); return 1; }
        fclose(f);
        const char *base = strrchr(files[i], '/'); base = base ? base + 1 : files[i];
        run_stream(base, data, len, csv);
        free(data);
    }
    return 0;
}
