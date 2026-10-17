/*
 * bzrouter - Experiment C of beat-zstd
 *
 * Question: can a cheap "probe and route" layer (the GPX idea, CPU-only)
 * beat stock zstd at every fixed level on a mixed corpus, by choosing per
 * file: level, dictionary, and a byte-split / delta pre-filter for 16/32-bit
 * binary data?
 *
 * Modes:
 *   probe <file>                      CSV: file,size,h0,h1,shingle
 *   bench <file> [--levels 1,3,...]   CSV: file,level,dict,csize,cspeed,dspeed
 *   route  <file> [--dict D]          decision + verified result
 *   train <dir> <out.dict>            ZDICT_trainFromBuffer on small files
 *   filterbench <file> [level]        plain vs split2/split4/delta, CSV
 *   ftransform/funtransform           filter helpers for external pipelines
 *
 * Routing rules (hand-set, documented in README, not fitted to the corpus):
 *   tiny + dict      -> dict + level 12
 *   incompressible   -> level 1
 *   size >= 8MB      -> level 19
 *   shingle > 0.55   -> level 17
 *   shingle > 0.30   -> level 12
 *   texty (h1 < 6.5) -> level 16
 *   else             -> level 6
 * Filter choice is empirical per file: compress a 128KB sample plain vs
 * split2/split4/delta at level 6, take the smallest (plain wins ties).
 *
 * Build: cc -O2 -o bzrouter bzrouter.c -I<zstd>/lib -L<zstd>/lib -lzstd
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <math.h>
#include <time.h>
#include <dirent.h>
#define ZSTD_STATIC_LINKING_ONLY
#include <zstd.h>
#include <zdict.h>

static double now_ms(void) {
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000.0 + ts.tv_nsec / 1e6;
}

static uint8_t *read_file(const char *path, size_t *len) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END); long sz = ftell(f); fseek(f, 0, SEEK_SET);
    if (sz < 0) { fclose(f); return NULL; }
    uint8_t *buf = malloc((size_t)sz ? (size_t)sz : 1);
    if (sz && fread(buf, 1, (size_t)sz, f) != (size_t)sz) { fclose(f); free(buf); return NULL; }
    fclose(f); *len = (size_t)sz;
    return buf;
}

/* ---------- probe ---------- */
typedef struct { size_t size; double h0, h1; double shingle; } Probe;

static Probe probe_buf(const uint8_t *d, size_t n) {
    Probe p = {0, 0, 0, 0};
    p.size = n;
    /* order-0 + order-1 on up to 256KB */
    size_t sn = n < 262144 ? n : 262144;
    static uint32_t h0c[256];
    static uint32_t h1c[256][256];
    memset(h0c, 0, sizeof(h0c));
    memset(h1c, 0, sizeof(h1c));
    for (size_t i = 0; i < sn; i++) {
        h0c[d[i]]++;
        if (i) h1c[d[i-1]][d[i]]++;
    }
    double H = 0, tot = (double)sn;
    for (int s = 0; s < 256; s++) if (h0c[s]) { double q = h0c[s] / tot; H -= q * log2(q); }
    p.h0 = H;
    if (sn > 1) {
        double H1 = 0, tot1 = (double)(sn - 1);
        for (int a = 0; a < 256; a++) for (int b = 0; b < 256; b++)
            if (h1c[a][b]) { double q = h1c[a][b] / tot1; H1 -= q * log2(q); }
        p.h1 = H1;
    }
    /* 8-byte shingle repeat rate via 21-bit hash map, stride 7
       (2^17 slots had ~14% birthday collisions on 256KB of random data,
       which fooled the incompressible rule; 2^21 brings it under 1%) */
    static uint8_t seen[1 << 21];
    memset(seen, 0, (1 << 21));
    size_t total = 0, rep = 0;
    for (size_t i = 0; i + 8 <= sn; i += 7) {
        uint32_t hh = 0;
        for (int k = 0; k < 8; k++) hh = hh * 16777619u + d[i + k];
        hh = (hh ^ (hh >> 13)) & ((1 << 21) - 1);
        if (seen[hh]) rep++;
        else seen[hh] = 1;
        total++;
    }
    p.shingle = total ? (double)rep / (double)total : 0;
    return p;
}

/* ---------- filters ---------- */
static uint8_t *fwd_split(const uint8_t *d, size_t n, int planes, size_t *outn) {
    uint8_t *o = malloc(n);
    size_t idx = 0;
    size_t cnt[4] = {0};
    for (size_t i = 0; i < n; i++) cnt[i % planes]++;
    size_t off[4]; size_t acc = 0;
    for (int k = 0; k < planes; k++) { off[k] = acc; acc += cnt[k]; }
    for (size_t i = 0; i < n; i++) o[off[i % planes] + (i / planes)] = d[i];
    *outn = n;
    (void)idx;
    return o;
}
static uint8_t *inv_split(const uint8_t *d, size_t n, int planes) {
    uint8_t *o = malloc(n);
    size_t cnt[4] = {0};
    for (size_t i = 0; i < n; i++) cnt[i % planes]++;
    size_t off[4]; size_t acc = 0;
    for (int k = 0; k < planes; k++) { off[k] = acc; acc += cnt[k]; }
    for (size_t i = 0; i < n; i++) o[i] = d[off[i % planes] + (i / planes)];
    return o;
}
static uint8_t *fwd_delta(const uint8_t *d, size_t n) {
    uint8_t *o = malloc(n);
    o[0] = d[0];
    for (size_t i = 1; i < n; i++) o[i] = (uint8_t)(d[i] - d[i-1]);
    return o;
}
static uint8_t *inv_delta(const uint8_t *d, size_t n) {
    uint8_t *o = malloc(n);
    o[0] = d[0];
    for (size_t i = 1; i < n; i++) o[i] = (uint8_t)(d[i] + o[i-1]);
    return o;
}

/* ---------- zstd wrappers ---------- */
static size_t zstd_compress_level(const uint8_t *src, size_t n, int level, uint8_t *dst, size_t dstCap,
                                   const void *dict, size_t dictSize, double *ms) {
    ZSTD_CCtx *c = ZSTD_createCCtx();
    ZSTD_CCtx_setParameter(c, ZSTD_c_compressionLevel, level);
    if (dict && dictSize) ZSTD_CCtx_loadDictionary(c, dict, dictSize);
    size_t r = 0;
    double t0 = now_ms();
    r = ZSTD_compress2(c, dst, dstCap, src, n);
    double t1 = now_ms();
    ZSTD_freeCCtx(c);
    if (ZSTD_isError(r)) { fprintf(stderr, "compress error %s\n", ZSTD_getErrorName(r)); exit(1); }
    if (ms) *ms = t1 - t0;
    return r;
}

static int zstd_roundtrip_level(const uint8_t *src, size_t n, int level, uint8_t *dst, size_t dstCap,
                                const void *dict, size_t dictSize, double *cms, double *dms, size_t *csize) {
    *csize = zstd_compress_level(src, n, level, dst, dstCap, dict, dictSize, cms);
    ZSTD_DCtx *d = ZSTD_createDCtx();
    if (dict && dictSize) ZSTD_DCtx_loadDictionary(d, dict, dictSize);
    uint8_t *back = malloc(n ? n : 1);
    double t0 = now_ms();
    size_t r = ZSTD_decompressDCtx(d, back, n, dst, *csize);
    double t1 = now_ms();
    ZSTD_freeDCtx(d);
    int ok = !ZSTD_isError(r) && r == n && memcmp(back, src, n) == 0;
    free(back);
    if (dms) *dms = t1 - t0;
    return ok;
}

/* ---------- modes ---------- */
static int mode_probe(const char *file) {
    size_t n; uint8_t *d = read_file(file, &n);
    if (!d) { fprintf(stderr, "open %s\n", file); return 1; }
    Probe p = probe_buf(d, n);
    const char *base = strrchr(file, '/'); base = base ? base + 1 : file;
    printf("%s,%zu,%.4f,%.4f,%.4f\n", base, n, p.h0, p.h1, p.shingle);
    free(d);
    return 0;
}

static void parse_levels(const char *s, int *levels, int *nlev) {
    *nlev = 0;
    char tmp[128]; strncpy(tmp, s, 127); tmp[127] = 0;
    char *tok = strtok(tmp, ",");
    while (tok && *nlev < 24) { levels[(*nlev)++] = atoi(tok); tok = strtok(NULL, ","); }
}

static int mode_bench(const char *file, const char *levelsStr, const char *dictPath) {
    size_t n; uint8_t *d = read_file(file, &n);
    if (!d) { fprintf(stderr, "open %s\n", file); return 1; }
    int levels[24], nlev;
    if (levelsStr) parse_levels(levelsStr, levels, &nlev);
    else { int def[] = {1,3,6,9,12,16,19}; memcpy(levels, def, sizeof(def)); nlev = 7; }
    void *dict = NULL; size_t dictSize = 0;
    if (dictPath) { dict = read_file(dictPath, &dictSize); if (!dict) { fprintf(stderr, "dict open %s\n", dictPath); return 1; } }
    size_t cap = ZSTD_compressBound(n);
    uint8_t *dst = malloc(cap);
    const char *base = strrchr(file, '/'); base = base ? base + 1 : file;
    for (int i = 0; i < nlev; i++) {
        int lvl = levels[i];
        int reps = (n < 8388608) ? 2 : 1;
        double bcms = 1e30, bdms = 1e30; size_t cs = 0; int ok = 0;
        for (int r = 0; r < reps; r++) {
            double cms, dms; size_t c2;
            ok = zstd_roundtrip_level(d, n, lvl, dst, cap, dict, dictSize, &cms, &dms, &c2);
            if (cms < bcms) bcms = cms;
            if (dms < bdms) bdms = dms;
            cs = c2;
        }
        if (!ok) { fprintf(stderr, "roundtrip FAIL %s level %d\n", base, lvl); return 1; }
        printf("%s,%d,%s,%zu,%.1f,%.1f,%.1f,%.1f\n", base, lvl, dict ? "dict" : "nodict",
               cs, bcms, n / 1048576.0 / (bcms / 1000.0), bdms, n / 1048576.0 / (bdms / 1000.0));
    }
    free(d); free(dst); free(dict);
    return 0;
}

/* pick filter by compressing a 128KB sample at level 6 */
static int pick_filter(const uint8_t *d, size_t n) {
    if (n < 65536) return 0;
    size_t sn = n < 131072 ? n : 131072;
    size_t cap = ZSTD_compressBound(sn);
    uint8_t *dst = malloc(cap);
    size_t plain = zstd_compress_level(d, sn, 6, dst, cap, NULL, 0, NULL);
    size_t best = plain; int bestf = 0;
    size_t on;
    uint8_t *t = fwd_split(d, sn, 2, &on);
    size_t s2 = zstd_compress_level(t, sn, 6, dst, cap, NULL, 0, NULL);
    if (s2 * 100 < best * 99) { best = s2; bestf = 2; } /* need 1% win to switch */
    free(t);
    t = fwd_split(d, sn, 4, &on);
    size_t s4 = zstd_compress_level(t, sn, 6, dst, cap, NULL, 0, NULL);
    if (s4 * 100 < best * 99) { best = s4; bestf = 4; }
    free(t);
    t = fwd_delta(d, sn);
    size_t sd = zstd_compress_level(t, sn, 6, dst, cap, NULL, 0, NULL);
    if (sd * 100 < best * 99) { best = sd; bestf = 1; }
    free(t);
    free(dst);
    return bestf;
}

/* v2: calibrate the level on a 1MB sample - compress at 6/12/19, take the
 * fastest level within 0.5% of the smallest sample size. This is the GPX
 * probe-and-route idea done with the only honest calibrator available: the
 * codec itself, on a sample. */
static int route_decide_v2(Probe p, const uint8_t *d, size_t n, const void *dict, size_t dictSize) {
    int hasDict = (dict && dictSize);
    if (p.size <= 4096 && hasDict) return 12;
    if (p.h0 > 7.85 && p.shingle < 0.02) return 1;
    size_t sn = n < (1u << 20) ? n : (1u << 20);
    size_t cap = ZSTD_compressBound(sn);
    uint8_t *dst = malloc(cap);
    size_t c6 = zstd_compress_level(d, sn, 6, dst, cap, dict, dictSize, NULL);
    size_t c12 = zstd_compress_level(d, sn, 12, dst, cap, dict, dictSize, NULL);
    size_t c19 = zstd_compress_level(d, sn, 19, dst, cap, dict, dictSize, NULL);
    free(dst);
    size_t mn = c6 < c12 ? c6 : c12; if (c19 < mn) mn = c19;
    if (c6 <= mn + mn / 200) return 6;
    if (c12 <= mn + mn / 200) return 12;
    return 19;
}

static int route_decide(Probe p, int hasDict) {
    if (p.size < 4096 && hasDict) return 12;
    if (p.h0 > 7.85 && p.shingle < 0.02) return 1;
    if (p.size >= (32u << 20)) return 19;
    if (p.size >= (8u << 20)) return 17;
    if (p.shingle > 0.55) return 17;
    if (p.shingle > 0.30) return 12;
    if (p.h1 < 6.5) return 16;
    return 6;
}

static int mode_route(const char *file, const char *dictPath, int v2) {
    size_t n; uint8_t *d = read_file(file, &n);
    if (!d) { fprintf(stderr, "open %s\n", file); return 1; }
    void *dict = NULL; size_t dictSize = 0;
    if (dictPath) { dict = read_file(dictPath, &dictSize); if (!dict) { fprintf(stderr, "dict open %s\n", dictPath); return 1; } }
    Probe p = probe_buf(d, n);
    int level = v2 ? route_decide_v2(p, d, n, dict, dictSize) : route_decide(p, dict != NULL);
    /* filter transform + zstd: pick the filter first, then time the whole
     * pipeline (transform included) so route cspeed is comparable with
     * filterbench and no transform cost is hidden. */
    int filter = pick_filter(d, n);
    double tf0 = now_ms();
    const uint8_t *payload = d; size_t payloadLen = n; uint8_t *ft = NULL;
    if (filter == 2) { size_t on; ft = fwd_split(d, n, 2, &on); payload = ft; payloadLen = on; }
    else if (filter == 4) { size_t on; ft = fwd_split(d, n, 4, &on); payload = ft; payloadLen = on; }
    else if (filter == 1) { ft = fwd_delta(d, n); payload = ft; payloadLen = n; }
    double tf1 = now_ms();
    size_t cap = ZSTD_compressBound(payloadLen) + 1;
    uint8_t *dst = malloc(cap);
    dst[0] = (uint8_t)filter;
    double cms, dms; size_t cs;
    int ok = zstd_roundtrip_level(payload, payloadLen, level, dst + 1, cap - 1, dict, dictSize, &cms, &dms, &cs);
    cms += tf1 - tf0; /* charge the transform to the route's compress time */
    if (!ok) { fprintf(stderr, "route roundtrip FAIL %s\n", file); return 1; }
    /* verify inverse filter via full decode */
    ZSTD_DCtx *dc = ZSTD_createDCtx();
    if (dict) ZSTD_DCtx_loadDictionary(dc, dict, dictSize);
    uint8_t *back = malloc(payloadLen ? payloadLen : 1);
    size_t r = ZSTD_decompressDCtx(dc, back, payloadLen, dst + 1, cs);
    ZSTD_freeDCtx(dc);
    if (ZSTD_isError(r) || r != payloadLen) { fprintf(stderr, "route decode FAIL\n"); return 1; }
    if (filter == 2 || filter == 4) {
        uint8_t *inv = inv_split(back, payloadLen, filter);
        if (memcmp(inv, d, n) != 0) { fprintf(stderr, "route filter VERIFY FAIL\n"); return 1; }
        free(inv);
    } else if (filter == 1) {
        uint8_t *inv = inv_delta(back, payloadLen);
        if (memcmp(inv, d, n) != 0) { fprintf(stderr, "route filter VERIFY FAIL\n"); return 1; }
        free(inv);
    } else if (memcmp(back, d, n) != 0) { fprintf(stderr, "route VERIFY FAIL\n"); return 1; }
    const char *fname = filter == 2 ? "split2" : filter == 4 ? "split4" : filter == 1 ? "delta" : "plain";
    const char *base = strrchr(file, '/'); base = base ? base + 1 : file;
    printf("%s,route%s,%s,%d,%s,%zu,%.1f,%.1f,%.1f,%.1f\n", base, v2 ? "2" : "1", dict ? "dict" : "nodict", level, fname,
           cs + 1, cms, payloadLen / 1048576.0 / (cms / 1000.0), dms, n / 1048576.0 / (dms / 1000.0));
    free(d); free(dst); free(ft); free(back); free(dict);
    return 0;
}

static int mode_train(const char *dir, const char *out) {
    DIR *dp = opendir(dir);
    if (!dp) { fprintf(stderr, "opendir %s\n", dir); return 1; }
    size_t total = 0; const size_t CAP = 24u << 20;
    uint8_t *samples = malloc(CAP);
    size_t *sizes = malloc(200000 * sizeof(size_t));
    size_t ns = 0, nb = 0;
    struct dirent *e;
    char path[4096];
    while ((e = readdir(dp)) != NULL) {
        if (e->d_name[0] == '.') continue;
        snprintf(path, sizeof(path), "%s/%s", dir, e->d_name);
        size_t n; uint8_t *d = read_file(path, &n);
        if (!d) continue;
        if (n >= 2 && n <= 32768 && nb + n <= CAP && ns < 200000) {
            memcpy(samples + nb, d, n);
            nb += n; sizes[ns++] = n;
            total += n;
        }
        free(d);
    }
    closedir(dp);
    if (ns < 100) { fprintf(stderr, "too few samples (%zu)\n", ns); return 1; }
    size_t dictCap = 112640;
    void *dict = malloc(dictCap);
    size_t ds = ZDICT_trainFromBuffer(dict, dictCap, samples, sizes, (unsigned)ns);
    if (ZDICT_isError(ds)) { fprintf(stderr, "train error: %s\n", ZDICT_getErrorName(ds)); return 1; }
    FILE *f = fopen(out, "wb");
    fwrite(dict, 1, ds, f);
    fclose(f);
    fprintf(stderr, "trained dict: %zu bytes from %zu samples (%.1f MB)\n", ds, ns, total / 1048576.0);
    free(samples); free(sizes); free(dict);
    return 0;
}

static int mode_filterbench(const char *file, int level) {
    size_t n; uint8_t *d = read_file(file, &n);
    if (!d) { fprintf(stderr, "open %s\n", file); return 1; }
    size_t cap = ZSTD_compressBound(n);
    uint8_t *dst = malloc(cap);
    const char *base = strrchr(file, '/'); base = base ? base + 1 : file;
    double cms, dms; size_t cs;
    int ok = zstd_roundtrip_level(d, n, level, dst, cap, NULL, 0, &cms, &dms, &cs);
    if (!ok) { fprintf(stderr, "fail plain\n"); return 1; }
    printf("%s,plain,%d,%zu,%.1f,%.1f,%.1f,%.1f\n", base, level, cs, cms, n / 1048576.0 / (cms / 1000.0), dms, n / 1048576.0 / (dms / 1000.0));
    struct { const char *name; int f; } fs[] = { {"delta", 1}, {"split2", 2}, {"split4", 4} };
    for (unsigned i = 0; i < 3; i++) {
        size_t on; uint8_t *t;
        if (fs[i].f == 1) t = fwd_delta(d, n);
        else t = fwd_split(d, n, fs[i].f, &on);
        double tf = now_ms();
        ok = zstd_roundtrip_level(t, n, level, dst, cap, NULL, 0, &cms, &dms, &cs);
        double tf1 = now_ms();
        if (!ok) { fprintf(stderr, "fail %s\n", fs[i].name); return 1; }
        printf("%s,%s,%d,%zu,%.1f,%.1f,%.1f,%.1f\n", base, fs[i].name, level, cs + 1,
               cms + (tf1 - tf), n / 1048576.0 / ((cms + (tf1 - tf)) / 1000.0), dms, n / 1048576.0 / (dms / 1000.0));
        free(t);
    }
    free(d); free(dst);
    return 0;
}

int main(int argc, char **argv) {
    if (argc < 3) {
        fprintf(stderr, "usage: bzrouter probe|bench|route|train|filterbench|ftransform|funtransform ...\n");
        return 1;
    }
    const char *mode = argv[1];
    if (!strcmp(mode, "probe")) return mode_probe(argv[2]);
    if (!strcmp(mode, "bench")) {
        const char *levels = NULL, *dict = NULL;
        for (int i = 3; i < argc; i++) {
            if (!strcmp(argv[i], "--levels") && i + 1 < argc) levels = argv[++i];
            else if (!strcmp(argv[i], "--dict") && i + 1 < argc) dict = argv[++i];
        }
        return mode_bench(argv[2], levels, dict);
    }
    if (!strcmp(mode, "route")) {
        const char *dict = NULL; int v2 = 0;
        for (int i = 3; i < argc; i++) {
            if (!strcmp(argv[i], "--dict") && i + 1 < argc) dict = argv[++i];
            else if (!strcmp(argv[i], "--v2")) v2 = 1;
        }
        return mode_route(argv[2], dict, v2);
    }
    if (!strcmp(mode, "train")) {
        if (argc < 4) { fprintf(stderr, "train <dir> <out>\n"); return 1; }
        return mode_train(argv[2], argv[3]);
    }
    if (!strcmp(mode, "filterbench")) {
        int level = (argc > 3) ? atoi(argv[3]) : 19;
        return mode_filterbench(argv[2], level);
    }
    if (!strcmp(mode, "ftransform") || !strcmp(mode, "funtransform")) {
        if (argc < 5) { fprintf(stderr, "%s <in> <out> <2|4|delta>\n", mode); return 1; }
        size_t n; uint8_t *d = read_file(argv[2], &n);
        if (!d) { fprintf(stderr, "open\n"); return 1; }
        int fwd = !strcmp(mode, "ftransform");
        uint8_t *o;
        if (!strcmp(argv[4], "delta")) o = fwd ? fwd_delta(d, n) : inv_delta(d, n);
        else {
            int pl = atoi(argv[4]);
            if (pl != 2 && pl != 4) { fprintf(stderr, "planes 2 or 4\n"); return 1; }
            o = fwd ? fwd_split(d, n, pl, &n) : inv_split(d, n, pl);
        }
        FILE *f = fopen(argv[3], "wb");
        fwrite(o, 1, n, f); fclose(f);
        return 0;
    }
    fprintf(stderr, "unknown mode %s\n", mode);
    return 1;
}
