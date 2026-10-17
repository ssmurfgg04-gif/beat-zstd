/*
 * lzmini - Experiment B of beat-zstd
 *
 * Question: does beam-search parsing beat greedy/lazy/optimal parsing, and
 * by how much, under a fixed LZ cost model with repcodes?
 * (GPX reports beam parsing alone saved +0.084% on its test set.)
 *
 * Method: one small LZ77 engine (hash-chain matches, window 64KB within
 * 256KB blocks, min match 4, max 512) and one FIXED cost model, with the
 * parser as the only variable:
 *
 *   literal:               9 bits
 *   match(off, len):       1 + 4 + ceil(log2(off+1)) + 3 + ceil(log2(len-3))
 *   rep-match(len):        1 + 3 + 3 + ceil(log2(len-3))   (offset == last)
 *
 * Parsers: greedy, lazy, dp (backward DP, exact optimum of the offset-only
 * model), beam{4,8,16} (per-position frontier, dedup by rep offset).
 * Match candidates are precomputed once per position and shared by all
 * parsers, so runtime differences are pure parser work.
 *
 * The absolute ratio is NOT zstd's; the PARSER DELTA is the measurement.
 * Every run decodes and verifies the token stream against the input.
 *
 * Build: cc -O2 -o lzmini lzmini.c
 * Usage: lzmini <file> [--parser P] [--cap MB] [--dump-literals FILE]
 *        P in {greedy,lazy,dp,beam4,beam8,beam16}
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <time.h>

#define WIN     65536
#define BLOCK   (256 * 1024)
#define MINM    4
#define MAXM    512
#define HASHLOG 15
#define HASHSZ  (1u << HASHLOG)
#define CHDEPTH 128
#define BNMAX   9000000

static uint32_t hashTab[HASHSZ];
static uint32_t *chain;
static uint32_t ceilld[1024];

static uint32_t h4(const uint8_t *p) {
    uint32_t v; memcpy(&v, p, 4);
    return (v * 2654435761u) >> (32 - HASHLOG);
}

typedef struct { uint32_t off, len; } Match;

/* chain walk, no rep logic (rep is handled by callers) */
static Match find_match(const uint8_t *d, uint32_t n, uint32_t i) {
    Match m = {0, 0};
    if (i + MINM > n) return m;
    uint32_t h = h4(d + i);
    uint32_t cand = hashTab[h];
    uint32_t depth = CHDEPTH;
    while (cand != 0xFFFFFFFFu && depth--) {
        uint32_t off = i - cand;
        if (off == 0) { cand = chain[cand]; continue; } /* skip self (pre-inserted) */
        if (off >= WIN) break; /* off must fit u16 */
        if (memcmp(d + cand, d + i, MINM) == 0) {
            uint32_t l = MINM;
            while (l < MAXM && i + l < n && d[cand + l] == d[i + l]) l++;
            if (l > m.len) { m.len = l; m.off = off; }
            if (l >= MAXM) break;
        }
        cand = chain[cand];
    }
    return m;
}

/* extend a rep match at i with offset rep */
static uint32_t rep_len(const uint8_t *d, uint32_t n, uint32_t i, uint32_t rep) {
    if (!rep || rep > i || i + MINM > n) return 0;
    if (memcmp(d + i - rep, d + i, MINM) != 0) return 0;
    uint32_t l = MINM;
    while (l < MAXM && i + l < n && d[i - rep + l] == d[i + l]) l++;
    return l;
}

static uint32_t mbits_(uint32_t off, uint32_t len) {
    uint32_t ob = ceilld[off + 1 < 1024 ? off + 1 : 1023];
    uint32_t lb = ceilld[len - 3 < 1024 ? len - 3 : 1023];
    return 8 + ob + lb; /* 1 flag + 4 + ceil(log2 off+1) + 3 + ceil(log2 len-3) */
}
static uint32_t repbits_(uint32_t len) {
    return 1 + 3 + 3 + ceilld[len - 3 < 1024 ? len - 3 : 1023];
}

/* ---- tokens ---- */
typedef struct { uint8_t isMatch; uint32_t off, len; } Tok; /* literal byte in off */
static Tok *toks; static size_t ntok, toksCap;
static void emit_lit(uint8_t b) { if (ntok < toksCap) { toks[ntok].isMatch = 0; toks[ntok].off = b; toks[ntok].len = 1; ntok++; } }
static void emit_match(uint32_t off, uint32_t len) { if (ntok < toksCap) { toks[ntok].isMatch = 1; toks[ntok].off = off; toks[ntok].len = len; ntok++; } }

static Match *mc; /* per-position cached best match */

static void parse_greedy(const uint8_t *d, uint32_t n, int lazy, int userep) {
    uint32_t i = 0, rep = 0;
    while (i < n) {
        Match m = mc[i];
        uint32_t rl = userep ? rep_len(d, n, i, rep) : 0;
        if (rl >= MINM && rl + 2 >= m.len) { m.len = rl; m.off = rep; }
        if (m.len >= MINM && mbits_(m.off, m.len) < 9.0 * m.len) {
            if (lazy && i + 1 < n) {
                Match m2 = mc[i + 1];
                uint32_t rl2 = rep_len(d, n, i + 1, m.off);
                if (rl2 >= MINM && rl2 + 2 >= m2.len) { m2.len = rl2; m2.off = m.off; }
                if (m2.len >= MINM && mbits_(m2.off, m2.len) + 9 < mbits_(m.off, m.len)) {
                    emit_lit(d[i]); i++; continue;
                }
            }
            emit_match(m.off, m.len);
            rep = m.off; i += m.len;
        } else { emit_lit(d[i]); i++; }
    }
}

static Tok *dpToks; static double *best;
static void parse_dp(const uint8_t *d, uint32_t n) {
    (void)d;
    best[n] = 0;
    for (uint32_t i = n; i-- > 0; ) {
        double c = best[i + 1] + 9.0;
        dpToks[i].isMatch = 0; dpToks[i].len = 1; dpToks[i].off = 0;
        Match m = mc[i];
        if (m.len >= MINM) {
            for (uint32_t l = MINM; l <= m.len; l++) {
                if (i + l > n) break;
                double c2 = mbits_(m.off, l) + best[i + l];
                if (c2 < c) { c = c2; dpToks[i].isMatch = 1; dpToks[i].len = l; dpToks[i].off = m.off; }
            }
        }
        best[i] = c;
    }
    uint32_t i = 0;
    while (i < n) {
        if (dpToks[i].isMatch) { emit_match(dpToks[i].off, dpToks[i].len); i += dpToks[i].len; }
        else { emit_lit(d[i]); i++; }
    }
}

/* ---- beam: per-position append-only frontier ----
 * Nodes are immutable once inserted (soundness: a node's cost and its
 * parent path always match). Selection of which B nodes to expand is
 * done read-only at expansion time: cheapest B nodes with distinct rep
 * offsets. Same-rep dominance filter at insert time drops nodes that
 * cannot win. */
typedef struct { float cost; uint16_t rep; uint32_t parentIdx; uint8_t isMatch; uint16_t len, off; uint32_t next; } BNode;
static BNode *bnodes; static uint32_t bcount;
static uint32_t *bhead; static uint8_t *bcnt;

static void beam_ins(uint32_t pos, float cost, uint16_t rep, uint32_t parentIdx, uint8_t isMatch, uint16_t off, uint16_t len) {
    if (pos > BLOCK) return;
    /* same-rep dominance: an existing same-rep node at least as cheap wins */
    for (uint32_t ni = bhead[pos]; ni != 0xFFFFFFFFu; ni = bnodes[ni].next)
        if (bnodes[ni].rep == rep && bnodes[ni].cost <= cost) return;
    if (bcount >= BNMAX) return; /* overflow guard: drop (beam degrades, stays sound) */
    BNode *nd = &bnodes[bcount];
    nd->cost = cost; nd->rep = rep; nd->parentIdx = parentIdx; nd->isMatch = isMatch; nd->off = off; nd->len = len;
    nd->next = bhead[pos]; bhead[pos] = bcount; bcnt[pos]++;
    bcount++;
}

static void parse_beam(const uint8_t *d, uint32_t n, int B, int userep) {
    memset(bhead, 0xFF, (n + 1) * sizeof(uint32_t));
    memset(bcnt, 0, n + 1);
    bcount = 0;
    bnodes[0].cost = 0; bnodes[0].rep = 0; bnodes[0].parentIdx = 0xFFFFFFFFu;
    bnodes[0].isMatch = 0; bnodes[0].len = 0; bnodes[0].off = 0; bnodes[0].next = 0xFFFFFFFFu;
    bhead[0] = 0; bcnt[0] = 1; bcount = 1;
    uint32_t sel[64]; /* selected node indices to expand */
    for (uint32_t p = 0; p < n; p++) {
        if (bhead[p] == 0xFFFFFFFFu) continue;
        /* read-only selection: cheapest B distinct-rep nodes (insertion sort) */
        int ns = 0;
        for (uint32_t ni = bhead[p]; ni != 0xFFFFFFFFu && ns < 64; ni = bnodes[ni].next)
            sel[ns++] = ni;
        for (int i = 1; i < ns; i++) { /* sort by cost */
            uint32_t v = sel[i]; int j = i - 1;
            while (j >= 0 && bnodes[sel[j]].cost > bnodes[v].cost) { sel[j+1] = sel[j]; j--; }
            sel[j+1] = v;
        }
        int nexp = 0; uint16_t reps[64];
        for (int i = 0; i < ns && nexp < B; i++) {
            int dup = 0;
            for (int j = 0; j < nexp; j++) if (reps[j] == bnodes[sel[i]].rep) { dup = 1; break; }
            if (!dup) { reps[nexp] = bnodes[sel[i]].rep; sel[nexp] = sel[i]; nexp++; }
        }
        /* sel[0..nexp-1] are the nodes to expand (indices) */
        for (int i = 0; i < nexp; i++) {
            uint32_t ni = sel[i];
            (void)reps[i];
            BNode *nd = &bnodes[ni];
            if (p + 1 <= n) beam_ins(p + 1, nd->cost + 9.0f, nd->rep, ni, 0, 0, 0);
            Match m = mc[p];
            uint32_t rl = userep ? rep_len(d, n, p, nd->rep) : 0;
            if (rl >= MINM && rl + 2 >= m.len) { m.len = rl; m.off = nd->rep; }
            if (m.len >= MINM && p + m.len <= n) {
                uint32_t bits = (m.off == nd->rep && userep) ? repbits_(m.len) : mbits_(m.off, m.len);
                beam_ins(p + m.len, nd->cost + (float)bits, (uint16_t)m.off, ni, 1, (uint16_t)m.off, (uint16_t)m.len);
            }
        }
    }
    uint32_t win = bhead[n];
    if (win == 0xFFFFFFFFu) { fprintf(stderr, "beam stuck\n"); exit(1); }
    for (uint32_t ni = bhead[n]; ni != 0xFFFFFFFFu; ni = bnodes[ni].next)
        if (bnodes[ni].cost < bnodes[win].cost) win = ni;
    /* reconstruct backwards, replay forwards */
    typedef struct { uint32_t node, endpos; } Frame;
    Frame *st = malloc((n + 2) * sizeof(Frame));
    uint32_t sp = 0, cur = win, curpos = n;
    while (cur != 0) {
        st[sp].node = cur; st[sp].endpos = curpos; sp++;
        uint32_t par = bnodes[cur].parentIdx;
        if (par == 0xFFFFFFFFu) break;
        curpos = (bnodes[cur].isMatch) ? curpos - bnodes[cur].len : curpos - 1;
        cur = par;
        if (sp > n + 1) { fprintf(stderr, "beam trace overflow\n"); exit(1); }
    }
    for (uint32_t k = sp; k-- > 0; ) {
        BNode *nd = &bnodes[st[k].node];
        if (nd->isMatch) emit_match(nd->off, nd->len);
        else emit_lit(d[st[k].endpos - 1]);
    }
    free(st);
}

/* ---- decode + verify ---- */
static uint8_t *decBuf;
static int decode_verify(const uint8_t *d, uint32_t n) {
    uint32_t i = 0;
    for (size_t t = 0; t < ntok; t++) {
        if (toks[t].isMatch) {
            uint32_t off = toks[t].off, len = toks[t].len;
            if (off == 0 || off > i || len > n - i) return 0;
            for (uint32_t k = 0; k < len; k++) decBuf[i + k] = decBuf[i - off + k];
            i += len;
        } else {
            if (i >= n) return 0;
            decBuf[i++] = (uint8_t)toks[t].off;
        }
    }
    return i == n && memcmp(decBuf, d, n) == 0;
}

int main(int argc, char **argv) {
    const char *file = NULL, *parser = "lazy", *dumpLit = NULL;
    uint32_t capMB = 16;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--parser") && i + 1 < argc) parser = argv[++i];
        else if (!strcmp(argv[i], "--cap") && i + 1 < argc) capMB = (uint32_t)atoi(argv[++i]);
        else if (!strcmp(argv[i], "--dump-literals") && i + 1 < argc) dumpLit = argv[++i];
        else if (argv[i][0] != '-') file = argv[i];
    }
    if (!file) { fprintf(stderr, "usage: lzmini <file> [--parser P] [--cap MB] [--dump-literals F]\n"); return 1; }

    for (uint32_t x = 1; x < 1024; x++) {
        uint32_t v = x, l = 0; while ((1u << l) < v) l++; ceilld[x] = l;
    }
    ceilld[0] = 0;

    FILE *f = fopen(file, "rb");
    if (!f) { fprintf(stderr, "open %s\n", file); return 1; }
    fseek(f, 0, SEEK_END); long sz = ftell(f); fseek(f, 0, SEEK_SET);
    size_t len = (size_t)sz < (size_t)capMB * 1048576 ? (size_t)sz : (size_t)capMB * 1048576;
    uint8_t *data = malloc(len);
    if (fread(data, 1, len, f) != len) { fprintf(stderr, "read\n"); return 1; }
    fclose(f);

    int userep = strcmp(parser, "dp") != 0; /* dp optimizes the offset-only model */

    toksCap = len + 16;
    toks = malloc(toksCap * sizeof(Tok));
    dpToks = malloc(BLOCK * sizeof(Tok));
    best = malloc((BLOCK + 1) * sizeof(double));
    chain = malloc(BLOCK * sizeof(uint32_t));
    decBuf = malloc(BLOCK);
    bnodes = malloc(BNMAX * sizeof(BNode));
    bhead = malloc((BLOCK + 1) * sizeof(uint32_t));
    bcnt = malloc(BLOCK + 1);
    mc = malloc(BLOCK * sizeof(Match));
    uint8_t *litDump = malloc(len + 16); size_t litCount = 0;
    if (!toks || !dpToks || !best || !chain || !decBuf || !bnodes || !bhead || !bcnt || !mc || !litDump) { fprintf(stderr, "oom\n"); return 1; }

    double totalBits = 0; size_t totalLit = 0, totalMatchB = 0;
    struct timespec t0, t1; clock_gettime(CLOCK_MONOTONIC, &t0);

    size_t nblocks = (len + BLOCK - 1) / BLOCK;
    for (size_t b = 0; b < nblocks; b++) {
        const uint8_t *blk = data + b * BLOCK;
        uint32_t n = (uint32_t)((len - b * BLOCK < BLOCK) ? len - b * BLOCK : BLOCK);
        /* full index + cached matches shared by all parsers */
        memset(hashTab, 0xFF, sizeof(hashTab));
        for (uint32_t i = 0; i + MINM <= n; i++) {
            uint32_t h = h4(blk + i);
            chain[i] = hashTab[h];
            hashTab[h] = i;
        }
        for (uint32_t i = 0; i < n; i++) mc[i] = find_match(blk, n, i);

        ntok = 0;
        if (!strcmp(parser, "greedy")) parse_greedy(blk, n, 0, userep);
        else if (!strcmp(parser, "lazy")) parse_greedy(blk, n, 1, userep);
        else if (!strcmp(parser, "dp")) parse_dp(blk, n);
        else if (!strcmp(parser, "beam4")) parse_beam(blk, n, 4, userep);
        else if (!strcmp(parser, "beam8")) parse_beam(blk, n, 8, userep);
        else if (!strcmp(parser, "beam16")) parse_beam(blk, n, 16, userep);
        else { fprintf(stderr, "unknown parser %s\n", parser); return 1; }

        if (!decode_verify(blk, n)) { fprintf(stderr, "VERIFY FAIL parser=%s block=%zu\n", parser, b); return 1; }

        uint32_t rep = 0;
        for (size_t t = 0; t < ntok; t++) {
            if (toks[t].isMatch) {
                totalMatchB += toks[t].len;
                if (userep && toks[t].off == rep && t > 0) totalBits += repbits_(toks[t].len);
                else totalBits += mbits_(toks[t].off, toks[t].len);
                rep = toks[t].off;
            } else {
                totalBits += 9; totalLit++;
                if (dumpLit && litCount < len) litDump[litCount++] = (uint8_t)toks[t].off;
            }
        }
    }
    clock_gettime(CLOCK_MONOTONIC, &t1);
    double ms = (t1.tv_sec - t0.tv_sec) * 1000.0 + (t1.tv_nsec - t0.tv_nsec) / 1e6;

    if (dumpLit) {
        FILE *df = fopen(dumpLit, "wb");
        if (df) { fwrite(litDump, 1, litCount, df); fclose(df); }
    }
    const char *base = strrchr(file, '/'); base = base ? base + 1 : file;
    printf("%s,%s,%zu,%.0f,%.4f,%.1f,%.2f\n",
           base, parser, len, totalBits / 8.0,
           totalBits / (len * 8.0), ms, len / 1048576.0 / (ms / 1000.0));
    fprintf(stderr, "  tokens_lits=%zu matchbytes=%zu bits/byte=%.5f verify=OK\n",
            totalLit, totalMatchB, totalBits / (len * 8.0));
    return 0;
}
